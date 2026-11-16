#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace mw::midi
{
    // MIDI system messages are not part of the recordable channel-voice stream.
    // In particular, MIDI Clock, Active Sensing and long SysEx packets must not
    // poison an otherwise valid callback-loop take. A *malformed channel* packet
    // is different: reject it rather than queue an ambiguous recording event.
    enum class MidiLoopCaptureMessageKind
    {
        IgnoreSystem,
        ChannelVoice,
        Invalid
    };

    inline MidiLoopCaptureMessageKind classifyMidiLoopCaptureMessage(
        const std::uint8_t* bytes, int size) noexcept
    {
        if (bytes == nullptr || size <= 0)
            return MidiLoopCaptureMessageKind::Invalid;
        const auto status = bytes[0];
        if (status >= 0xf0U)
            return MidiLoopCaptureMessageKind::IgnoreSystem;
        if (status < 0x80U)
            return MidiLoopCaptureMessageKind::Invalid;
        const auto kind = static_cast<std::uint8_t>(status & 0xf0U);
        const auto expectedSize = (kind == 0xc0U || kind == 0xd0U) ? 2 : 3;
        if (size != expectedSize || (bytes[1] & 0x80U) != 0U
            || (expectedSize == 3 && (bytes[2] & 0x80U) != 0U))
            return MidiLoopCaptureMessageKind::Invalid;
        return MidiLoopCaptureMessageKind::ChannelVoice;
    }

    // All producers and consumers use the same monotonic host clock.  The
    // device-frame estimate is explicitly a *host callback estimate*, not a
    // hardware timestamp.  Failure to calibrate means stop/cancel capture,
    // never assign a note to a guessed pass.
    class MidiLoopCaptureFrameClock final
    {
    public:
        static std::int64_t nowNanoseconds() noexcept
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void invalidate() noexcept
        {
            sequence_.fetch_add(1, std::memory_order_acq_rel);
            sampleRate_.store(0, std::memory_order_relaxed);
            sequence_.fetch_add(1, std::memory_order_release);
        }

        // Audio callback writer. A specified monotonic timestamp permits
        // deterministic clock-skew tests without sleeping or altering production.
        void callbackStartedAt(std::int64_t firstFrame, int sampleRate, int frames,
                               std::int64_t hostNanoseconds) noexcept
        {
            sequence_.fetch_add(1, std::memory_order_acq_rel);
            startNanoseconds_.store(hostNanoseconds, std::memory_order_relaxed);
            firstFrame_.store(firstFrame, std::memory_order_relaxed);
            blockSize_.store(frames, std::memory_order_relaxed);
            sampleRate_.store(sampleRate, std::memory_order_relaxed);
            sequence_.fetch_add(1, std::memory_order_release);
        }

        void callbackStarted(std::int64_t firstFrame, int sampleRate, int frames) noexcept
        {
            callbackStartedAt(firstFrame, sampleRate, frames, nowNanoseconds());
        }

        void callbackCompleted(std::int64_t frame) noexcept
        {
            // Completion must never move backwards in one device epoch.
            // Ignore corrupt/late frame notices; devicePrepared() explicitly
            // starts the next epoch at zero.
            if (frame < 0)
                return;
            auto prior = completedFrame_.load(std::memory_order_relaxed);
            while (frame > prior && !completedFrame_.compare_exchange_weak(
                prior, frame, std::memory_order_release, std::memory_order_relaxed))
            {
            }
        }

        std::int64_t completedFrame() const noexcept
        {
            return completedFrame_.load(std::memory_order_acquire);
        }

        void devicePrepared() noexcept
        {
            invalidate();
            // A new output device starts its physical frame counter at zero.
            // Never compare new input estimates against a prior device's
            // completed-frame watermark.
            completedFrame_.store(0, std::memory_order_release);
            prepareEpoch_.fetch_add(1, std::memory_order_acq_rel);
        }

        std::uint64_t prepareEpoch() const noexcept
        {
            return prepareEpoch_.load(std::memory_order_acquire);
        }

        // Input-MIDI producer thread only. Conservative admission window: an
        // input arriving >2 buffers after last audio callback is not mapped.
        bool estimate(std::int64_t hostNanoseconds, std::int64_t& frame) const noexcept
        {
            const auto before = sequence_.load(std::memory_order_acquire);
            if ((before & 1U) != 0U)
                return false;
            const auto start = startNanoseconds_.load(std::memory_order_relaxed);
            const auto base = firstFrame_.load(std::memory_order_relaxed);
            const auto rate = sampleRate_.load(std::memory_order_relaxed);
            const auto block = blockSize_.load(std::memory_order_relaxed);
            const auto after = sequence_.load(std::memory_order_acquire);
            if (before != after || (after & 1U) != 0U || rate <= 0 || block <= 0
                || base < 0 || hostNanoseconds < start)
                return false;
            // Difference in floating point avoids signed overflow for
            // corrupted/extreme host-clock values; normal steady-clock values
            // remain exact at the millisecond-scale admission window.
            const auto elapsed = static_cast<long double>(hostNanoseconds)
                - static_cast<long double>(start);
            const auto scaled = elapsed
                * static_cast<long double>(rate) / 1000000000.0L;
            if (scaled < 0.0L || scaled > static_cast<long double>(block) * 2.0L)
                return false;
            const auto offset = static_cast<std::int64_t>(std::llround(scaled));
            if (offset > std::numeric_limits<std::int64_t>::max() - base)
                return false;
            const auto estimated = base + offset;
            const auto completed = completedFrame_.load(std::memory_order_acquire);
            // Reject stale/rewound calibration relative to the audio callback's
            // published completion watermark. Allow two full callback buffers
            // of host-input scheduling skew; we cannot claim hardware sync.
            const auto tolerance = static_cast<std::int64_t>(block) * 2;
            if (completed > tolerance && estimated < completed - tolerance)
                return false;
            if (completed > 0
                && completed <= std::numeric_limits<std::int64_t>::max() - tolerance
                && estimated > completed + tolerance)
                return false;
            frame = estimated;
            return true;
        }

        void reset() noexcept
        {
            invalidate();
            completedFrame_.store(0, std::memory_order_release);
            prepareEpoch_.store(0, std::memory_order_release);
        }

    private:
        std::atomic<std::uint64_t> sequence_ { 0 };
        std::atomic<std::int64_t> startNanoseconds_ { 0 };
        std::atomic<std::int64_t> firstFrame_ { 0 };
        std::atomic<int> sampleRate_ { 0 };
        std::atomic<int> blockSize_ { 0 };
        std::atomic<std::int64_t> completedFrame_ { 0 };
        std::atomic<std::uint64_t> prepareEpoch_ { 0 };
    };


    // The control thread seals this gate before Stop/final capture draining.
    // A callback that was already admitted must leave before the final drain;
    // callbacks arriving after seal cannot queue a new MIDI event. A drained
    // queue therefore remains stable. No allocation, mutex, or sleeping in the
    // MIDI producer path. open() requires a quiescent producer.
    class MidiLoopCaptureIngressFence final
    {
    public:
        void open() noexcept { accepting_.store(true, std::memory_order_release); }
        void seal() noexcept { accepting_.store(false, std::memory_order_release); }
        bool tryEnter() noexcept
        {
            inFlight_.fetch_add(1, std::memory_order_acq_rel);
            if (!accepting_.load(std::memory_order_acquire))
            {
                leave();
                return false;
            }
            return true;
        }
        void leave() noexcept { inFlight_.fetch_sub(1, std::memory_order_release); }
        bool quiescent() const noexcept
        {
            return inFlight_.load(std::memory_order_acquire) == 0;
        }
        bool isOpen() const noexcept
        {
            return accepting_.load(std::memory_order_acquire);
        }
    private:
        std::atomic<bool> accepting_ { false };
        std::atomic<unsigned int> inFlight_ { 0 };
    };

    struct MidiLoopCapturedMessage
    {
        std::uint64_t recordingGeneration = 0;
        std::int64_t deviceFrame = -1;
        std::uint8_t status = 0;
        std::uint8_t data1 = 0;
        std::uint8_t data2 = 0;
        std::uint8_t size = 0;
    };

    // Single MIDI-driver producer, single message-thread consumer.  No locks,
    // JUCE objects, heap allocation, or other UI work on push().  Producer
    // overruns permanently poison the current recording generation.
    class MidiLoopCapturedMessageMailbox final
    {
    public:
        static constexpr std::size_t capacity = 512;

        bool push(const MidiLoopCapturedMessage& message) noexcept
        {
            if (failed_.load(std::memory_order_acquire))
                return false;
            const auto write = writeIndex_.load(std::memory_order_relaxed);
            const auto read = readIndex_.load(std::memory_order_acquire);
            if (write - read >= capacity || message.recordingGeneration == 0
                || message.deviceFrame < 0 || message.size < 1 || message.size > 3)
            {
                failed_.store(true, std::memory_order_release);
                return false;
            }
            slots_[write % capacity] = message;
            writeIndex_.store(write + 1U, std::memory_order_release);
            return true;
        }

        bool peek(MidiLoopCapturedMessage& message) const noexcept
        {
            const auto read = readIndex_.load(std::memory_order_relaxed);
            if (read >= writeIndex_.load(std::memory_order_acquire))
                return false;
            message = slots_[read % capacity];
            return true;
        }

        bool pop(MidiLoopCapturedMessage& message) noexcept
        {
            const auto read = readIndex_.load(std::memory_order_relaxed);
            if (read >= writeIndex_.load(std::memory_order_acquire))
                return false;
            message = slots_[read % capacity];
            readIndex_.store(read + 1U, std::memory_order_release);
            return true;
        }

        bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
        bool empty() const noexcept
        {
            return readIndex_.load(std::memory_order_acquire)
                == writeIndex_.load(std::memory_order_acquire);
        }
        void resetWhenStopped() noexcept
        {
            // Both producer and consumer MUST be disabled/quiescent for reset.
            readIndex_.store(0, std::memory_order_release);
            writeIndex_.store(0, std::memory_order_release);
            failed_.store(false, std::memory_order_release);
        }

    private:
        std::array<MidiLoopCapturedMessage, capacity> slots_ {};
        std::atomic<std::uint64_t> readIndex_ { 0 };
        std::atomic<std::uint64_t> writeIndex_ { 0 };
        std::atomic<bool> failed_ { false };
    };

    // Native callback physical-frame contract, shared by message-thread code
    // and deterministic regressions. Pending notices are validated before
    // consumption; only finalized notices advance the expected boundary.
    class MidiLoopCaptureBoundaryFrames final
    {
    public:
        bool arm(std::int64_t firstOutFrame, std::int64_t loopLengthFrames) noexcept
        {
            expectedOut_ = firstOutFrame;
            loopLength_ = loopLengthFrames;
            failed_ = firstOutFrame <= 0 || loopLengthFrames <= 0;
            return !failed_;
        }
        void cancel() noexcept
        {
            expectedOut_ = -1;
            loopLength_ = 0;
            failed_ = false;
        }
        bool validateQueuedWrap(std::int64_t frame, std::size_t pending) const noexcept
        {
            if (failed_ || expectedOut_ <= 0 || loopLength_ <= 0
                || pending > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())
                    / static_cast<std::size_t>(loopLength_))
                return false;
            const auto offset = static_cast<std::int64_t>(pending) * loopLength_;
            return offset <= std::numeric_limits<std::int64_t>::max() - expectedOut_
                && frame == expectedOut_ + offset;
        }
        bool commitWrap(std::int64_t frame) noexcept
        {
            if (!validateQueuedWrap(frame, 0)
                || loopLength_ > std::numeric_limits<std::int64_t>::max() - expectedOut_)
            {
                failed_ = true;
                return false;
            }
            expectedOut_ += loopLength_;
            return true;
        }
        bool isBeforeNextWrap(std::int64_t frame) const noexcept
        {
            return !failed_ && frame >= 0 && frame < expectedOut_;
        }
        bool failed() const noexcept { return failed_; }
        std::int64_t expectedOutFrame() const noexcept { return expectedOut_; }

    private:
        std::int64_t expectedOut_ = -1;
        std::int64_t loopLength_ = 0;
        bool failed_ = false;
    };

    // An event exactly on the physical output-frame wrap belongs to the next
    // pass. The control thread owns both this router and the pass validator.
    class MidiLoopCaptureOrder final
    {
    public:
        void arm(std::uint64_t generation, std::int64_t initialFrame = 0) noexcept
        {
            generation_ = generation;
            lastFrame_ = initialFrame;
            failed_ = generation == 0 || initialFrame < 0;
        }
        void cancel() noexcept { generation_ = 0; lastFrame_ = -1; failed_ = false; }
        bool accept(const MidiLoopCapturedMessage& message) noexcept
        {
            if (failed_ || generation_ == 0
                || message.recordingGeneration != generation_
                || message.deviceFrame < lastFrame_)
            {
                failed_ = true;
                return false;
            }
            lastFrame_ = message.deviceFrame;
            return true;
        }
        bool failed() const noexcept { return failed_; }
        std::int64_t lastFrame() const noexcept { return lastFrame_; }

    private:
        std::uint64_t generation_ = 0;
        std::int64_t lastFrame_ = -1;
        bool failed_ = false;
    };
}

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace mw::playback
{
    // The audio callback owns publish(), and a single control-thread reader owns
    // poll(). No memory allocation, mutex, OS calls or UI work occurs on publish().
    // A reader that falls behind fails closed; missing a wrap must never quietly
    // merge two recording passes.
    struct LoopWrapAcknowledgement
    {
        std::uint64_t serial = 0;
        std::int64_t deviceFrame = 0; // Frames emitted since this callback was activated.
        std::int64_t loopInSample = 0; // Relative to this playback request's start.
    };

    enum class LoopWrapPollResult
    {
        None,
        Event,
        Overrun,
        Invalid
    };

    class LoopWrapAcknowledgementMailbox final
    {
    public:
        static constexpr std::size_t capacity = 64;

        // One realtime producer. A different callback instance is required for a
        // new playback; the mailbox is never cleared while the producer is live.
        void publish(std::int64_t deviceFrame, std::int64_t loopInSample) noexcept
        {
            const auto serial = ++producerSerial_;
            auto& slot = slots_[(serial - 1U) % capacity];
            slot.serial.store(0, std::memory_order_release);
            slot.deviceFrame.store(deviceFrame, std::memory_order_relaxed);
            slot.loopInSample.store(loopInSample, std::memory_order_relaxed);
            slot.serial.store(serial, std::memory_order_release);
            publishedSerial_.store(serial, std::memory_order_release);
        }

        std::uint64_t publishedSerial() const noexcept
        {
            return publishedSerial_.load(std::memory_order_acquire);
        }

        // The cursor must remain owned by one control thread. On Overrun or
        // Invalid the recording controller must STOP, not try to guess passes.
        LoopWrapPollResult poll(std::uint64_t& consumedSerial,
                                LoopWrapAcknowledgement& event) const noexcept
        {
            const auto published = publishedSerial();
            if (published == consumedSerial)
                return LoopWrapPollResult::None;
            if (published < consumedSerial)
                return LoopWrapPollResult::Invalid;
            if (published - consumedSerial > capacity)
                return LoopWrapPollResult::Overrun;
            if (consumedSerial == std::numeric_limits<std::uint64_t>::max())
                return LoopWrapPollResult::Invalid;

            const auto expected = consumedSerial + 1U;
            const auto& slot = slots_[(expected - 1U) % capacity];
            if (slot.serial.load(std::memory_order_acquire) != expected)
                return LoopWrapPollResult::Invalid;
            const auto deviceFrame = slot.deviceFrame.load(std::memory_order_relaxed);
            const auto loopInSample = slot.loopInSample.load(std::memory_order_relaxed);
            if (slot.serial.load(std::memory_order_acquire) != expected
                || deviceFrame < 0 || loopInSample < 0)
                return LoopWrapPollResult::Invalid;
            event = { expected, deviceFrame, loopInSample };
            consumedSerial = expected;
            return LoopWrapPollResult::Event;
        }

    private:
        struct Slot
        {
            std::atomic<std::uint64_t> serial { 0 };
            std::atomic<std::int64_t> deviceFrame { 0 };
            std::atomic<std::int64_t> loopInSample { 0 };
        };
        std::array<Slot, capacity> slots_ {};
        std::uint64_t producerSerial_ = 0; // callback-thread only
        std::atomic<std::uint64_t> publishedSerial_ { 0 };
    };
}

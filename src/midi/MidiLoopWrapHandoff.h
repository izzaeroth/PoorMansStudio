#pragma once

#include "midi/MidiRecordingLoop.h"
#include "playback/LoopWrapAcknowledgement.h"

#include <cstdint>

namespace mw::midi
{
    // Pure control-thread validator: no recording session, GUI, MIDI input or
    // audio-callback state is mutated until the caller commits a completed pass.
    // Recording code must validate the notice, finalize the current pass at Out,
    // and only then call commit().
    class MidiLoopWrapHandoff final
    {
    public:
        void arm(std::uint64_t recordingGeneration,
                 std::uint64_t lastConsumedSerial,
                 std::int64_t loopInSample) noexcept
        {
            recordingGeneration_ = recordingGeneration;
            lastSerial_ = lastConsumedSerial;
            loopInSample_ = loopInSample;
            failed_ = recordingGeneration == 0 || loopInSample < 0;
        }

        void cancel() noexcept
        {
            recordingGeneration_ = 0;
            lastSerial_ = 0;
            loopInSample_ = -1;
            failed_ = false;
        }

        bool validate(const mw::playback::LoopWrapAcknowledgement& notice,
                      const MidiRecordingLoopRunState& run,
                      std::uint64_t generation) noexcept
        {
            if (failed_ || recordingGeneration_ == 0
                || generation != recordingGeneration_ || !run.matches(generation)
                || run.passNumber() <= 0 || run.wrapClaimed()
                || notice.serial != lastSerial_ + 1U
                || notice.loopInSample != loopInSample_
                || notice.deviceFrame < 0)
            {
                failed_ = true;
                return false;
            }
            return true;
        }

        bool commit(const mw::playback::LoopWrapAcknowledgement& notice,
                    MidiRecordingLoopRunState& run,
                    std::uint64_t generation,
                    std::int64_t loopOutTick) noexcept
        {
            if (!validate(notice, run, generation)
                || !run.claimWrap(generation, loopOutTick)
                || !run.beginNextPass(generation))
            {
                failed_ = true;
                return false;
            }
            lastSerial_ = notice.serial;
            return true;
        }

        bool failed() const noexcept { return failed_; }
        std::uint64_t lastSerial() const noexcept { return lastSerial_; }

    private:
        std::uint64_t recordingGeneration_ = 0;
        std::uint64_t lastSerial_ = 0;
        std::int64_t loopInSample_ = -1;
        bool failed_ = false;
    };
}

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mw::playback
{
    // A callback reports only a stable numeric code. The control thread formats text.
    enum class RealtimeAudioFault : std::uint8_t
    {
        None, InvalidBlock, InvalidSlice, SliceBudget, InstrumentBusy,
        InstrumentScheduler, InstrumentProcess, InstrumentOutput,
        EffectBusy, EffectProcess, EffectOutput, AudioSourceOutput
    };

    constexpr const char* realtimeFaultText(RealtimeAudioFault code) noexcept
    {
        switch (code)
        {
            case RealtimeAudioFault::None: return "";
            case RealtimeAudioFault::InvalidBlock: return "Mixed preview received an unsupported audio callback block size.";
            case RealtimeAudioFault::InvalidSlice: return "Mixed preview loop coordinator returned an invalid callback slice.";
            case RealtimeAudioFault::SliceBudget: return "Mixed preview loop exceeded its realtime slice budget; increase the loop length.";
            case RealtimeAudioFault::InstrumentBusy: return "Mixed preview instrument remained busy too long.";
            case RealtimeAudioFault::InstrumentScheduler: return "Mixed preview plugin scheduler exceeded the callback slice.";
            case RealtimeAudioFault::InstrumentProcess: return "Mixed preview plugin instrument processing failed.";
            case RealtimeAudioFault::InstrumentOutput: return "Mixed preview plugin instrument returned invalid audio.";
            case RealtimeAudioFault::EffectBusy: return "Mixed preview effect was busy; the complete wet chain was stopped without partial output.";
            case RealtimeAudioFault::EffectProcess: return "Mixed preview effect failed; no partial wet effect chain was delivered.";
            case RealtimeAudioFault::EffectOutput: return "Mixed preview effect returned invalid audio.";
            case RealtimeAudioFault::AudioSourceOutput: return "Mixed preview rendered source had invalid audio dimensions.";
        }
        return "Mixed preview audio callback failed.";
    }

    class RealtimeAudioFaultLatch
    {
    public:
        void reset() noexcept { fault_.store(RealtimeAudioFault::None, std::memory_order_release); }
        void publish(RealtimeAudioFault fault) noexcept
        {
            if (fault == RealtimeAudioFault::None) return;
            auto empty = RealtimeAudioFault::None;
            fault_.compare_exchange_strong(empty, fault, std::memory_order_acq_rel);
        }
        RealtimeAudioFault read() const noexcept { return fault_.load(std::memory_order_acquire); }
    private:
        std::atomic<RealtimeAudioFault> fault_ { RealtimeAudioFault::None };
    };

    // Dedicated direct-preview faults are published from device callbacks as numbers.
    // Converting to diagnostic text is reserved for the polling/control thread.
    enum class DirectPreviewFault : std::uint8_t
    {
        None, VstInvalidBlock, VstBusy, VstScheduler, VstInstrument,
        VstEffectMissing, VstEffectBusy, VstEffect,
        ClapBusy, ClapInstrument, ClapInstrumentOutput,
        ClapEffectMissing, ClapEffectBusy, ClapEffect, ClapEffectOutput,
        ClapAudioSourceOutput
    };

    constexpr const char* directPreviewFaultText(DirectPreviewFault code) noexcept
    {
        switch (code)
        {
            case DirectPreviewFault::None: return "";
            case DirectPreviewFault::VstInvalidBlock: return "VST3 direct preview received an invalid audio callback block.";
            case DirectPreviewFault::VstBusy: return "VST3 direct preview instrument stayed busy; playback stopped.";
            case DirectPreviewFault::VstScheduler: return "VST3 direct preview scheduler exceeded callback frames.";
            case DirectPreviewFault::VstInstrument: return "VST3 direct preview instrument processing failed.";
            case DirectPreviewFault::VstEffectMissing: return "VST3 direct preview effect session was unavailable; no partial wet chain was delivered.";
            case DirectPreviewFault::VstEffectBusy: return "VST3 direct preview effect was busy; no partial wet chain was delivered.";
            case DirectPreviewFault::VstEffect: return "VST3 direct preview effect processing failed; no partial wet chain was delivered.";
            case DirectPreviewFault::ClapBusy: return "CLAP direct preview instrument stayed busy; playback stopped.";
            case DirectPreviewFault::ClapInstrument: return "CLAP direct preview instrument processing failed.";
            case DirectPreviewFault::ClapInstrumentOutput: return "CLAP direct preview instrument returned invalid audio.";
            case DirectPreviewFault::ClapEffectMissing: return "CLAP direct preview effect session was unavailable; no partial wet chain was delivered.";
            case DirectPreviewFault::ClapEffectBusy: return "CLAP direct preview effect was busy; no partial wet chain was delivered.";
            case DirectPreviewFault::ClapEffect: return "CLAP direct preview effect processing failed; no partial wet chain was delivered.";
            case DirectPreviewFault::ClapEffectOutput: return "CLAP direct preview effect returned invalid audio.";
            case DirectPreviewFault::ClapAudioSourceOutput: return "CLAP direct preview rendered audio had invalid dimensions.";
        }
        return "Direct preview callback failed.";
    }

    class DirectPreviewFaultLatch
    {
    public:
        void reset() noexcept { fault_.store(DirectPreviewFault::None, std::memory_order_release); }
        void publish(DirectPreviewFault code) noexcept
        {
            if (code == DirectPreviewFault::None) return;
            auto expected = DirectPreviewFault::None;
            fault_.compare_exchange_strong(expected, code, std::memory_order_acq_rel);
        }
        DirectPreviewFault read() const noexcept { return fault_.load(std::memory_order_acquire); }
    private:
        std::atomic<DirectPreviewFault> fault_ { DirectPreviewFault::None };
    };

    constexpr bool realtimeCallbackEventCountAllows(std::size_t count, std::size_t reserved) noexcept
    {
        return count <= reserved;
    }

    // Fail closed: neither an unavailable slot nor a failed plugin may become
    // an implicit partial effect chain. All previously mixed output is cleared.
    enum class EffectChainAction : std::uint8_t { Process, FailClosed };
    constexpr EffectChainAction effectChainAction(bool sessionReady, bool mutexAcquired) noexcept
    {
        return sessionReady && mutexAcquired ? EffectChainAction::Process : EffectChainAction::FailClosed;
    }

    struct RealtimeEventSortKey
    {
        std::uint32_t frameOffset = 0;
        int priority = 0;
        std::size_t stableOrder = 0;
    };

    constexpr bool realtimeEventLess(RealtimeEventSortKey a, RealtimeEventSortKey b) noexcept
    {
        if (a.frameOffset != b.frameOffset) return a.frameOffset < b.frameOffset;
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.stableOrder < b.stableOrder;
    }

    // The realtime CLAP path may only append into event storage reserved beforehand.
    constexpr bool realtimeEventCapacityAllows(std::size_t reserved, std::size_t required) noexcept
    {
        return required <= reserved;
    }
}

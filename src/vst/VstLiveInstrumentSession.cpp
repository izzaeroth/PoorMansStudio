#include "vst/VstLiveInstrumentSession.h"

#include "vst/VstInstrumentHost.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace mw::vst
{
    namespace
    {
        constexpr int kMaximumLiveChannels = 64;
    }

    struct VstLiveInstrumentSession::Impl
    {
        VstLiveInstrumentSessionInfo info;
        std::unique_ptr<juce::AudioPluginInstance> instance;
    };

    VstLiveInstrumentSession::VstLiveInstrumentSession()
        : impl_(std::make_unique<Impl>())
    {
    }

    VstLiveInstrumentSession::~VstLiveInstrumentSession()
    {
        close();
    }

    bool VstLiveInstrumentSession::open(const VstLiveInstrumentSessionConfig& config,
                                        std::string& errorMessage)
    {
        close();

        if (!config.plugin.hasPluginIdentity())
        {
            errorMessage = "VST3 live instrument assignment is empty.";
            return false;
        }

        const auto sampleRate = std::clamp(config.sampleRate, 8000.0, 384000.0);
        const auto channelCount = std::clamp(config.channelCount > 0 ? config.channelCount : 2, 1, kMaximumLiveChannels);
        const auto blockSize = std::clamp(config.blockSize > 0 ? config.blockSize : 512, 16, 8192);

        auto load = VstInstrumentHost::loadPluginAssignment(
            config.plugin,
            sampleRate,
            blockSize,
            0,
            channelCount);

        if (!load.success || load.instance == nullptr)
        {
            errorMessage = load.message.empty() ? "VST3 live instrument failed to load." : load.message;
            return false;
        }

        const auto reportedOutputs = load.instance->getTotalNumOutputChannels();
        if (reportedOutputs <= 0 || reportedOutputs > channelCount || reportedOutputs > kMaximumLiveChannels)
        {
            load.instance->releaseResources();
            errorMessage = "VST3 live instrument reported an unsupported output-channel count.";
            return false;
        }

        impl_->instance = std::move(load.instance);
        impl_->info.open = true;
        impl_->info.pluginName = config.plugin.name.empty()
            ? config.plugin.bundlePath.stem().string()
            : config.plugin.name;
        impl_->info.sampleRate = sampleRate;
        impl_->info.channelCount = channelCount;
        impl_->info.blockSize = blockSize;
        impl_->info.stateRestored = load.savedStateApplied;
        impl_->info.reportedInputChannels = impl_->instance->getTotalNumInputChannels();
        impl_->info.reportedOutputChannels = reportedOutputs;
        impl_->info.latencySamples = std::max(0, impl_->instance->getLatencySamples());
        impl_->info.message = load.message;
        if (!load.savedStateMessage.empty())
            impl_->info.message += " " + load.savedStateMessage;

        errorMessage.clear();
        return true;
    }

    void VstLiveInstrumentSession::close()
    {
        if (impl_ == nullptr)
            return;

        if (impl_->instance != nullptr)
        {
            try
            {
                impl_->instance->suspendProcessing(true);
                impl_->instance->releaseResources();
            }
            catch (...)
            {
            }
            impl_->instance.reset();
        }

        impl_->info = {};
    }

    bool VstLiveInstrumentSession::isOpen() const noexcept
    {
        return impl_ != nullptr && impl_->instance != nullptr && impl_->info.open;
    }

    VstLiveInstrumentSessionInfo VstLiveInstrumentSession::info() const
    {
        return impl_ != nullptr ? impl_->info : VstLiveInstrumentSessionInfo{};
    }

    bool VstLiveInstrumentSession::prepareForPlayback(double sampleRate,
                                                       int blockSize,
                                                       int channelCount,
                                                       std::string& errorMessage) noexcept
    {
        if (!isOpen())
        {
            errorMessage = "VST3 live instrument session is not open.";
            return false;
        }

        const auto safeSampleRate = std::clamp(sampleRate, 8000.0, 384000.0);
        const auto safeBlockSize = std::clamp(blockSize > 0 ? blockSize : 512, 16, 8192);
        const auto safeChannels = std::clamp(channelCount > 0 ? channelCount : 2, 1, kMaximumLiveChannels);

        try
        {
            impl_->instance->suspendProcessing(true);
            impl_->instance->releaseResources();
            impl_->instance->setPlayConfigDetails(0, safeChannels, safeSampleRate, safeBlockSize);
            impl_->instance->prepareToPlay(safeSampleRate, safeBlockSize);
            impl_->instance->reset();
            impl_->instance->suspendProcessing(false);

            const auto outputs = impl_->instance->getTotalNumOutputChannels();
            if (outputs <= 0 || outputs > safeChannels || outputs > kMaximumLiveChannels)
            {
                errorMessage = "VST3 live instrument reported an unsupported output-channel count after device preparation.";
                return false;
            }

            impl_->info.sampleRate = safeSampleRate;
            impl_->info.blockSize = safeBlockSize;
            impl_->info.channelCount = safeChannels;
            impl_->info.reportedInputChannels = impl_->instance->getTotalNumInputChannels();
            impl_->info.reportedOutputChannels = outputs;
            impl_->info.latencySamples = std::max(0, impl_->instance->getLatencySamples());
            errorMessage.clear();
            return true;
        }
        catch (const std::exception& ex)
        {
            try { impl_->instance->suspendProcessing(false); } catch (...) {}
            errorMessage = std::string("VST3 live instrument device preparation failed: ") + ex.what();
        }
        catch (...)
        {
            try { impl_->instance->suspendProcessing(false); } catch (...) {}
            errorMessage = "VST3 live instrument device preparation failed.";
        }

        return false;
    }

    bool VstLiveInstrumentSession::processBlock(juce::AudioBuffer<float>& audio,
                                                juce::MidiBuffer& midi,
                                                std::string& errorMessage) noexcept
    {
        if (!isOpen())
        {
            errorMessage = "VST3 live instrument session is not open.";
            return false;
        }

        if (audio.getNumChannels() <= 0
            || audio.getNumSamples() <= 0
            || audio.getNumChannels() < impl_->info.reportedOutputChannels)
        {
            errorMessage = "VST3 live instrument received an invalid audio buffer.";
            return false;
        }

        try
        {
            impl_->instance->processBlock(audio, midi);
            errorMessage.clear();
            return true;
        }
        catch (const std::exception& ex)
        {
            errorMessage = std::string("VST3 live instrument processing failed: ") + ex.what();
        }
        catch (...)
        {
            errorMessage = "VST3 live instrument processing failed.";
        }

        return false;
    }
}

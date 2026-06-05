#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace mw::gui
{
    inline std::atomic_bool helperBubblesGloballyEnabled { true };

    inline void setHelperBubblesGloballyEnabled(bool enabled)
    {
        helperBubblesGloballyEnabled.store(enabled);
    }

    inline bool areHelperBubblesGloballyEnabled()
    {
        return helperBubblesGloballyEnabled.load();
    }

    class FreshHoverTooltipWindow final : public juce::TooltipWindow
    {
    public:
        FreshHoverTooltipWindow(juce::Component* parentComponent, int newHoverDelayMs)
            : juce::TooltipWindow(parentComponent, 50),
              hoverDelayMs(newHoverDelayMs)
        {
        }

        juce::String getTipFor(juce::Component& component) override
        {
            if (!areHelperBubblesGloballyEnabled())
            {
                lastTipComponent = nullptr;
                lastTipText = {};
                hoverStartedMs = juce::Time::getMillisecondCounter();
                return {};
            }

            const auto tip = juce::TooltipWindow::getTipFor(component);
            const auto now = juce::Time::getMillisecondCounter();

            if (tip.isEmpty())
            {
                lastTipComponent = nullptr;
                lastTipText = {};
                hoverStartedMs = now;
                return {};
            }

            if (lastTipComponent != &component || lastTipText != tip)
            {
                lastTipComponent = &component;
                lastTipText = tip;
                hoverStartedMs = now;
                return {};
            }

            if (static_cast<int>(now - hoverStartedMs) < hoverDelayMs)
                return {};

            return tip;
        }

    private:
        int hoverDelayMs = 2000;
        juce::Component* lastTipComponent = nullptr;
        juce::String lastTipText;
        uint32_t hoverStartedMs = 0;
    };

    class HelperTooltipLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        void drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) override
        {
            g.fillAll(juce::Colours::transparentBlack);

            auto borderArea = juce::Rectangle<float>(0.5f, 0.5f,
                                                     static_cast<float>(std::max(1, width)) - 1.0f,
                                                     static_cast<float>(std::max(1, height)) - 1.0f);
            auto fillArea = borderArea.reduced(1.5f);

            g.setColour(juce::Colours::black.withAlpha(0.98f));
            g.fillRoundedRectangle(borderArea, 6.0f);
            g.setColour(juce::Colour(0xffffe066));
            g.fillRoundedRectangle(fillArea, 5.0f);
            g.setColour(juce::Colours::black.withAlpha(0.95f));
            g.drawRoundedRectangle(fillArea, 5.0f, 1.0f);
            g.setColour(juce::Colours::black);
            g.setFont(juce::Font(14.0f, juce::Font::plain));
            g.drawFittedText(text, juce::Rectangle<int>(width, height).reduced(11, 8), juce::Justification::centredLeft, 4);
        }

        juce::Rectangle<int> getTooltipBounds(const juce::String& text, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override
        {
            const auto maxWidth = std::min(420, std::max(220, parentArea.getWidth() - 24));
            const int estimatedTextWidth = 18 + static_cast<int>(text.length()) * 8;
            const int textWidth = juce::jlimit(180, maxWidth, estimatedTextWidth);
            const int lines = std::max(1, 1 + static_cast<int>(text.length()) / 54);
            const auto height = juce::jlimit(38, 104, 22 + lines * 18);

            auto bounds = juce::Rectangle<int>(textWidth, height).withPosition(screenPos.x + 14, screenPos.y + 20);

            if (bounds.getRight() > parentArea.getRight())
                bounds.setX(parentArea.getRight() - bounds.getWidth() - 6);

            if (bounds.getBottom() > parentArea.getBottom())
                bounds.setY(screenPos.y - bounds.getHeight() - 12);

            if (bounds.getX() < parentArea.getX())
                bounds.setX(parentArea.getX() + 6);

            if (bounds.getY() < parentArea.getY())
                bounds.setY(parentArea.getY() + 6);

            return bounds;
        }
    };
}

#include <juce_gui_extra/juce_gui_extra.h>

#include "app/AppVersion.h"
#include <functional>
#include "BinaryData.h"

#include "gui/MainComponent.h"

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
    bool closeApprovedForQuit = false;


    void maximiseWindowForPoorMansStudio(juce::DocumentWindow& window)
    {
#if JUCE_WINDOWS
        if (auto* peer = window.getPeer())
        {
            if (auto nativeHandle = peer->getNativeHandle())
            {
                ShowWindow(static_cast<HWND>(nativeHandle), SW_MAXIMIZE);
                return;
            }
        }
#endif

        window.setBounds(juce::Desktop::getInstance().getDisplays().getMainDisplay().userArea);
    }

    juce::Image makePoorMansStudioMainIcon()
    {
        auto icon = juce::ImageFileFormat::loadFrom(
            BinaryData::app_icon_512_png,
            BinaryData::app_icon_512_pngSize
        );

        if (icon.isValid())
            return icon;

        juce::Image fallback(juce::Image::ARGB, 128, 128, true);
        juce::Graphics g(fallback);
        g.fillAll(juce::Colours::transparentBlack);
        g.setColour(juce::Colour(0xffffd447));
        g.fillEllipse(40.0f, 76.0f, 32.0f, 22.0f);
        g.drawLine(68.0f, 34.0f, 68.0f, 88.0f, 8.0f);
        return fallback;
    }

    class PoorMansStudioMainTitleBarButton final : public juce::Button,
                                                      private juce::Timer
    {
    public:
        enum class Kind
        {
            Minimise,
            Maximise,
            Close
        };

        PoorMansStudioMainTitleBarButton(juce::String symbolIn, juce::Colour colourIn, Kind kindIn)
            : juce::Button(symbolIn), symbol(std::move(symbolIn)), colour(colourIn), kind(kindIn)
        {
            setTriggeredOnMouseDown(false);
            setWantsKeyboardFocus(false);
            setMouseCursor(juce::MouseCursor::NormalCursor);

            if (kind == Kind::Maximise)
                startTimerHz(4);
        }

        void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
        {
            auto area = getLocalBounds().toFloat();
            if (shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
            {
                g.setColour(colour.withAlpha(shouldDrawButtonAsDown ? 0.22f : 0.12f));
                g.fillRoundedRectangle(area.reduced(4.0f, 5.0f), 5.0f);
            }

            g.setColour(colour);

            if (kind == Kind::Maximise && shouldDrawRestoreSymbol())
            {
                const auto glyph = area.withSizeKeepingCentre(10.0f, 10.0f);
                g.fillRoundedRectangle(glyph, 1.8f);
                return;
            }

            const auto cx = area.getCentreX();
            const auto cy = area.getCentreY();
            const float thickness = (kind == Kind::Close ? 2.8f : 2.6f);

            if (kind == Kind::Minimise)
            {
                g.fillRoundedRectangle(cx - 5.8f, cy + 2.6f, 11.6f, thickness, thickness * 0.5f);
                return;
            }

            if (kind == Kind::Maximise)
            {
                g.fillRoundedRectangle(cx - 5.6f, cy - 1.3f, 11.2f, thickness, thickness * 0.5f);
                g.fillRoundedRectangle(cx - (thickness * 0.5f), cy - 5.6f, thickness, 11.2f, thickness * 0.5f);
                return;
            }

            juce::Path cross;
            cross.startNewSubPath(cx - 5.2f, cy - 5.2f);
            cross.lineTo(cx + 5.2f, cy + 5.2f);
            cross.startNewSubPath(cx + 5.2f, cy - 5.2f);
            cross.lineTo(cx - 5.2f, cy + 5.2f);
            g.strokePath(cross, juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

    private:
        void timerCallback() override
        {
            repaint();
        }

        bool shouldDrawRestoreSymbol() const
        {
            if (auto* window = dynamic_cast<juce::DocumentWindow*>(getTopLevelComponent()))
            {
#if JUCE_WINDOWS
                if (auto* peer = window->getPeer())
                {
                    if (auto nativeHandle = peer->getNativeHandle())
                        return IsZoomed(static_cast<HWND>(nativeHandle)) != FALSE;
                }
#endif
                return window->isFullScreen();
            }

            return false;
        }

        juce::String symbol;
        juce::Colour colour;
        Kind kind;
    };

    class PoorMansStudioMainWindowLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        juce::Button* createDocumentWindowButton(int buttonType) override
        {
            if (buttonType == juce::DocumentWindow::minimiseButton)
                return new PoorMansStudioMainTitleBarButton("-", juce::Colour(0xff2d8eff), PoorMansStudioMainTitleBarButton::Kind::Minimise);

            if (buttonType == juce::DocumentWindow::maximiseButton)
                return new PoorMansStudioMainTitleBarButton("+", juce::Colour(0xff2fbe58), PoorMansStudioMainTitleBarButton::Kind::Maximise);

            if (buttonType == juce::DocumentWindow::closeButton)
                return new PoorMansStudioMainTitleBarButton("x", juce::Colour(0xffe13c46), PoorMansStudioMainTitleBarButton::Kind::Close);

            return juce::LookAndFeel_V4::createDocumentWindowButton(buttonType);
        }

        void drawDocumentWindowTitleBar(juce::DocumentWindow& window,
                                        juce::Graphics& g,
                                        int w,
                                        int h,
                                        int titleSpaceX,
                                        int titleSpaceW,
                                        const juce::Image* icon,
                                        bool /*drawTitleTextOnLeft*/) override
        {
            g.fillAll(juce::Colour(0xffd1d5db));
            g.setColour(juce::Colour(0xffaeb5bf));
            g.drawRect(0, 0, w, h, 1);
            g.setColour(juce::Colour(0xff9fa7b2));
            g.drawLine(0.0f, static_cast<float>(h - 1), static_cast<float>(w), static_cast<float>(h - 1), 1.0f);

            int titleX = 12;
            if (icon != nullptr && icon->isValid())
            {
                // The main app icon can use a little more of the custom 32 px title bar
                // without making the bar taller. 26 px gives it more readable detail
                // while still leaving a clean 3 px top/bottom margin.
                const int iconSize = juce::jmin(26, juce::jmax(20, h - 6));
                const int iconY = (h - iconSize) / 2;
                g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
                g.drawImageWithin(*icon, titleX, iconY, iconSize, iconSize, juce::RectanglePlacement::centred);
                titleX += iconSize + 12;
            }

            const int rightLimit = juce::jmax(titleX, titleSpaceX + titleSpaceW);
            auto titleBounds = juce::Rectangle<int>(titleX, 0, juce::jmax(0, rightLimit - titleX), h).reduced(0, 1);
            g.setColour(juce::Colour(0xff202124));
            g.setFont(juce::Font(13.5f, juce::Font::plain));
            g.drawText(window.getName(), titleBounds, juce::Justification::centredLeft, true);
        }
    };

    PoorMansStudioMainWindowLookAndFeel& getPoorMansStudioMainWindowLookAndFeel()
    {
        static PoorMansStudioMainWindowLookAndFeel lookAndFeel;
        return lookAndFeel;
    }

    void applyPoorMansStudioMainCustomTitleBar(juce::DocumentWindow& window)
    {
        window.setLookAndFeel(&getPoorMansStudioMainWindowLookAndFeel());
        window.setUsingNativeTitleBar(false);
        window.setTitleBarHeight(32);
    }

    void runAfterMainMouseButtonsReleased(std::function<void()> callback, int attempts = 0)
    {
        if (juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown() && attempts < 200)
        {
            juce::Timer::callAfterDelay(20, [callback = std::move(callback), attempts]() mutable
            {
                runAfterMainMouseButtonsReleased(std::move(callback), attempts + 1);
            });
            return;
        }

        juce::Timer::callAfterDelay(1, [callback = std::move(callback)]() mutable
        {
            if (callback)
                callback();
        });
    }
}


class SplashContent final : public juce::Component
{
public:
    SplashContent()
    {
        splashImage = juce::ImageFileFormat::loadFrom(
            BinaryData::splash_static_png,
            BinaryData::splash_static_pngSize
        );
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff050505));

        auto area = getLocalBounds().toFloat();

        if (splashImage.isValid())
        {
            const auto imageBounds = splashImage.getBounds().toFloat();
            const auto scale = std::min(
                area.getWidth() / imageBounds.getWidth(),
                area.getHeight() / imageBounds.getHeight()
            );

            const auto width = imageBounds.getWidth() * scale;
            const auto height = imageBounds.getHeight() * scale;
            const auto target = juce::Rectangle<float>(
                area.getCentreX() - width * 0.5f,
                area.getCentreY() - height * 0.5f,
                width,
                height
            );

            g.drawImage(splashImage, target);
        }
        else
        {
            g.setColour(juce::Colours::white);
            g.setFont(juce::FontOptions(32.0f, juce::Font::bold));
            g.drawFittedText("Poor Man's Studio", getLocalBounds(), juce::Justification::centred, 1);
        }
    }

private:
    juce::Image splashImage;
};

class SplashWindow final : public juce::DocumentWindow
{
public:
    SplashWindow()
        : juce::DocumentWindow(
            "Poor Man's Studio",
            juce::Colours::black,
            0
        )
    {
        setUsingNativeTitleBar(false);
        setResizable(false, false);
        setContentOwned(new SplashContent(), true);
        centreWithSize(960, 540);
        setVisible(true);

        if (auto* peer = getPeer())
            peer->setIcon(makePoorMansStudioMainIcon());
    }

    void closeButtonPressed() override {}
};


class MusicWorkstationApplication final : public juce::JUCEApplication,
                                         private juce::Timer
{
private:
    class MainWindow;

public:
    const juce::String getApplicationName() override
    {
        return "Poor Man's Studio";
    }

    const juce::String getApplicationVersion() override
    {
        return mw::app::appVersion;
    }

    bool moreThanOneInstanceAllowed() override
    {
        return true;
    }

    void initialise(const juce::String&) override
    {
        splashWindow = std::make_unique<SplashWindow>();
        startTimer(3000);
    }

    void shutdown() override
    {
        stopTimer();
        splashWindow = nullptr;
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override;

    void anotherInstanceStarted(const juce::String&) override
    {
    }

    void timerCallback() override;

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow(juce::String name)
            : DocumentWindow(
                name,
                juce::Desktop::getInstance().getDefaultLookAndFeel()
                    .findColour(juce::ResizableWindow::backgroundColourId),
                DocumentWindow::allButtons
            )
        {
            const auto mainIcon = makePoorMansStudioMainIcon();
            setIcon(mainIcon);
            applyPoorMansStudioMainCustomTitleBar(*this);
            setContentOwned(new mw::gui::MainComponent(), true);
            setResizable(true, true);
            centreWithSize(1250, 820);
            setVisible(true);
            applyPoorMansStudioMainCustomTitleBar(*this);

            if (auto* peer = getPeer())
                peer->setIcon(mainIcon);

            maximiseWindowForPoorMansStudio(*this);
        }

        ~MainWindow() override
        {
            setLookAndFeel(nullptr);
        }

        void closeButtonPressed() override
        {
            if (closeRequestPending)
                return;

            closeRequestPending = true;
            juce::Component::SafePointer<MainWindow> safeThis(this);

            // Let the custom title-bar X mouse press fully release before any dirty-project
            // confirmation appears. If an alert opens under a still-held cursor, moving
            // toward its buttons can drag the alert window instead.
            runAfterMainMouseButtonsReleased([safeThis]
            {
                if (safeThis == nullptr)
                    return;

                safeThis->closeRequestPending = false;
                safeThis->requestClose();
            });
        }

        void requestClose()
        {
            auto quitApp = []
            {
                closeApprovedForQuit = true;
                juce::JUCEApplication::getInstance()->quit();
            };

            if (auto* mainComponent = dynamic_cast<mw::gui::MainComponent*>(getContentComponent()))
            {
                mainComponent->requestCloseWithSaveAsPrompt(quitApp);
                return;
            }

            quitApp();
        }

    private:
        bool closeRequestPending = false;
    };

    std::unique_ptr<SplashWindow> splashWindow;
    std::unique_ptr<MainWindow> mainWindow;
};


void MusicWorkstationApplication::systemRequestedQuit()
{
    if (closeApprovedForQuit)
    {
        quit();
        return;
    }

    if (mainWindow != nullptr)
    {
        mainWindow->requestClose();
        return;
    }

    quit();
}

void MusicWorkstationApplication::timerCallback()
{
    stopTimer();
    splashWindow = nullptr;
    mainWindow = std::make_unique<MainWindow>(getApplicationName());
}

START_JUCE_APPLICATION(MusicWorkstationApplication)

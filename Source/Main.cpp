/*
  ==============================================================================
    Main.cpp
    Pink Noise Audio Monitor (pnam) — application entry point.

    Hosts pnam::MainComponent in a single resizable DocumentWindow. All audio,
    analysis, logging, and UI wiring live inside MainComponent and the classes
    it owns; this file only handles JUCE application lifecycle and window
    chrome.
  ==============================================================================
*/

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "UI/MainComponent.h"
#include "UI/DarkLookAndFeel.h"

namespace pnam
{

//==============================================================================
class PinkNoiseAudioMonitorWindow : public juce::DocumentWindow
{
public:
    PinkNoiseAudioMonitorWindow (const juce::String& name)
        : juce::DocumentWindow (name,
                                pnam::Palette::get (pnam::Palette::windowBackground),
                                juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, true);
        setResizeLimits (900, 600, 10000, 10000);

        setContentOwned (new MainComponent(), false);

       #if JUCE_IOS || JUCE_ANDROID
        setFullScreen (true);
       #else
        centreWithSize (1120, 720);
       #endif

        setVisible (true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PinkNoiseAudioMonitorWindow)
};

//==============================================================================
class PinkNoiseAudioMonitorApplication : public juce::JUCEApplication
{
public:
    PinkNoiseAudioMonitorApplication() = default;

    const juce::String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override     { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override               { return true; }

    void initialise (const juce::String&) override
    {
        mainWindow.reset (new PinkNoiseAudioMonitorWindow (getApplicationName()));
    }

    void shutdown() override
    {
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override
    {
        // Single-window app: bring the existing window to the front.
        if (mainWindow != nullptr)
        {
            mainWindow->toFront (true);
        }
    }

private:
    std::unique_ptr<PinkNoiseAudioMonitorWindow> mainWindow;
};

} // namespace pnam

//==============================================================================
START_JUCE_APPLICATION (pnam::PinkNoiseAudioMonitorApplication)

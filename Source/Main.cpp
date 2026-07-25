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
/** Command IDs for the macOS main menu. Preferences is an application-menu item
    (Cmd-,) rather than a button in the transport bar. */
namespace CommandIDs
{
    enum
    {
        showPreferences = 0x2000,
        showAudioDeviceSetup
    };
}

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

        auto* content = new MainComponent();
        mainComponent = content;
        setContentOwned (content, false);

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

    /** The hosted MainComponent, or nullptr once it has been torn down. */
    MainComponent* getMainComponent() const noexcept { return mainComponent.getComponent(); }

private:
    juce::Component::SafePointer<MainComponent> mainComponent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PinkNoiseAudioMonitorWindow)
};

//==============================================================================
class PinkNoiseAudioMonitorApplication : public juce::JUCEApplication,
                                          public juce::MenuBarModel
{
public:
    PinkNoiseAudioMonitorApplication() = default;

    const juce::String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override     { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override               { return true; }

    void initialise (const juce::String&) override
    {
        mainWindow.reset (new PinkNoiseAudioMonitorWindow (getApplicationName()));

        commandManager.registerAllCommandsForTarget (this);
        mainWindow->addKeyListener (commandManager.getKeyMappings());

       #if JUCE_MAC
        // Preferences goes in the application menu (next to About/Quit), which
        // is where macOS users expect it — and it picks up Cmd-, from the
        // command's default keypress.
        juce::PopupMenu appleMenuItems;
        appleMenuItems.addCommandItem (&commandManager, CommandIDs::showPreferences);

        juce::MenuBarModel::setMacMainMenu (this, &appleMenuItems);
       #endif
    }

    void shutdown() override
    {
       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (nullptr);
       #endif

        mainWindow = nullptr;
    }

    //==========================================================================
    // juce::MenuBarModel
    juce::StringArray getMenuBarNames() override
    {
        return { "Audio" };
    }

    juce::PopupMenu getMenuForIndex (int topLevelMenuIndex, const juce::String&) override
    {
        juce::PopupMenu menu;

        if (topLevelMenuIndex == 0)
            menu.addCommandItem (&commandManager, CommandIDs::showAudioDeviceSetup);

        return menu;
    }

    void menuItemSelected (int /*menuItemID*/, int /*topLevelMenuIndex*/) override
    {
        // All items are command items; the command manager invokes them.
    }

    //==========================================================================
    // juce::ApplicationCommandTarget
    void getAllCommands (juce::Array<juce::CommandID>& commands) override
    {
        JUCEApplication::getAllCommands (commands);
        commands.add (CommandIDs::showPreferences);
        commands.add (CommandIDs::showAudioDeviceSetup);
    }

    void getCommandInfo (juce::CommandID commandID, juce::ApplicationCommandInfo& result) override
    {
        switch (commandID)
        {
            case CommandIDs::showPreferences:
                result.setInfo ("Preferences...", "Detection thresholds, notifications, seed, log folder",
                                "General", 0);
                result.addDefaultKeypress (',', juce::ModifierKeys::commandModifier);
                return;

            case CommandIDs::showAudioDeviceSetup:
                result.setInfo ("Audio Device Setup...", "Choose the input and output devices",
                                "General", 0);
                result.addDefaultKeypress ('a', juce::ModifierKeys::commandModifier
                                                  | juce::ModifierKeys::shiftModifier);
                return;

            default:
                JUCEApplication::getCommandInfo (commandID, result);
                return;
        }
    }

    bool perform (const InvocationInfo& info) override
    {
        auto* content = mainWindow != nullptr ? mainWindow->getMainComponent() : nullptr;

        switch (info.commandID)
        {
            case CommandIDs::showPreferences:
                if (content != nullptr)
                    content->openSettings();
                return true;

            case CommandIDs::showAudioDeviceSetup:
                if (content != nullptr)
                    content->openAudioDeviceSetup();
                return true;

            default:
                return JUCEApplication::perform (info);
        }
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
    juce::ApplicationCommandManager commandManager;
    std::unique_ptr<PinkNoiseAudioMonitorWindow> mainWindow;
};

} // namespace pnam

//==============================================================================
START_JUCE_APPLICATION (pnam::PinkNoiseAudioMonitorApplication)

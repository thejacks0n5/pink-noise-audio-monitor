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
#include "Update/UpdateChecker.h"

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
        showAudioDeviceSetup,
        setLogFile,
        showChannelSetup,
        toggleAudio,
        toggleLogging,
        addMarker,
        showAbout,
        checkForUpdates,
        windowMinimize,
        windowZoom
    };
}

//==============================================================================
/** Standard About box: name, version, one-line description, copyright. */
class AboutBoxComponent : public juce::Component
{
public:
    AboutBoxComponent()
    {
        title.setText (JUCE_APPLICATION_NAME_STRING, juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
        title.setJustificationType (juce::Justification::centred);

        version.setText ("Version " + juce::String (JUCE_APPLICATION_VERSION_STRING)
                           + juce::String::fromUTF8 ("  \xc2\xb7  JUCE ") + juce::String (JUCE_MAJOR_VERSION) + "."
                           + juce::String (JUCE_MINOR_VERSION) + "." + juce::String (JUCE_BUILDNUMBER),
                         juce::dontSendNotification);
        version.setJustificationType (juce::Justification::centred);
        version.setColour (juce::Label::textColourId, pnam::Palette::get (pnam::Palette::textSecondary));

        blurb.setText ("Deterministic pink-noise monitoring for long-run audio path testing:\n"
                       "per-channel analysis, fault detection and CSV session logs.",
                       juce::dontSendNotification);
        blurb.setJustificationType (juce::Justification::centred);

        copyright.setText (juce::String::fromUTF8 ("\xc2\xa9 ") + juce::String (juce::Time::getCurrentTime().getYear())
                             + " pnam", juce::dontSendNotification);
        copyright.setJustificationType (juce::Justification::centred);
        copyright.setColour (juce::Label::textColourId, pnam::Palette::get (pnam::Palette::textSecondary));

        for (auto* l : { &title, &version, &blurb, &copyright })
            addAndMakeVisible (l);

        setSize (380, 190);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (16);
        title.setBounds (b.removeFromTop (34));
        version.setBounds (b.removeFromTop (22));
        b.removeFromTop (10);
        blurb.setBounds (b.removeFromTop (56));
        copyright.setBounds (b.removeFromBottom (20));
    }

private:
    juce::Label title, version, blurb, copyright;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutBoxComponent)
};

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

        // Restore the last window placement; a monitoring app should come back
        // exactly where the user left it.
        if (auto* settings = content->getUserSettings())
        {
            const auto state = settings->getValue ("windowState");
            if (state.isNotEmpty())
                restoreWindowStateFromString (state);
        }
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
        appleMenuItems.addCommandItem (&commandManager, CommandIDs::showAbout);
        appleMenuItems.addCommandItem (&commandManager, CommandIDs::checkForUpdates);
        appleMenuItems.addSeparator();
        appleMenuItems.addCommandItem (&commandManager, CommandIDs::showPreferences);

        juce::MenuBarModel::setMacMainMenu (this, &appleMenuItems);
       #endif

        updateChecker = std::make_unique<UpdateChecker>();
        updateChecker->startAutomaticChecks();
    }

    void shutdown() override
    {
        // First: its dialogs use the look-and-feel MainComponent owns.
        updateChecker = nullptr;

       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (nullptr);
       #endif

        if (mainWindow != nullptr)
            if (auto* content = mainWindow->getMainComponent())
                if (auto* settings = content->getUserSettings())
                {
                    settings->setValue ("windowState", mainWindow->getWindowStateAsString());
                    settings->saveIfNeeded();
                }

        mainWindow = nullptr;
    }

    //==========================================================================
    // juce::MenuBarModel
    juce::StringArray getMenuBarNames() override
    {
        return { "Audio", "Logging", "Window" };
    }

    juce::PopupMenu getMenuForIndex (int topLevelMenuIndex, const juce::String&) override
    {
        juce::PopupMenu menu;

        if (topLevelMenuIndex == 0)
        {
            menu.addCommandItem (&commandManager, CommandIDs::toggleAudio);
            menu.addSeparator();
            menu.addCommandItem (&commandManager, CommandIDs::showAudioDeviceSetup);
            menu.addCommandItem (&commandManager, CommandIDs::showChannelSetup);
        }
        else if (topLevelMenuIndex == 1)
        {
            menu.addCommandItem (&commandManager, CommandIDs::toggleLogging);
            menu.addCommandItem (&commandManager, CommandIDs::addMarker);
            menu.addSeparator();
            menu.addCommandItem (&commandManager, CommandIDs::setLogFile);
        }
        else if (topLevelMenuIndex == 2)
        {
            menu.addCommandItem (&commandManager, CommandIDs::windowMinimize);
            menu.addCommandItem (&commandManager, CommandIDs::windowZoom);
        }

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
        commands.add (CommandIDs::setLogFile);
        commands.add (CommandIDs::showChannelSetup);
        commands.add (CommandIDs::toggleAudio);
        commands.add (CommandIDs::toggleLogging);
        commands.add (CommandIDs::addMarker);
        commands.add (CommandIDs::showAbout);
        commands.add (CommandIDs::checkForUpdates);
        commands.add (CommandIDs::windowMinimize);
        commands.add (CommandIDs::windowZoom);
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

            case CommandIDs::setLogFile:
                result.setInfo ("Set Log File...", "Name this log and choose where to save it",
                                "General", 0);
                result.addDefaultKeypress ('l', juce::ModifierKeys::commandModifier
                                                  | juce::ModifierKeys::shiftModifier);
                return;

            case CommandIDs::showChannelSetup:
                result.setInfo ("Channel Names...", "Name each input/output and set its reference",
                                "General", 0);
                result.addDefaultKeypress ('k', juce::ModifierKeys::commandModifier
                                                  | juce::ModifierKeys::shiftModifier);
                return;

            case CommandIDs::toggleAudio:
                result.setInfo ("Start/Stop Audio", "Toggle the pink-noise engine", "Transport", 0);
                result.addDefaultKeypress ('r', juce::ModifierKeys::commandModifier);
                return;

            case CommandIDs::toggleLogging:
                result.setInfo ("Start/Stop Logging", "Toggle CSV logging", "Transport", 0);
                result.addDefaultKeypress ('l', juce::ModifierKeys::commandModifier);
                return;

            case CommandIDs::addMarker:
                result.setInfo ("Add Marker", "Drop a manual marker into the log", "Transport", 0);
                result.addDefaultKeypress ('m', juce::ModifierKeys::commandModifier
                                                  | juce::ModifierKeys::shiftModifier);
                return;

            case CommandIDs::showAbout:
                result.setInfo ("About " + juce::String (JUCE_APPLICATION_NAME_STRING),
                                "Version and credits", "General", 0);
                return;

            case CommandIDs::checkForUpdates:
                result.setInfo ("Check for Updates...", "Look for a newer version and offer to install it",
                                "General", 0);
                return;

            case CommandIDs::windowMinimize:
                result.setInfo ("Minimize", "Minimize the window", "Window", 0);
                result.addDefaultKeypress ('m', juce::ModifierKeys::commandModifier);
                return;

            case CommandIDs::windowZoom:
                result.setInfo ("Zoom", "Zoom the window", "Window", 0);
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

            case CommandIDs::setLogFile:
                if (content != nullptr)
                    content->openLogFileSetup();
                return true;

            case CommandIDs::showChannelSetup:
                if (content != nullptr)
                    content->openChannelSetup();
                return true;

            case CommandIDs::toggleAudio:
                if (content != nullptr)
                    content->toggleAudio();
                return true;

            case CommandIDs::toggleLogging:
                if (content != nullptr)
                    content->toggleLogging();
                return true;

            case CommandIDs::addMarker:
                if (content != nullptr)
                    content->addMarkerNow();
                return true;

            case CommandIDs::showAbout:
                showAboutBox();
                return true;

            case CommandIDs::checkForUpdates:
                if (updateChecker != nullptr)
                    updateChecker->checkNow (true);
                return true;

            case CommandIDs::windowMinimize:
                if (mainWindow != nullptr)
                    mainWindow->setMinimised (true);
                return true;

            case CommandIDs::windowZoom:
                if (mainWindow != nullptr)
                    mainWindow->setFullScreen (! mainWindow->isFullScreen());
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
    void showAboutBox()
    {
        if (aboutWindow != nullptr)
        {
            aboutWindow->toFront (true);
            return;
        }

        juce::DialogWindow::LaunchOptions launch;
        launch.content.setOwned (new AboutBoxComponent());
        launch.dialogTitle = "About " + juce::String (JUCE_APPLICATION_NAME_STRING);
        launch.dialogBackgroundColour = pnam::Palette::get (pnam::Palette::panelBackground);
        launch.escapeKeyTriggersCloseButton = true;
        launch.useNativeTitleBar = true;
        launch.resizable = false;

        aboutWindow = launch.launchAsync();
    }

    juce::ApplicationCommandManager commandManager;
    std::unique_ptr<PinkNoiseAudioMonitorWindow> mainWindow;
    std::unique_ptr<UpdateChecker> updateChecker;
    juce::Component::SafePointer<juce::DialogWindow> aboutWindow;
};

} // namespace pnam

//==============================================================================
START_JUCE_APPLICATION (pnam::PinkNoiseAudioMonitorApplication)

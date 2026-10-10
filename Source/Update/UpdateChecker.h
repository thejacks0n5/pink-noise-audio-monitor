/*
  ==============================================================================
    UpdateChecker.h
    Pink Noise Audio Monitor (pnam) — in-app updates from GitHub releases.

    Asks the GitHub API for the repo's latest release, and when its version is
    newer than the running build: downloads the release's macOS zip, unpacks
    it, verifies the unpacked app is signed by the expected team and really is
    that newer version, then asks the user whether to install it. Installing
    swaps the app bundle from a small detached shell script once this process
    has quit, and relaunches.

    Threading: all network, unzip and signature work happens on this object's
    own background thread. Results come back to the message thread through an
    AsyncUpdater, which is where every dialog is shown. Nothing here is ever
    touched from the audio thread.

    Nothing is installed without the user saying so, and an automatic check
    that fails (offline rack machine, API unreachable) stays silent — only a
    check the user asked for reports its outcome either way.
  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace pnam
{

class UpdateChecker : private juce::Thread,
                      private juce::AsyncUpdater,
                      private juce::Timer
{
public:
    UpdateChecker();
    ~UpdateChecker() override;

    /** Starts the automatic schedule: one check shortly after launch, then one
        a day for as long as the app keeps running (sessions last days). */
    void startAutomaticChecks();

    /** Checks now. A user-initiated check always reports its outcome; an
        automatic one only speaks up when an update is ready to install. */
    void checkNow (bool userInitiated);

    /** True if `candidate` is a strictly newer dotted version than `current`
        ("1.0.10" > "1.0.9"; a leading 'v' is ignored). */
    static bool isNewerVersion (const juce::String& candidate, const juce::String& current);

private:
    enum class Outcome { none, upToDate, updateReady, failed };

    struct Result
    {
        Outcome      outcome = Outcome::none;
        bool         userInitiated = false;
        juce::String version;        // the release's version, without the 'v'
        juce::String notes;          // release notes body
        juce::String pageUrl;        // the release's web page
        juce::File   downloadedApp;  // verified, unpacked .app (updateReady)
        juce::String error;          // why it failed (failed)
    };

    // juce::Thread
    void run() override;

    // juce::AsyncUpdater — message thread
    void handleAsyncUpdate() override;

    // juce::Timer — message thread
    void timerCallback() override;

    Result performCheck (bool userInitiated);
    bool   downloadTo (const juce::URL& url, const juce::File& destination, juce::String& error);
    bool   verifyDownloadedApp (const juce::File& app, const juce::String& expectedVersion, juce::String& error);

    void promptToInstall (const Result&);
    void installAndRelaunch (const juce::File& newApp, const juce::String& version, const juce::String& pageUrl);
    void showMessage (juce::MessageBoxIconType, const juce::String& title, const juce::String& message);

    static juce::File getUpdatesFolder();

    std::atomic<bool> pendingUserInitiated { false };
    bool launchCheckDone = false;

    juce::CriticalSection resultLock;
    Result latestResult;              // guarded by resultLock

    juce::String promptedVersion;     // message thread: asked once per run
    juce::ScopedMessageBox messageBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateChecker)
};

} // namespace pnam

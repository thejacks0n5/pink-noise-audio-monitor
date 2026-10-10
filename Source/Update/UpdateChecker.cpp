/*
  ==============================================================================
    UpdateChecker.cpp
    Pink Noise Audio Monitor (pnam) — in-app updates from GitHub releases.
    See UpdateChecker.h for the contract and threading rules.
  ==============================================================================
*/

#include "UpdateChecker.h"

#include <unistd.h>

namespace pnam
{

namespace
{
    // The release contract this updater depends on: releases are tagged
    // "v<major>.<minor>.<patch>" and carry one asset ending in assetSuffix that
    // unzips to a single Developer ID signed .app. See BUILD_NOTES.md §6.
    const char* const latestReleaseApiUrl = "https://api.github.com/repos/thejacks0n5/pink-noise-audio-monitor/releases/latest";
    const char* const assetSuffix         = "-macOS-universal.zip";

    // Only an app that satisfies this is ever offered for install: Apple's
    // standard Developer ID requirement, pinned to our team and bundle id. A
    // valid signature from anyone else, or an ad-hoc one, does not pass.
    const char* const signingRequirement =
        "anchor apple generic"
        " and identifier \"com.pnam.pinknoiseaudiomonitor\""
        " and certificate 1[field.1.2.840.113635.100.6.2.6] exists"
        " and certificate leaf[field.1.2.840.113635.100.6.1.13] exists"
        " and certificate leaf[subject.OU] = \"U6N5RLQS69\"";

    constexpr int   connectionTimeoutMs = 10000;
    [[maybe_unused]] constexpr int launchCheckDelayMs = 5000;
    constexpr int   recheckIntervalMs   = 24 * 60 * 60 * 1000;
    constexpr juce::int64 maxDownloadBytes = 512 * 1024 * 1024;

    juce::String currentVersion()   { return JUCE_APPLICATION_VERSION_STRING; }

    juce::Array<int> versionParts (juce::String v)
    {
        v = v.trim();

        if (v.startsWithIgnoreCase ("v"))
            v = v.substring (1);

        // Ignore any pre-release / build suffix ("1.2.0-beta", "1.2.0+7").
        v = v.upToFirstOccurrenceOf ("-", false, false)
             .upToFirstOccurrenceOf ("+", false, false);

        juce::Array<int> parts;

        for (const auto& token : juce::StringArray::fromTokens (v, ".", {}))
            parts.add (token.getIntValue());

        return parts;
    }

    /** Runs a tool to completion. Returns its exit code, or -1 if it could not
        be started or did not finish in time. */
    int runTool (const juce::StringArray& arguments, int timeoutMs, juce::String* output = nullptr)
    {
        juce::ChildProcess process;

        if (! process.start (arguments))
            return -1;

        const auto text = process.readAllProcessOutput();

        if (output != nullptr)
            *output = text;

        if (! process.waitForProcessToFinish (timeoutMs))
        {
            process.kill();
            return -1;
        }

        return (int) process.getExitCode();
    }

    juce::String readBundleVersion (const juce::File& app)
    {
        const auto plist = juce::parseXML (app.getChildFile ("Contents/Info.plist"));

        if (plist == nullptr)
            return {};

        if (const auto* dict = plist->getChildByName ("dict"))
            for (const auto* key : dict->getChildWithTagNameIterator ("key"))
                if (key->getAllSubText().trim() == "CFBundleShortVersionString")
                    if (const auto* value = key->getNextElement())
                        return value->getAllSubText().trim();

        return {};
    }

    juce::File findAppIn (const juce::File& folder)
    {
        const auto apps = folder.findChildFiles (juce::File::findDirectories, false, "*.app");
        return apps.size() == 1 ? apps.getFirst() : juce::File();
    }

    juce::String shortenedNotes (const juce::String& notes)
    {
        constexpr int maxChars = 500;
        const auto trimmed = notes.trim();

        return trimmed.length() <= maxChars ? trimmed
                                            : trimmed.substring (0, maxChars).trimEnd() + "...";
    }
}

//==============================================================================
UpdateChecker::UpdateChecker()
    : juce::Thread ("Update check")
{
}

UpdateChecker::~UpdateChecker()
{
    stopTimer();
    cancelPendingUpdate();
    messageBox.close();

    // A check in flight is at worst inside one network read; it polls
    // threadShouldExit() between chunks and the connect is bounded by
    // connectionTimeoutMs.
    stopThread (connectionTimeoutMs + 5000);
}

void UpdateChecker::startAutomaticChecks()
{
   #if JUCE_DEBUG
    // Development builds do not phone home on their own; Check for Updates...
    // in the application menu still works.
   #else
    startTimer (launchCheckDelayMs);
   #endif
}

void UpdateChecker::checkNow (bool userInitiated)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (userInitiated)
        pendingUserInitiated.store (true);

    // If a check is already running, its result is reported as user-initiated
    // (see handleAsyncUpdate), so the user still gets an answer.
    if (! isThreadRunning())
        startThread (juce::Thread::Priority::background);
}

bool UpdateChecker::isNewerVersion (const juce::String& candidate, const juce::String& current)
{
    const auto a = versionParts (candidate);
    const auto b = versionParts (current);

    for (int i = 0; i < juce::jmax (a.size(), b.size()); ++i)
    {
        const int x = i < a.size() ? a[i] : 0;
        const int y = i < b.size() ? b[i] : 0;

        if (x != y)
            return x > y;
    }

    return false;
}

juce::File UpdateChecker::getUpdatesFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
             .getChildFile ("Caches/PinkNoiseAudioMonitor/Updates");
}

//==============================================================================
void UpdateChecker::timerCallback()
{
    if (! launchCheckDone)
    {
        launchCheckDone = true;
        startTimer (recheckIntervalMs);
    }

    checkNow (false);
}

void UpdateChecker::run()
{
    auto result = performCheck (pendingUserInitiated.exchange (false));

    if (threadShouldExit())
        return;

    {
        const juce::ScopedLock lock (resultLock);
        latestResult = std::move (result);
    }

    triggerAsyncUpdate();
}

UpdateChecker::Result UpdateChecker::performCheck (bool userInitiated)
{
    Result result;
    result.userInitiated = userInitiated;

    const auto fail = [&result] (const juce::String& why)
    {
        result.outcome = Outcome::failed;
        result.error = why;
        return result;
    };

    //--------------------------------------------------------------------------
    // 1. What is the latest release?
    int statusCode = 0;
    auto stream = juce::URL (latestReleaseApiUrl)
                    .createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                          .withConnectionTimeoutMs (connectionTimeoutMs)
                                          .withStatusCode (&statusCode)
                                          .withExtraHeaders ("Accept: application/vnd.github+json\r\n"
                                                             "User-Agent: PinkNoiseAudioMonitor/" + currentVersion()));

    if (stream == nullptr)
        return fail ("Could not reach GitHub. Check the network connection and try again.");

    const auto body = stream->readEntireStreamAsString();
    stream.reset();

    if (statusCode != 200)
        return fail ("GitHub answered with HTTP " + juce::String (statusCode) + " when asked for the latest release.");

    const auto release = juce::JSON::parse (body);
    const auto tag = release["tag_name"].toString();

    if (tag.isEmpty())
        return fail ("GitHub's answer did not describe a release.");

    result.version = tag.startsWithIgnoreCase ("v") ? tag.substring (1) : tag;
    result.notes   = release["body"].toString();
    result.pageUrl = release["html_url"].toString();

    if (! isNewerVersion (result.version, currentVersion()))
    {
        result.outcome = Outcome::upToDate;
        return result;
    }

    //--------------------------------------------------------------------------
    // 2. Find the macOS download on it.
    juce::String assetName, assetUrl;
    juce::int64 assetSize = 0;

    if (const auto* assets = release["assets"].getArray())
    {
        for (const auto& asset : *assets)
        {
            const auto name = asset["name"].toString();

            if (name.endsWithIgnoreCase (assetSuffix))
            {
                assetName = name;
                assetUrl  = asset["browser_download_url"].toString();
                assetSize = (juce::int64) asset["size"];
                break;
            }
        }
    }

    if (assetUrl.isEmpty())
        return fail ("Version " + result.version + " has been released, but it has no macOS download attached.");

    //--------------------------------------------------------------------------
    // 3. Fetch and unpack it, unless a verified copy is already waiting from an
    //    earlier check the user answered with "Later".
    const auto updatesFolder = getUpdatesFolder();
    const auto versionFolder = updatesFolder.getChildFile (juce::File::createLegalFileName (result.version));
    const auto unpackFolder  = versionFolder.getChildFile ("app");

    for (const auto& stale : updatesFolder.findChildFiles (juce::File::findDirectories, false))
        if (stale != versionFolder)
            stale.deleteRecursively();

    juce::String error;
    auto app = findAppIn (unpackFolder);

    if (app == juce::File() || ! verifyDownloadedApp (app, result.version, error))
    {
        versionFolder.deleteRecursively();

        if (! unpackFolder.createDirectory())
            return fail ("Could not create " + unpackFolder.getFullPathName() + ".");

        const auto zip = versionFolder.getChildFile (juce::File::createLegalFileName (assetName));

        if (! downloadTo (juce::URL (assetUrl), zip, error))
            return fail (error);

        if (assetSize > 0 && zip.getSize() != assetSize)
            return fail ("The download of version " + result.version + " was incomplete.");

        // ditto, not juce::ZipFile: the bundle's symlinks, permissions and
        // extended attributes have to survive or the signature will not.
        if (runTool ({ "/usr/bin/ditto", "-x", "-k", zip.getFullPathName(), unpackFolder.getFullPathName() }, 120000) != 0)
            return fail ("The download of version " + result.version + " could not be unpacked.");

        zip.deleteFile();
        app = findAppIn (unpackFolder);

        if (app == juce::File())
            return fail ("The download of version " + result.version + " did not contain the app.");

        if (! verifyDownloadedApp (app, result.version, error))
        {
            versionFolder.deleteRecursively();
            return fail (error);
        }
    }

    result.outcome = Outcome::updateReady;
    result.downloadedApp = app;
    return result;
}

bool UpdateChecker::downloadTo (const juce::URL& url, const juce::File& destination, juce::String& error)
{
    int statusCode = 0;
    auto in = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                       .withConnectionTimeoutMs (connectionTimeoutMs)
                                       .withStatusCode (&statusCode)
                                       .withExtraHeaders ("User-Agent: PinkNoiseAudioMonitor/" + currentVersion()));

    if (in == nullptr || statusCode != 200)
    {
        error = "The update could not be downloaded" + (statusCode != 0 ? " (HTTP " + juce::String (statusCode) + ")."
                                                                        : juce::String ("."));
        return false;
    }

    destination.deleteFile();
    juce::FileOutputStream out (destination);

    if (out.failedToOpen())
    {
        error = "Could not write " + destination.getFullPathName() + ".";
        return false;
    }

    juce::HeapBlock<char> buffer (65536);
    juce::int64 total = 0;

    for (;;)
    {
        if (threadShouldExit())
        {
            error = "The update check was cancelled.";
            return false;
        }

        const int numRead = in->read (buffer, 65536);

        if (numRead < 0)
        {
            error = "The download was interrupted.";
            return false;
        }

        if (numRead == 0)
            break;

        total += numRead;

        if (total > maxDownloadBytes || ! out.write (buffer, (size_t) numRead))
        {
            error = "The update could not be saved.";
            return false;
        }
    }

    out.flush();
    return out.getStatus().wasOk();
}

bool UpdateChecker::verifyDownloadedApp (const juce::File& app, const juce::String& expectedVersion, juce::String& error)
{
    juce::String output;

    if (runTool ({ "/usr/bin/codesign", "--verify", "--deep", "--strict",
                   "-R=" + juce::String (signingRequirement), app.getFullPathName() }, 60000, &output) != 0)
    {
        error = "The downloaded update is not signed by this app's developer, so it was discarded.";
        return false;
    }

    const auto bundleVersion = readBundleVersion (app);

    // The bundle has to be the version the release claims, and newer than this
    // build: otherwise a mislabelled release would be offered on every check.
    if (bundleVersion != expectedVersion || ! isNewerVersion (bundleVersion, currentVersion()))
    {
        error = "The downloaded update reports version \"" + bundleVersion
                  + "\" instead of " + expectedVersion + ", so it was discarded.";
        return false;
    }

    return true;
}

//==============================================================================
void UpdateChecker::handleAsyncUpdate()
{
    Result result;

    {
        const juce::ScopedLock lock (resultLock);
        result = std::move (latestResult);
        latestResult = {};
    }

    // The user asked while an automatic check was already under way.
    if (pendingUserInitiated.exchange (false))
        result.userInitiated = true;

    switch (result.outcome)
    {
        case Outcome::updateReady:
            if (result.userInitiated || promptedVersion != result.version)
                promptToInstall (result);
            break;

        case Outcome::upToDate:
            if (result.userInitiated)
                showMessage (juce::MessageBoxIconType::InfoIcon, "You're up to date",
                             "Version " + currentVersion() + " is the latest version.");
            break;

        case Outcome::failed:
            if (result.userInitiated)
                showMessage (juce::MessageBoxIconType::WarningIcon, "Could not check for updates", result.error);
            break;

        case Outcome::none:
            break;
    }
}

void UpdateChecker::promptToInstall (const Result& result)
{
    promptedVersion = result.version;

    juce::String message;
    message << "You have version " << currentVersion() << ". Version " << result.version
            << " has been downloaded and verified.\n\n"
            << "Installing quits the app, which ends any audio and logging session that is running, "
            << "then reopens it on the new version.";

    const auto notes = shortenedNotes (result.notes);

    if (notes.isNotEmpty())
        message << "\n\n" << notes;

    const auto newApp  = result.downloadedApp;
    const auto version = result.version;
    const auto pageUrl = result.pageUrl;

    messageBox = juce::AlertWindow::showScopedAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::InfoIcon)
            .withTitle ("Version " + result.version + " is available")
            .withMessage (message)
            .withButton ("Install and Relaunch")
            .withButton ("Later"),
        [this, newApp, version, pageUrl] (int button)
        {
            if (button == 1)
                installAndRelaunch (newApp, version, pageUrl);
        });
}

void UpdateChecker::installAndRelaunch (const juce::File& newApp, const juce::String& version, const juce::String& pageUrl)
{
    const auto currentApp = juce::File::getSpecialLocation (juce::File::currentApplicationFile);

    // An app opened straight from a browser download runs from a read-only,
    // randomised "translocated" path until it is moved in Finder, so it cannot
    // replace itself. Neither can one in a folder this user cannot write to.
    const bool canReplaceItself = currentApp.hasFileExtension ("app")
                               && ! currentApp.getFullPathName().contains ("/AppTranslocation/")
                               && currentApp.getParentDirectory().hasWriteAccess();

    if (! canReplaceItself)
    {
        messageBox = juce::AlertWindow::showScopedAsync (
            juce::MessageBoxOptions()
                .withIconType (juce::MessageBoxIconType::InfoIcon)
                .withTitle ("Install version " + version + " by hand")
                .withMessage ("This copy of the app cannot replace itself where it is running from. "
                              "Move the app into the Applications folder and future updates will install themselves.\n\n"
                              "Version " + version + " is downloaded and ready: quit this app, then drag the new one over the old.")
                .withButton ("Show New Version in Finder")
                .withButton ("Cancel"),
            [newApp] (int button)
            {
                if (button == 1)
                    newApp.revealToUser();
            });

        return;
    }

    // The swap has to happen after this process has gone, so it is done by a
    // detached shell script: wait for our pid, move the old bundle aside, copy
    // the new one in (restoring the old one if that fails), reopen.
    const auto workFolder = newApp.getParentDirectory().getParentDirectory();
    const auto script     = workFolder.getChildFile ("install.sh");
    const auto log        = workFolder.getChildFile ("install.log");

    const juce::String scriptText =
        "#!/bin/sh\n"
        "PID=\"$1\"; NEW=\"$2\"; DEST=\"$3\"\n"
        "n=0\n"
        "while /bin/kill -0 \"$PID\" 2>/dev/null; do\n"
        "    n=$((n+1))\n"
        "    if [ \"$n\" -gt 300 ]; then echo \"app did not quit; nothing changed\"; exit 1; fi\n"
        "    /bin/sleep 0.2\n"
        "done\n"
        "BACKUP=\"$DEST.previous\"\n"
        "/bin/rm -rf \"$BACKUP\"\n"
        "if /bin/mv \"$DEST\" \"$BACKUP\"; then\n"
        "    if /usr/bin/ditto \"$NEW\" \"$DEST\"; then\n"
        "        /bin/rm -rf \"$BACKUP\"\n"
        "        echo \"installed\"\n"
        "    else\n"
        "        echo \"copy failed; restoring the previous version\"\n"
        "        /bin/rm -rf \"$DEST\"\n"
        "        /bin/mv \"$BACKUP\" \"$DEST\"\n"
        "    fi\n"
        "else\n"
        "    echo \"could not move the running version aside; nothing changed\"\n"
        "fi\n"
        "/usr/bin/open \"$DEST\"\n";

    juce::ChildProcess launcher;

    if (! script.replaceWithText (scriptText)
        || ! launcher.start ({ "/bin/sh", "-c", "/bin/sh \"$0\" \"$1\" \"$2\" \"$3\" > \"$4\" 2>&1 &",
                               script.getFullPathName(),
                               juce::String ((int) ::getpid()),
                               newApp.getFullPathName(),
                               currentApp.getFullPathName(),
                               log.getFullPathName() }, 0))
    {
        showMessage (juce::MessageBoxIconType::WarningIcon, "Could not start the update",
                     "Nothing was changed. You can download version " + version + " from\n" + pageUrl);
        return;
    }

    launcher.waitForProcessToFinish (5000);

    if (auto* app = juce::JUCEApplicationBase::getInstance())
        app->systemRequestedQuit();
}

void UpdateChecker::showMessage (juce::MessageBoxIconType icon, const juce::String& title, const juce::String& message)
{
    messageBox = juce::AlertWindow::showScopedAsync (juce::MessageBoxOptions()
                                                       .withIconType (icon)
                                                       .withTitle (title)
                                                       .withMessage (message)
                                                       .withButton ("OK"),
                                                     nullptr);
}

} // namespace pnam

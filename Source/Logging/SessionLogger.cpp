/*
  ==============================================================================
    SessionLogger.cpp
    Pink Noise Audio Monitor (pnam) — per-session CSV logging implementation.

    MESSAGE THREAD ONLY.
  ==============================================================================
*/

#include "SessionLogger.h"

namespace pnam
{

//==============================================================================
namespace
{
    juce::String formatDb (float db)
    {
        return juce::String (db, 2);
    }

    juce::String formatSeconds (double s)
    {
        return juce::String (s, 3);
    }

    juce::String formatCorrelation (float c)
    {
        return juce::String (c, 3);
    }
}

//==============================================================================
SessionLogger::~SessionLogger()
{
    stopSession();
}

//==============================================================================
bool SessionLogger::startSession (const juce::File& baseFolder, const SessionInfo& info)
{
    stopSession();

    if (! baseFolder.exists() && ! baseFolder.createDirectory())
    {
        reportErrorOnce ("Could not create base log folder: " + baseFolder.getFullPathName());
        return false;
    }

    auto sessionFolder = baseFolder.getChildFile (info.sessionId);

    if (! sessionFolder.exists() && ! sessionFolder.createDirectory())
    {
        reportErrorOnce ("Could not create session folder: " + sessionFolder.getFullPathName());
        return false;
    }

    auto measurementsFile = sessionFolder.getChildFile ("measurements.csv");
    auto eventsFile       = sessionFolder.getChildFile ("events.csv");

    // Adopt the info before opening: the measurement header's per-channel
    // columns are derived from its channel counts and names.
    activeInfo = info;

    if (! measurementWriter.open (measurementsFile, getMeasurementCsvHeader (activeInfo)))
    {
        reportErrorOnce ("Could not open measurements.csv for writing.");
        return false;
    }

    writtenEventRows.clear();
    eventsFilePath = eventsFile;

    if (! eventWriter.open (eventsFile, getEventCsvHeader()))
    {
        reportErrorOnce ("Could not open events.csv for writing.");
        measurementWriter.close();
        return false;
    }

    activeInfo = info;
    activeInfo.logFolder = sessionFolder;

    // Write session_info.txt as a simple key: value dump.
    auto infoFile = sessionFolder.getChildFile ("session_info.txt");
    juce::String text;
    text << "sessionId: "         << activeInfo.sessionId << "\n";
    text << "startTimeUtc: "      << activeInfo.startTimeUtc.toISO8601 (true) << "\n";
    text << "sampleRate: "        << activeInfo.sampleRate << "\n";
    text << "bufferSize: "        << activeInfo.bufferSize << "\n";
    text << "numOutputChannels: " << activeInfo.numOutputChannels << "\n";
    text << "numInputChannels: "  << activeInfo.numInputChannels << "\n";
    text << "inputDeviceName: "   << activeInfo.inputDeviceName << "\n";
    text << "outputDeviceName: "  << activeInfo.outputDeviceName << "\n";
    text << "pinkNoiseSeed: "     << (juce::int64) activeInfo.pinkNoiseSeed << "\n";
    text << "outputLevelDb: "     << activeInfo.outputLevelDb << "\n";
    text << "logFolder: "         << activeInfo.logFolder.getFullPathName() << "\n";

    for (int ch = 0; ch < juce::jlimit (0, maxChannels, activeInfo.numOutputChannels); ++ch)
        text << "outputChannel" << (ch + 1) << ": " << channelName (activeInfo, ch, false) << "\n";

    for (int ch = 0; ch < juce::jlimit (0, maxChannels, activeInfo.numInputChannels); ++ch)
    {
        const int refOut = juce::isPositiveAndBelow (ch, activeInfo.referenceOutputForInput.size())
                             ? activeInfo.referenceOutputForInput[ch] : 0;

        text << "inputChannel" << (ch + 1) << ": " << channelName (activeInfo, ch, true)
             << " (referenced to " << channelName (activeInfo, refOut, false) << ")"
             << (isInputLogged (activeInfo, ch) ? "" : " [not monitored - excluded from measurements.csv]")
             << "\n";
    }

    infoFile.replaceWithText (text);

    active = true;
    measurementErrorReported = false;
    eventErrorReported = false;
    lastFlushMillis = juce::Time::getMillisecondCounter();

    return true;
}

//==============================================================================
void SessionLogger::stopSession()
{
    if (! active)
        return;

    measurementWriter.flush();
    eventWriter.flush();

    // Append a stop line to session_info.txt.
    auto infoFile = activeInfo.logFolder.getChildFile ("session_info.txt");
    if (infoFile.existsAsFile())
    {
        juce::String stopLine;
        stopLine << "stopTimeUtc: " << juce::Time::getCurrentTime().toISO8601 (true) << "\n";
        stopLine << "measurementRowsWritten: " << (juce::int64) measurementWriter.getRowsWritten() << "\n";
        stopLine << "eventRowsWritten: " << (juce::int64) eventWriter.getRowsWritten() << "\n";
        infoFile.appendText (stopLine);
    }

    measurementWriter.close();
    eventWriter.close();

    active = false;
}

//==============================================================================
bool SessionLogger::isSessionActive() const noexcept
{
    return active;
}

SessionInfo SessionLogger::getSessionInfo() const
{
    return activeInfo;
}

juce::File SessionLogger::getSessionFolder() const
{
    return active ? activeInfo.logFolder : juce::File();
}

//==============================================================================
juce::String SessionLogger::makeTimestampUtc (double elapsedSeconds) const
{
    auto t = activeInfo.startTimeUtc + juce::RelativeTime (elapsedSeconds);
    return t.toISO8601 (true);
}

//==============================================================================
void SessionLogger::logMeasurement (const Measurement& m)
{
    if (! active)
        return;

    juce::StringArray row;
    row.add (makeTimestampUtc (m.elapsedSeconds));
    row.add (formatSeconds (m.elapsedSeconds));
    row.add (juce::String (m.sampleRate, 1));
    row.add (juce::String (m.bufferSize));
    // Per-channel columns, in the same order getMeasurementCsvHeader() emits
    // them. The header is built from the session's channel counts, so iterate
    // those — not the measurement's — or a mid-session device change would
    // silently shift every column.
    const int outs = juce::jlimit (0, maxChannels, activeInfo.numOutputChannels);
    const int ins  = juce::jlimit (0, maxChannels, activeInfo.numInputChannels);

    for (int ch = 0; ch < outs; ++ch)
    {
        const bool valid = ch < m.numOutputChannels;
        row.add (valid ? formatDb (m.refPeakDb[ch]) : juce::String());
        row.add (valid ? formatDb (m.refRmsDb[ch])  : juce::String());
    }

    for (int ch = 0; ch < ins; ++ch)
    {
        if (! isInputLogged (activeInfo, ch))
            continue;

        const bool valid = ch < m.numInputChannels;
        row.add (valid ? formatDb (m.inPeakDb[ch])   : juce::String());
        row.add (valid ? formatDb (m.inRmsDb[ch])    : juce::String());
        row.add (valid ? formatDb (m.gainDiffDb[ch]) : juce::String());
        row.add (valid ? juce::String (m.clipCountPerChannel[ch]) : juce::String());
    }

    row.add (formatCorrelation (m.correlation));
    row.add (juce::String (m.dropoutFlag));
    row.add (juce::String (m.clipCountBlock));
    row.add (juce::String (m.statusCode));

    if (! measurementWriter.writeRow (row) || measurementWriter.hadWriteError())
        reportErrorOnce ("Failed writing to measurements.csv");

    auto nowMillis = juce::Time::getMillisecondCounter();
    if ((nowMillis - lastFlushMillis) >= (juce::uint32) (flushIntervalSeconds * 1000.0))
    {
        measurementWriter.flush();
        lastFlushMillis = nowMillis;

        if (measurementWriter.hadWriteError())
            reportErrorOnce ("Failed flushing measurements.csv");
    }
}

//==============================================================================
void SessionLogger::logEvent (const Event& e, const juce::String& description)
{
    if (! active)
        return;

    juce::StringArray row;
    row.add (makeTimestampUtc (e.elapsedSeconds));
    row.add (formatSeconds (e.elapsedSeconds));
    row.add (juce::String (toString (e.type)));
    row.add (juce::String (toString (e.severity)));
    // Prefer the user's channel name; fall back to the legacy L/R/Both label
    // for events that aren't tied to one channel.
    row.add (e.channelIndex >= 0 ? channelName (activeInfo, e.channelIndex, e.channelIsInput)
                                 : juce::String (toString (e.channel)));
    row.add (juce::String (e.measuredValue, 3));
    row.add (juce::String (e.expectedValue, 3));
    row.add (juce::String (e.difference, 3));
    row.add (juce::String (e.thresholdUsed, 3));
    row.add (juce::String (e.code));
    row.add (description);

    if (! eventWriter.writeRow (row) || eventWriter.hadWriteError())
        reportErrorOnce ("Failed writing to events.csv");
    else
        writtenEventRows.push_back (row);

    eventWriter.flush();

    if (eventWriter.hadWriteError())
        reportErrorOnce ("Failed flushing events.csv");
}

bool SessionLogger::amendEventDescription (double elapsedSeconds, EventType type,
                                           const juce::String& newDescription)
{
    if (! active)
        return false;

    // Rows carry elapsed as formatSeconds(3 decimals); compare the same way so
    // float noise cannot miss the match. Search newest-first: if two markers
    // somehow share a timestamp, the later one is the one just edited.
    const auto elapsedText = formatSeconds (elapsedSeconds);
    const auto typeText    = juce::String (toString (type));

    for (auto it = writtenEventRows.rbegin(); it != writtenEventRows.rend(); ++it)
    {
        auto& row = *it;

        if (row.size() >= 11 && row[1] == elapsedText && row[2] == typeText)
        {
            row.set (row.size() - 1, newDescription);

            // Rewrite the whole file: close, reopen with the header, replay.
            eventWriter.close();

            if (! eventWriter.open (eventsFilePath, getEventCsvHeader()))
            {
                reportErrorOnce ("Could not rewrite events.csv");
                return false;
            }

            for (const auto& r : writtenEventRows)
                eventWriter.writeRow (r);

            eventWriter.flush();
            return ! eventWriter.hadWriteError();
        }
    }

    return false;
}

//==============================================================================
juce::String SessionLogger::composeDescription (const Event& e)
{
    // channelIndex identifies the channel once there are more than two; the
    // legacy `channel` field reads "Both" for every per-channel fault.
    juce::String channelText = e.channelIndex >= 0
                                 ? (e.channelIsInput ? "In " : "Out ") + juce::String (e.channelIndex + 1)
                                 : juce::String (toString (e.channel));

    switch (e.type)
    {
        case EventType::CompleteDropout:
        {
            if (e.code == eventCodeStart)
                return "Dropout started (input " + juce::String (e.measuredValue, 1)
                        + " dB, expected " + juce::String (e.expectedValue, 1) + " dB)";
            if (e.code == eventCodeEnd)
                // End events carry the duration in measuredValue and difference;
                // there is no meaningful dB reading to report, and printing
                // `difference` as one produced nonsense like "input 484.6 dB".
                return "Dropout ended after " + juce::String (e.measuredValue, 2) + " s";
            return "Dropout (" + channelText + ")";
        }

        case EventType::Clipping:
        {
            if (e.code == eventCodeStart)
                return "Clipping started on channel " + channelText;
            if (e.code == eventCodeEnd)
                return "Clipping ended after " + juce::String (e.measuredValue, 2) + " s on channel " + channelText;
            return "Clipping detected on channel " + channelText;
        }

        case EventType::LevelChange:
        {
            if (e.code == eventCodeStart)
                return "Level change started: measured " + juce::String (e.measuredValue, 1)
                        + " dB vs expected " + juce::String (e.expectedValue, 1)
                        + " dB (diff " + juce::String (e.difference, 1) + " dB)";
            if (e.code == eventCodeEnd)
                return "Level change ended after " + juce::String (e.measuredValue, 2) + " s";
            return "Level change (" + channelText + ")";
        }

        case EventType::ManualMarker:
            return "Manual marker";

        case EventType::AudioDeviceLost:
            return "Audio device lost";

        case EventType::AudioSettingsChanged:
            // MainComponent supplies a specific description naming what changed;
            // this is the fallback when the event is composed from the POD alone.
            return "Audio settings changed";

        case EventType::LoggingError:
            return "Logging error";

        case EventType::Other:
        default:
            return "Event";
    }
}

//==============================================================================
juce::String SessionLogger::channelName (const SessionInfo& info, int channelIndex, bool isInput)
{
    const auto& names = isInput ? info.inputChannelNames : info.outputChannelNames;

    if (juce::isPositiveAndBelow (channelIndex, names.size()) && names[channelIndex].isNotEmpty())
        return names[channelIndex];

    return (isInput ? "In " : "Out ") + juce::String (channelIndex + 1);
}

bool SessionLogger::isInputLogged (const SessionInfo& info, int channelIndex)
{
    // An unmonitored input is one the user told us has nothing patched in, so it
    // is omitted from the CSV entirely rather than filling columns with noise.
    return ! juce::isPositiveAndBelow (channelIndex, info.inputMonitored.size())
        || info.inputMonitored[channelIndex];
}

juce::StringArray SessionLogger::getMeasurementCsvHeader (const SessionInfo& info)
{
    // Column names are sanitised so a channel called "Booth, Left" cannot break
    // the CSV or collide with the delimiter.
    const auto columnise = [] (juce::String name) -> juce::String
    {
        name = name.retainCharacters ("abcdefghijklmnopqrstuvwxyz"
                                      "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-");
        return name.trim().replaceCharacter (' ', '_').replaceCharacter ('-', '_').toLowerCase();
    };

    juce::StringArray header { "timestamp_utc", "elapsed_s", "sample_rate", "buffer_size" };

    const int outs = juce::jlimit (0, maxChannels, info.numOutputChannels);
    const int ins  = juce::jlimit (0, maxChannels, info.numInputChannels);

    // Sanitised names must stay unique: two channels named "Booth L" and
    // "booth-l" would otherwise collapse into identical column names and break
    // any ingestion that keys on the header. Blank results fall back to the
    // positional name; duplicates get a numeric suffix.
    juce::StringArray usedNames;

    const auto uniqueColumn = [&usedNames, &columnise] (juce::String name,
                                                        const juce::String& fallback) -> juce::String
    {
        auto n = columnise (name);

        if (n.isEmpty())
            n = fallback;

        auto candidate = n;
        for (int suffix = 2; usedNames.contains (candidate); ++suffix)
            candidate = n + "_" + juce::String (suffix);

        usedNames.add (candidate);
        return candidate;
    };

    for (int ch = 0; ch < outs; ++ch)
    {
        const auto n = uniqueColumn (channelName (info, ch, false), "out_" + juce::String (ch + 1));
        header.add (n + "_peak_db");
        header.add (n + "_rms_db");
    }

    for (int ch = 0; ch < ins; ++ch)
    {
        if (! isInputLogged (info, ch))
            continue;

        const auto n = uniqueColumn (channelName (info, ch, true), "in_" + juce::String (ch + 1));
        header.add (n + "_peak_db");
        header.add (n + "_rms_db");
        header.add (n + "_gain_diff_db");
        header.add (n + "_clip_count");
    }

    header.addArray ({ "correlation", "dropout_flag", "clip_count", "status_code" });
    return header;
}

juce::StringArray SessionLogger::getEventCsvHeader()
{
    return { "timestamp_utc", "elapsed_s", "event_type", "severity", "channel",
             "measured_value", "expected_value", "difference", "threshold_used",
             "code", "description" };
}

//==============================================================================
void SessionLogger::setMeasurementFlushIntervalSeconds (double seconds) noexcept
{
    flushIntervalSeconds = seconds;
}

int64_t SessionLogger::getMeasurementRowsWritten() const noexcept
{
    return measurementWriter.getRowsWritten();
}

int64_t SessionLogger::getEventRowsWritten() const noexcept
{
    return eventWriter.getRowsWritten();
}

//==============================================================================
void SessionLogger::reportErrorOnce (const juce::String& message)
{
    bool alreadyReported = message.contains ("measurements.csv") ? measurementErrorReported
                                                                  : eventErrorReported;
    if (alreadyReported)
        return;

    if (message.contains ("measurements.csv"))
        measurementErrorReported = true;
    else
        eventErrorReported = true;

    if (onLoggingError != nullptr)
        onLoggingError (message);
}

} // namespace pnam

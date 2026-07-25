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

    if (! measurementWriter.open (measurementsFile, getMeasurementCsvHeader()))
    {
        reportErrorOnce ("Could not open measurements.csv for writing.");
        return false;
    }

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
    row.add (formatDb (m.refPeakDb));
    row.add (formatDb (m.refRmsDb));
    row.add (formatDb (m.inPeakDbL));
    row.add (formatDb (m.inPeakDbR));
    row.add (formatDb (m.inRmsDbL));
    row.add (formatDb (m.inRmsDbR));
    row.add (formatDb (m.gainDiffDb));
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
    row.add (juce::String (toString (e.channel)));
    row.add (juce::String (e.measuredValue, 3));
    row.add (juce::String (e.expectedValue, 3));
    row.add (juce::String (e.difference, 3));
    row.add (juce::String (e.thresholdUsed, 3));
    row.add (juce::String (e.code));
    row.add (description);

    if (! eventWriter.writeRow (row) || eventWriter.hadWriteError())
        reportErrorOnce ("Failed writing to events.csv");

    eventWriter.flush();

    if (eventWriter.hadWriteError())
        reportErrorOnce ("Failed flushing events.csv");
}

//==============================================================================
juce::String SessionLogger::composeDescription (const Event& e)
{
    juce::String channelText = juce::String (toString (e.channel));

    switch (e.type)
    {
        case EventType::CompleteDropout:
        {
            if (e.code == eventCodeStart)
                return "Dropout started (input " + juce::String (e.measuredValue, 1)
                        + " dB, expected " + juce::String (e.expectedValue, 1) + " dB)";
            if (e.code == eventCodeEnd)
                return "Dropout ended after " + juce::String (e.measuredValue, 2)
                        + " s (input " + juce::String (e.difference, 1)
                        + " dB, expected " + juce::String (e.expectedValue, 1) + " dB)";
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

        case EventType::LoggingError:
            return "Logging error";

        case EventType::Other:
        default:
            return "Event";
    }
}

//==============================================================================
juce::StringArray SessionLogger::getMeasurementCsvHeader()
{
    return { "timestamp_utc", "elapsed_s", "sample_rate", "buffer_size",
             "ref_peak_db", "ref_rms_db",
             "in_peak_db_l", "in_peak_db_r", "in_rms_db_l", "in_rms_db_r",
             "gain_diff_db", "correlation", "dropout_flag", "clip_count", "status_code" };
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

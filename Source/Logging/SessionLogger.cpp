/*
  ==============================================================================
    SessionLogger.cpp
    Pink Noise Audio Monitor (pnam) — per-session CSV logging implementation.

    MESSAGE THREAD ONLY.
  ==============================================================================
*/

#include "SessionLogger.h"
#include <cmath>

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

    /** How far a derived timestamp may sit from the real wall clock before the
        anchor is STEPPED. Well above drain latency and timer jitter; small
        enough that a sleep or a stalled device is caught on the first row after
        it. Ordinary drift never gets here: it is slewed away (below). */
    constexpr double maxClockDivergenceSeconds = 2.0;

    /** A divergence seen on a record that is not known to be current has to be
        seen twice, this far apart, before it is acted on. After a
        message-thread stall the queued rows all arrive at once and each LOOKS
        late; only the newest is current, and it clears the flag. */
    constexpr double divergenceConfirmMillis = 250.0;

    /** A record this close to the live audio clock is current: its stall, if
        any, is real, and it re-anchors on first sight. */
    constexpr double currentRecordWindowSeconds = 0.5;

    /** setLiveAudioClock() is expected every timer tick; a value older than
        this is not trusted. */
    constexpr double liveClockMaxAgeMillis = 1000.0;

    /** How fast the anchor may be slewed, as a fraction of audio-clock time
        elapsed. The gentle rate is 50x a bad crystal (10 ppm reaches 2 s in
        55 hours if nothing absorbs it); the brisk one closes the second or so
        a device restart costs within a couple of minutes; the last is for an
        audio clock that is seconds AHEAD, which may only ever be slewed —
        stepping it would run timestamp_utc backwards. All are far below 1, so
        consecutive rows always move forwards. */
    constexpr double gentleSlewRate = 0.0005;
    constexpr double briskSlewRate  = 0.01;
    constexpr double aheadSlewRate  = 0.05;
    constexpr double briskSlewAboveSeconds = 0.1;

    /** Interval between attempts to bring a failed CSV back. */
    constexpr double writeRetryIntervalMillis = 3000.0;
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

    // Fresh session, fresh error state — reset BEFORE anything below can fail,
    // or a failed start would leave the flags set and the next failure silent.
    measurementErrorReported = false;
    eventErrorReported = false;
    measurementsFailed = false;
    eventsDirty = false;
    measurementRowsSinceFlush = 0;
    measurementRowsLost = 0;
    measurementRowsLostTotal = 0;
    measurementRowsDiscarded = 0;
    eventRowsOnDisk = 0;
    lastElapsedSeen = 0.0;
    measurementsReopened = false;

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

    // Provisional anchor (audio-clock zero = startTimeUtc), replaced by the
    // real wall clock as soon as the first current record is written — or right
    // here, if the live audio clock is already being supplied.
    anchorWallMs = (double) activeInfo.startTimeUtc.toMilliseconds();
    anchorElapsed = 0.0;
    anchorTaken = false;
    lastSlewElapsed = 0.0;
    divergencePending = false;
    aheadReported = false;
    anyStamped = false;

    if (liveClockKnown
        && (juce::Time::getMillisecondCounterHiRes() - liveClockSetMillis) <= liveClockMaxAgeMillis)
    {
        anchorWallMs = (double) juce::Time::currentTimeMillis();
        anchorElapsed = liveClockElapsed;
        lastSlewElapsed = liveClockElapsed;
        anchorTaken = true;
    }

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
    // Unsigned: the default seed is above 2^63 and used to be written as a
    // negative number that could not be typed back in to replay the session.
    text << "pinkNoiseSeed: "     << juce::String (activeInfo.pinkNoiseSeed) << "\n";
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
    lastFlushMillis = juce::Time::getMillisecondCounterHiRes();

    return true;
}

//==============================================================================
void SessionLogger::stopSession()
{
    if (! active)
        return;

    // Last chance to bring a failed file back before it is closed.
    retryFailedWrites (lastElapsedSeen, true);

    measurementWriter.flush();
    eventWriter.flush();

    // Append a stop line to session_info.txt.
    auto infoFile = activeInfo.logFolder.getChildFile ("session_info.txt");
    if (infoFile.existsAsFile())
    {
        juce::String stopLine;
        stopLine << "stopTimeUtc: " << juce::Time::getCurrentTime().toISO8601 (true) << "\n";
        stopLine << "measurementRowsWritten: " << (juce::int64) getMeasurementRowsWritten() << "\n";
        stopLine << "eventRowsWritten: " << (juce::int64) eventRowsOnDisk << "\n";

        if (measurementRowsLostTotal > 0)
            stopLine << "measurementRowsLost: " << (juce::int64) measurementRowsLostTotal << "\n";

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
bool SessionLogger::hasUnrecoveredWriteError() const noexcept
{
    return active && (measurementsFailed || eventsDirty);
}

//==============================================================================
void SessionLogger::setLiveAudioClock (double elapsedSecondsNow) noexcept
{
    liveClockElapsed = elapsedSecondsNow;
    liveClockSetMillis = juce::Time::getMillisecondCounterHiRes();
    liveClockKnown = true;
}

juce::String SessionLogger::makeTimestampUtc (double elapsedSeconds)
{
    double stampMs = anchorWallMs + (elapsedSeconds - anchorElapsed) * 1000.0;

    // Monotonic in elapsed time against the newest record already stamped:
    // moving the anchor (a slew, or the first real anchor replacing the
    // provisional one) must never date a later record before an earlier one.
    if (anyStamped)
        stampMs = elapsedSeconds >= newestStampedElapsed ? juce::jmax (stampMs, newestStampMs)
                                                         : juce::jmin (stampMs, newestStampMs);

    if (anchorTaken && (! anyStamped || elapsedSeconds >= newestStampedElapsed))
    {
        newestStampedElapsed = elapsedSeconds;
        newestStampMs = stampMs;
        anyStamped = true;
    }

    return juce::Time ((juce::int64) std::llround (stampMs)).toISO8601 (true);
}

void SessionLogger::checkClock (double elapsedSeconds, bool stampedNow)
{
    const double wallNowMs = (double) juce::Time::currentTimeMillis();
    const double nowMillis = juce::Time::getMillisecondCounterHiRes();

    const bool haveLive = liveClockKnown && (nowMillis - liveClockSetMillis) <= liveClockMaxAgeMillis;

    // Without a live clock all there is to go on is the caller's word that this
    // record carries the current audio-clock time. Back-dated records say
    // nothing about "now".
    if (! haveLive && ! stampedNow)
        return;

    // What the audio clock reads now, and whether this record is known to be
    // current (as opposed to a row that sat in a queue while something stalled).
    const double audioNow = haveLive ? juce::jmax (liveClockElapsed, elapsedSeconds) : elapsedSeconds;
    const bool recordIsCurrent = haveLive && (audioNow - elapsedSeconds) <= currentRecordWindowSeconds;

    if (! anchorTaken)
    {
        anchorWallMs = wallNowMs;
        anchorElapsed = audioNow;
        lastSlewElapsed = audioNow;
        anchorTaken = true;
        return;
    }

    // > 0: the audio clock is behind the wall clock.
    const double divergence = (wallNowMs - (anchorWallMs + (audioNow - anchorElapsed) * 1000.0)) / 1000.0;

    //--------------------------------------------------------------------
    // Far behind: the audio clock stood still (sleep, a stalled or lost
    // device). Step the anchor forward and put the jump on record. A current
    // record does this on first sight, so it is itself stamped correctly; a
    // backlog row, or any row when no live clock is supplied, has to be seen
    // diverged twice (it keeps the old anchor, which is right for a row from
    // before the stall).
    if (divergence > maxClockDivergenceSeconds)
    {
        if (! recordIsCurrent)
        {
            if (! divergencePending)
            {
                divergencePending = true;
                divergenceFirstSeenMillis = nowMillis;
                return;
            }

            if (nowMillis - divergenceFirstSeenMillis < divergenceConfirmMillis)
                return;
        }

        anchorWallMs = wallNowMs;
        anchorElapsed = audioNow;
        lastSlewElapsed = audioNow;
        divergencePending = false;
        aheadReported = false;

        logInternalEvent (EventType::Other, Severity::Info, audioNow, divergence,
                          "Audio clock and wall clock diverged by " + juce::String (divergence, 1)
                            + " s (audio clock behind: sleep or device stall); timestamps re-anchored to the wall clock");
        return;
    }

    divergencePending = false;

    //--------------------------------------------------------------------
    // Everything else is slewed: the anchor moves towards the wall clock by a
    // small fraction of the audio time that has passed, so ordinary drift is
    // absorbed as it arises and timestamp_utc never has to jump — in
    // particular never backwards, which is what a step would do whenever the
    // interface clock runs fast.
    const double advance = audioNow - lastSlewElapsed;

    if (advance > 0.0)
    {
        const bool farAhead = divergence < -maxClockDivergenceSeconds;
        const double rate = farAhead ? aheadSlewRate
                                     : (std::abs (divergence) > briskSlewAboveSeconds ? briskSlewRate : gentleSlewRate);
        const double limitMs = rate * advance * 1000.0;

        anchorWallMs += juce::jlimit (-limitMs, limitMs, divergence * 1000.0);
        lastSlewElapsed = audioNow;

        if (farAhead && ! aheadReported)
        {
            aheadReported = true;

            logInternalEvent (EventType::Other, Severity::Info, audioNow, divergence,
                              "Audio clock and wall clock diverged by " + juce::String (-divergence, 1)
                                + " s (audio clock ahead: system time changed?); timestamps are being slowed"
                                  " until they agree, not stepped back");
        }
        else if (divergence > -0.5 * maxClockDivergenceSeconds)
        {
            aheadReported = false;
        }
    }
}

//==============================================================================
void SessionLogger::logMeasurement (const Measurement& m)
{
    if (! active)
        return;

    lastElapsedSeen = juce::jmax (lastElapsedSeen, m.elapsedSeconds);

    // A measurement is stamped "now" on the audio clock, so it is the regular
    // check of that clock against the wall clock (before this row is stamped).
    checkClock (m.elapsedSeconds, true);
    retryFailedWrites (m.elapsedSeconds, false);

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

        // Latency and residual exist only while the return is matched to its
        // reference; an empty cell means "not aligned", never "zero".
        const bool aligned = valid && m.artifactAligned[ch];
        row.add (aligned ? juce::String (m.latencyMs[ch], 3) : juce::String());
        row.add (aligned ? formatDb (m.residualDb[ch])        : juce::String());
        row.add (valid ? juce::String (m.clickCount[ch])      : juce::String());
    }

    row.add (formatCorrelation (m.correlation));
    row.add (juce::String (m.dropoutFlag));
    row.add (juce::String (m.clipCountBlock));
    row.add (juce::String (m.statusCode));

    // While the file is down the row has nowhere to go; it is counted so the
    // recovery row can say how many are missing.
    if (measurementsFailed)
    {
        // Reopened since the failure? Then THIS row is the test: the file only
        // counts as recovered once a real row has been written and flushed.
        // Reopening proves nothing (it succeeds on a full disk, and so does
        // flushing an empty buffer), and taking its word for it produced a
        // "being written again" row and alert every retry for as long as the
        // failure lasted.
        if (measurementsReopened)
        {
            measurementsReopened = false;

            if (measurementWriter.writeRow (row))
            {
                measurementWriter.flush();

                if (! measurementWriter.hadWriteError())
                {
                    const auto lost = measurementRowsLost;

                    measurementsFailed = false;
                    measurementErrorReported = false;
                    measurementRowsLost = 0;
                    measurementRowsSinceFlush = 0;
                    lastFlushMillis = juce::Time::getMillisecondCounterHiRes();

                    logInternalEvent (EventType::LoggingError, lost > 0 ? Severity::Warning : Severity::Info,
                                      m.elapsedSeconds, (double) lost,
                                      "measurements.csv is being written again after an error; up to "
                                        + juce::String ((juce::int64) lost) + " rows lost");
                    return;
                }

                ++measurementRowsDiscarded;   // accepted by the writer, lost with its buffer
            }
        }

        ++measurementRowsLost;
        ++measurementRowsLostTotal;
        return;
    }

    if (! measurementWriter.writeRow (row))
    {
        ++measurementRowsLost;
        ++measurementRowsLostTotal;
        noteMeasurementFailure ("Failed writing to measurements.csv");
        return;
    }

    ++measurementRowsSinceFlush;

    // HiRes counter (a double): the 32-bit millisecond counter wraps after 49.7
    // days of uptime, and a wrap mid-session used to stop these flushes.
    const double nowMillis = juce::Time::getMillisecondCounterHiRes();
    if ((nowMillis - lastFlushMillis) >= flushIntervalSeconds * 1000.0)
    {
        measurementWriter.flush();
        lastFlushMillis = nowMillis;

        if (measurementWriter.hadWriteError())
            noteMeasurementFailure ("Failed flushing measurements.csv");
        else
            measurementRowsSinceFlush = 0;
    }
}

void SessionLogger::noteMeasurementFailure (const juce::String& message)
{
    // Rows accepted since the last good flush were only buffered, and a failed
    // stream discards its buffer: count them as lost too.
    measurementRowsLost      += measurementRowsSinceFlush;
    measurementRowsLostTotal += measurementRowsSinceFlush;
    measurementRowsDiscarded += measurementRowsSinceFlush;
    measurementRowsSinceFlush = 0;

    measurementsFailed = true;
    lastRetryMillis = juce::Time::getMillisecondCounterHiRes();

    reportErrorOnce (message);
}

void SessionLogger::retryFailedWrites (double elapsedSeconds, bool force)
{
    if (! measurementsFailed && ! eventsDirty)
        return;

    const double nowMillis = juce::Time::getMillisecondCounterHiRes();

    if (! force && (nowMillis - lastRetryMillis) < writeRetryIntervalMillis)
        return;

    lastRetryMillis = nowMillis;

    // events.csv first, so the recovery rows below have somewhere to go. Every
    // event of the session is still in writtenEventRows, so a successful
    // rewrite brings the file fully up to date: nothing is lost.
    if (eventsDirty && rewriteEventsFile())
    {
        eventErrorReported = false;
        logInternalEvent (EventType::LoggingError, Severity::Info, elapsedSeconds, 0.0,
                          "events.csv is being written again after an error; no events were lost");
    }

    // measurements.csv: reopen only. Whether it really works again is decided
    // by the next measurement row (see logMeasurement); until then it stays
    // failed, and nothing is announced.
    if (measurementsFailed)
        measurementsReopened = measurementWriter.reopenForAppend (getMeasurementCsvHeader (activeInfo));
}

//==============================================================================
void SessionLogger::logEvent (const Event& e, const juce::String& description)
{
    if (! active)
        return;

    lastElapsedSeen = juce::jmax (lastElapsedSeen, e.elapsedSeconds);

    // Instant events (markers, device lost/restarted, settings changes) carry
    // the current audio-clock time. Start/End events are back-dated to when the
    // condition began or cleared; they are only checked against the live audio
    // clock, when one has been supplied.
    checkClock (e.elapsedSeconds, e.code == eventCodeInstant);

    retryFailedWrites (e.elapsedSeconds, false);

    appendEventRow (makeEventRow (e, description));
}

juce::StringArray SessionLogger::makeEventRow (const Event& e, const juce::String& description)
{
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
    row.add (juce::String());   // note — user-entered later via amendEventNote
    return row;
}

void SessionLogger::appendEventRow (const juce::StringArray& row)
{
    // Kept whether or not the write succeeds: this copy is what a failed
    // events.csv is rebuilt from, so an event is never lost to a write error.
    writtenEventRows.push_back (row);

    const bool written = eventWriter.writeRow (row);

    if (written)
        eventWriter.flush();

    if (! written || eventWriter.hadWriteError())
    {
        if (! eventsDirty)
            lastRetryMillis = juce::Time::getMillisecondCounterHiRes();

        eventsDirty = true;
        reportErrorOnce ("Failed writing to events.csv");
        return;
    }

    ++eventRowsOnDisk;
}

void SessionLogger::logInternalEvent (EventType type, Severity severity, double elapsedSeconds,
                                      double measuredValue, const juce::String& description)
{
    Event e {};
    e.type           = type;
    e.severity       = severity;
    e.channel        = ChannelId::None;
    e.elapsedSeconds = elapsedSeconds;
    e.measuredValue  = measuredValue;
    e.code           = eventCodeInstant;

    appendEventRow (makeEventRow (e, description));

    if (onInternalEvent != nullptr)
        onInternalEvent (e, description);
}

bool SessionLogger::rewriteEventsFile()
{
    const auto failed = [this]
    {
        if (! eventsDirty)
            lastRetryMillis = juce::Time::getMillisecondCounterHiRes();

        eventsDirty = true;   // the file no longer matches writtenEventRows; retried
        reportErrorOnce ("Could not rewrite events.csv");
        return false;
    };

    // Build the complete new file NEXT TO the real one and swap it in in one
    // step. Deleting events.csv and writing it again in place meant a crash, a
    // full disk or a pulled drive part-way through destroyed the event log.
    juce::TemporaryFile temp (eventsFilePath);

    {
        CsvWriter tempWriter;

        if (! tempWriter.open (temp.getFile(), getEventCsvHeader()))
            return failed();

        for (const auto& r : writtenEventRows)
            tempWriter.writeRow (r);

        tempWriter.flush();

        if (tempWriter.hadWriteError())
            return failed();
    }

    // Up to here events.csv has not been touched and its writer is still open.
    eventWriter.close();

    if (! temp.overwriteTargetFileWithTemporary())
    {
        eventWriter.reopenForAppend (getEventCsvHeader());   // carry on with the old file
        return failed();
    }

    eventRowsOnDisk = (int64_t) writtenEventRows.size();

    if (! eventWriter.reopenForAppend (getEventCsvHeader()))
        return failed();

    eventsDirty = false;
    return true;
}

bool SessionLogger::amendEventNote (const Event& e, const juce::String& note)
{
    if (! active)
        return false;

    const auto elapsedText = formatSeconds (e.elapsedSeconds);
    const auto typeText    = juce::String (toString (e.type));
    const auto channelText = e.channelIndex >= 0
                               ? channelName (activeInfo, e.channelIndex, e.channelIsInput)
                               : juce::String (toString (e.channel));
    const auto codeText    = juce::String (e.code);

    for (auto it = writtenEventRows.rbegin(); it != writtenEventRows.rend(); ++it)
    {
        auto& row = *it;

        if (row.size() >= 12 && row[1] == elapsedText && row[2] == typeText
            && row[4] == channelText && row[9] == codeText)
        {
            row.set (11, note);
            return rewriteEventsFile();
        }
    }

    return false;
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

        if (row.size() >= 12 && row[1] == elapsedText && row[2] == typeText)
        {
            row.set (10, newDescription);   // column 10 = description, 11 = note
            return rewriteEventsFile();
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
            // The loss is Critical. The engine reports the lost device coming
            // back by itself as an Info row of the same type — an instant event
            // of its own, not an "end" (the loss has no "start" to pair it
            // with) — with the wall-clock outage in measuredValue.
            if (e.severity == Severity::Info)
                return "Audio device restarted after " + juce::String (e.measuredValue, 1)
                        + " s; output resumed";
            return "Audio device lost";

        case EventType::AudioSettingsChanged:
            // MainComponent supplies a specific description naming what changed;
            // this is the fallback when the event is composed from the POD alone.
            return "Audio settings changed";

        case EventType::LoggingError:
            return "Logging error";

        case EventType::Click:
        {
            // difference = how long it lasted (ms): one sample is a tick, tens
            // of milliseconds is a burst of something that was not the signal.
            if (e.severity == Severity::Info)
            {
                // The periodic summary of clicks too small to be events:
                // expectedValue carries how many since the last summary.
                const int count = juce::roundToInt (e.expectedValue);
                return juce::String (count) + (count == 1 ? " small click on " : " small clicks on ") + channelText
                        + " since last reported: largest peak " + juce::String (e.measuredValue, 1) + " dBFS, "
                        + juce::String (e.difference, e.difference < 1.0 ? 2 : 1)
                        + " ms. Too small to log one by one (below " + juce::String (e.thresholdUsed, 1)
                        + " dBFS here); every one is in click_count in measurements.csv";
            }

            const auto kind = e.difference >= 5.0 ? "Burst" : "Click";
            return juce::String (kind) + " on " + channelText + ": peak " + juce::String (e.measuredValue, 1)
                    + " dBFS, " + juce::String (e.difference, e.difference < 1.0 ? 2 : 1)
                    + " ms (residual normally " + juce::String (e.expectedValue, 1) + " dBFS)";
        }

        case EventType::Noise:
        case EventType::Distortion:
        {
            const juce::String what = e.type == EventType::Distortion
                                        ? "Distortion (residual follows the signal's peaks: clipping or saturation in the path)"
                                        : "Noise (static, hiss, crackle or buzz added to the signal)";

            if (e.code == eventCodeStart)
                return what + " started on " + channelText + ": residual " + juce::String (e.measuredValue, 1)
                        + " dB vs normal " + juce::String (e.expectedValue, 1) + " dB";
            if (e.code == eventCodeEnd)
                return juce::String (e.type == EventType::Distortion ? "Distortion" : "Noise") + " ended on " + channelText
                        + " after " + juce::String (e.measuredValue, 2) + " s (worst residual "
                        + juce::String (e.difference, 1) + " dB)";

            // Instant: the return stopped matching the reference and was found
            // again at the same delay.
            return "Corrupted audio on " + channelText + ": the return did not match the reference for up to "
                    + juce::String (e.difference, 2) + " s";
        }

        case EventType::TimingSlip:
        {
            const double samples = e.measuredValue;
            return "Timing slip on " + channelText + ": about " + juce::String (std::abs (samples), std::abs (samples) < 10.0 ? 1 : 0)
                    + " samples (" + juce::String (std::abs (e.difference), 3) + " ms) "
                    + (samples < 0.0 ? "dropped" : "repeated or inserted")
                    + "; path delay was " + juce::String (e.expectedValue, 1) + " samples";
        }

        case EventType::SignalMismatch:
        {
            if (e.code == eventCodeStart)
                return "Signal mismatch started on " + channelText
                        + ": the return is present but is not the reference (wrong routing, another source, or destroyed audio)";
            if (e.code == eventCodeEnd)
                return "Signal mismatch ended on " + channelText + " after " + juce::String (e.measuredValue, 2) + " s";
            return "Signal mismatch (" + channelText + ")";
        }

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
        header.add (n + "_latency_ms");
        header.add (n + "_residual_db");
        header.add (n + "_click_count");
    }

    header.addArray ({ "correlation", "dropout_flag", "clip_count", "status_code" });
    return header;
}

juce::StringArray SessionLogger::getEventCsvHeader()
{
    return { "timestamp_utc", "elapsed_s", "event_type", "severity", "channel",
             "measured_value", "expected_value", "difference", "threshold_used",
             "code", "description", "note" };
}

//==============================================================================
void SessionLogger::setMeasurementFlushIntervalSeconds (double seconds) noexcept
{
    flushIntervalSeconds = seconds;
}

int64_t SessionLogger::getMeasurementRowsWritten() const noexcept
{
    // The writer counts rows it accepted; some of those never left its buffer.
    return measurementWriter.getRowsWritten() - measurementRowsDiscarded;
}

int64_t SessionLogger::getEventRowsWritten() const noexcept
{
    return eventRowsOnDisk;
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

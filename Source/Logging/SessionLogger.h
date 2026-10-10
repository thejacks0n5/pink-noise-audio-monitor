#pragma once
/*
  ==============================================================================
    SessionLogger.h
    Pink Noise Audio Monitor (pnam) — per-session CSV logging.

    MESSAGE THREAD ONLY. The SessionLogger never touches the audio thread; it
    is fed by the UI-owned juce::Timer (~30 Hz) which each tick does:

        measurementQueue.popAll ([&] (const Measurement& m) {
            sessionLogger.logMeasurement (m);          // if logging active
            // also update meters/plots
        });
        eventQueue.popAll ([&] (const Event& e) {
            auto desc = SessionLogger::composeDescription (e);
            sessionLogger.logEvent (e, desc);          // if logging active
            // also append to live log window, maybe notify
        });

    Note the deliberate decoupling in the product: "Start/Stop Audio" runs the
    engine and drains the queues for the live UI regardless; "Start/Stop
    Logging" (startSession/stopSession here) controls only whether drained
    records are also written to CSV.

    Session folder layout (created by startSession under the chosen base):
        <baseFolder>/<sessionId>/                e.g. .../20260725_231502/
            measurements.csv
            events.csv
            session_info.txt      (key: value dump of SessionInfo)

    CSV schemas:

      measurements.csv columns are DERIVED from the session's channel counts and
      user channel names (see getMeasurementCsvHeader):
        timestamp_utc, elapsed_s, sample_rate, buffer_size,
        <out name>_peak_db, <out name>_rms_db,          (per output)
        <in name>_peak_db, <in name>_rms_db,
        <in name>_gain_diff_db, <in name>_clip_count,   (per MONITORED input)
        correlation, dropout_flag, clip_count, status_code
      Unmonitored inputs contribute no columns. The layout is fixed for the
      lifetime of a session (it is taken from the SessionInfo snapshot), so a
      mid-session device change cannot shift columns.

      events.csv columns:
        timestamp_utc, elapsed_s, event_type, severity, channel,
        measured_value, expected_value, difference, threshold_used,
        code, description, note
      (note is user-entered after the fact — "changing patch cable here" — and
      is filled in by amendEventNote rewriting the file)

      elapsed_s is the audio clock (samples processed), printed with 3
      decimals. timestamp_utc is wall time, derived on the message thread from
      an anchor pair: anchorWall + (record.elapsedSeconds - anchorElapsed),
      ISO 8601. The anchor is taken from the wall clock when the first current
      record of the session is written. The audio clock is not a wall clock —
      it stands still while the machine sleeps or the device is stalled, and
      drifts by a few ppm otherwise — so it is checked against the real time
      as records are written:
        - ordinary drift is absorbed by SLEWING the anchor a hair per record
          (at most 0.5 ms per second of audio), so it never accumulates;
        - if the audio clock has fallen more than 2 s BEHIND (sleep, a stalled
          or lost device) the anchor is stepped forward to "now" and ONE Info
          row ("Audio clock and wall clock diverged by X s ...") is written to
          events.csv, so the jump is explained in the log rather than silently
          shifting every later timestamp;
        - if it is more than 2 s AHEAD (system time changed) there is one Info
          row and the anchor is slewed faster, never stepped.
      timestamp_utc therefore never runs backwards. Call setLiveAudioClock()
      once per timer tick: knowing the audio clock's current value is what lets
      the first record after a stall be stamped correctly, instead of the
      stall only being noticed on the record after it. dB values are printed
      with 2 decimals; correlation with 3.

    Flush policy: measurements.csv is flushed at least every 5 seconds;
    events.csv is flushed after every row (events are precious).
    Write errors surface via onLoggingError so the app can raise a
    LoggingError event and a notification — and are then retried, not
    latched: every few seconds the failed file is reopened (measurements.csv
    for append; events.csv rewritten whole from the in-memory copy, so no
    event is lost). A reopen proves nothing by itself — opening a file on a
    full disk succeeds — so measurements.csv only counts as recovered once a
    real row has been written to it and flushed; that is when a LoggingError
    row records the recovery and how many measurement rows were lost.
  ==============================================================================
*/

#include <functional>
#include <vector>
#include <juce_core/juce_core.h>

#include "../Model/DataModel.h"
#include "CsvWriter.h"

namespace pnam
{

//==============================================================================
class SessionLogger
{
public:
    SessionLogger() = default;

    /** Stops the session (finalising files) if still active. */
    ~SessionLogger();

    SessionLogger (const SessionLogger&) = delete;
    SessionLogger& operator= (const SessionLogger&) = delete;

    //==========================================================================
    /** Creates `<baseFolder>/<info.sessionId>/`, opens both CSVs with their
        headers, and writes session_info.txt.
        @param baseFolder  parent directory (created if missing).
        @param info        session metadata; `info.logFolder` is ignored on
                           input and the created folder is recorded in the
                           internally stored copy (see getSessionInfo()).
        @returns false (and stays inactive) if the folder or either CSV could
                 not be created. */
    bool startSession (const juce::File& baseFolder, const SessionInfo& info);

    /** Flushes and closes both CSVs and appends a stop line to
        session_info.txt. Safe to call when inactive (no-op). */
    void stopSession();

    bool isSessionActive() const noexcept;

    /** Copy of the active session's metadata (logFolder filled in).
        Only meaningful while isSessionActive(). */
    SessionInfo getSessionInfo() const;

    /** The per-session folder, or an invalid File when inactive. */
    juce::File getSessionFolder() const;

    //==========================================================================
    /** MESSAGE THREAD. Tells the logger what the audio clock reads NOW
        (AnalysisProcessor::getElapsedSeconds()). Call it once per timer tick,
        before draining the queues, whether or not a session is active.

        Every record carries its own elapsedSeconds, but not whether that is
        current: a start/end event is back-dated, and rows drained after a
        message-thread stall are seconds old. Without this the logger has to see
        a divergence twice before believing it, so the first record after a real
        stall is dated by the stale anchor. With it, a record within 0.5 s of
        the live clock re-anchors on first sight and only older (backlog)
        records wait for the second sighting. */
    void setLiveAudioClock (double elapsedSecondsNow) noexcept;

    /** Appends one row to measurements.csv (no-op when inactive).
        Called from the UI timer's measurement drain. */
    void logMeasurement (const Measurement& m);

    /** Appends one row to events.csv and flushes (no-op when inactive).
        @param description  human-readable text, typically from
                            composeDescription(); quoted/escaped by CsvWriter. */
    void logEvent (const Event& e, const juce::String& description);

    /** Replaces the description of an already-written event (used to name a
        manual marker after the fact) and rewrites events.csv so the file
        carries the final name in a single clean row. Matches on elapsed time
        and type; @returns false if no such row exists or the session is over.
        MESSAGE THREAD; the file is small, so the rewrite is cheap. The rewrite
        goes to a temporary file that then replaces events.csv in one step, so
        a crash or a full disk part-way through cannot destroy the event log. */
    bool amendEventDescription (double elapsedSeconds, EventType type,
                                const juce::String& newDescription);

    /** Sets the note column of an already-written event row and rewrites the
        file. The full Event identifies the row: three inputs can fault in the
        same block, so elapsed+type alone is ambiguous — channel and code
        complete the match. @returns false if no row matches. MESSAGE THREAD. */
    bool amendEventNote (const Event& e, const juce::String& note);

    //==========================================================================
    /** Builds the standard human-readable description for an event, e.g.
        "Dropout ended after 2.35 s (input -78.2 dB, expected -21.0 dB)".
        Static and pure — also used by the live log window and the
        NotificationEngine for consistent wording. */
    static juce::String composeDescription (const Event& e);

    /** Column layout depends on the session's channel count and names, so the
        header is derived from the SessionInfo rather than being fixed. Per-channel
        columns are emitted as "<name>_peak_db", "<name>_rms_db" (and
        "<name>_gain_diff_db" for inputs), outputs first, then inputs. */
    static juce::StringArray getMeasurementCsvHeader (const SessionInfo&);
    static juce::StringArray getEventCsvHeader();

    /** Safe channel-name lookup with an "Out n"/"In n" fallback. */
    static juce::String channelName (const SessionInfo&, int channelIndex, bool isInput);

    /** Whether an input appears in measurements.csv at all. Unmonitored inputs
        are omitted entirely — both header and rows consult this, so the two can
        never disagree about the column layout. */
    static bool isInputLogged (const SessionInfo&, int channelIndex);

    //==========================================================================
    /** Invoked (on the message thread) when a CSV write/flush fails after
        startSession succeeded. The app uses this to raise a LoggingError
        event/notification. Fired once per file per failure: it can fire again
        for a file that failed, recovered and failed again. */
    std::function<void (const juce::String& errorMessage)> onLoggingError;

    /** MESSAGE THREAD. Fired after the logger writes a row it originated itself
        (clock re-anchor, write recovery), with the same Event and description
        that went into events.csv, so the on-screen log can show it too. */
    std::function<void (const Event&, const juce::String& description)> onInternalEvent;

    /** True while either CSV is in a failed state and being retried. Goes back
        to false when the file recovers (the UI can use this to clear a
        write-error indicator). */
    bool hasUnrecoveredWriteError() const noexcept;

    /** Seconds between measurement-file flushes (default 5.0). */
    void setMeasurementFlushIntervalSeconds (double seconds) noexcept;

    /** Rows written so far in this session (measurements, events). */
    int64_t getMeasurementRowsWritten() const noexcept;
    int64_t getEventRowsWritten() const noexcept;

private:
    //==========================================================================
    /** anchorWall + (elapsedSeconds - anchorElapsed) -> ISO 8601 string with
        millis. Never earlier than the stamp already given to a record with a
        smaller or equal elapsedSeconds. */
    juce::String makeTimestampUtc (double elapsedSeconds);

    /** Checks the audio clock against the wall clock before a record is
        stamped: slews the anchor for ordinary drift, steps it forward (and logs
        one Info row) after a stall. @param stampedNow  the caller's belief that
        this record's elapsedSeconds is current (measurement rows, instant
        events) — only used when no live clock has been supplied. */
    void checkClock (double elapsedSeconds, bool stampedNow);

    void reportErrorOnce (const juce::String& message);

    /** Formats one events.csv row (stamped with the current anchor). */
    juce::StringArray makeEventRow (const Event& e, const juce::String& description);

    /** Keeps the row in writtenEventRows and appends it to events.csv. A failed
        write marks the file dirty; the row is not lost (see retryFailedWrites). */
    void appendEventRow (const juce::StringArray& row);

    /** Writes a row the logger itself originates (clock re-anchor, recovery). */
    void logInternalEvent (EventType type, Severity severity, double elapsedSeconds,
                           double measuredValue, const juce::String& description);

    /** Writes every row in writtenEventRows to a temporary file and swaps it in
        for events.csv atomically, then reopens the writer for append. Shared by
        the two amend methods and by error recovery. */
    bool rewriteEventsFile();

    /** Marks measurements.csv failed, counting the rows that were only
        buffered as lost. */
    void noteMeasurementFailure (const juce::String& message);

    /** Every few seconds while a file is in a failed state, tries to bring it
        back. @param force  ignore the retry interval (used at stopSession). */
    void retryFailedWrites (double elapsedSeconds, bool force);

    CsvWriter   measurementWriter;
    CsvWriter   eventWriter;

    // Every events.csv row written this session, kept so a row can be amended
    // and the file rewritten. Events are rare (two per fault), so this stays
    // tiny even overnight.
    std::vector<juce::StringArray> writtenEventRows;
    juce::File  eventsFilePath;
    SessionInfo activeInfo;
    bool        active { false };
    double      flushIntervalSeconds { 5.0 };
    double      lastFlushMillis { 0.0 };          // Time::getMillisecondCounterHiRes()
    bool        measurementErrorReported { false };
    bool        eventErrorReported       { false };

    // Wall-clock anchor for timestamp_utc (see the header comment). Until the
    // first current record arrives the anchor is (startTimeUtc, 0). Held in
    // milliseconds as a double so it can be slewed by fractions of one.
    double      anchorWallMs { 0.0 };
    double      anchorElapsed { 0.0 };
    bool        anchorTaken { false };
    double      lastSlewElapsed { 0.0 };          // audio clock at the last slew
    bool        divergencePending { false };      // seen once, awaiting confirmation
    double      divergenceFirstSeenMillis { 0.0 };
    bool        aheadReported { false };          // "audio clock ahead" row written for this episode

    // Newest record stamped so far (largest elapsedSeconds) and the stamp it
    // got: the guard that keeps timestamps monotonic in elapsed time.
    double      newestStampedElapsed { 0.0 };
    double      newestStampMs { 0.0 };
    bool        anyStamped { false };

    // From setLiveAudioClock().
    bool        liveClockKnown { false };
    double      liveClockElapsed { 0.0 };
    double      liveClockSetMillis { 0.0 };       // Time::getMillisecondCounterHiRes()

    // Write-error recovery.
    bool        measurementsFailed { false };     // measurements.csv not writable right now
    bool        measurementsReopened { false };   // ...but reopened, awaiting proof that a row can be written
    bool        eventsDirty { false };            // events.csv is behind writtenEventRows
    double      lastRetryMillis { 0.0 };
    int64_t     measurementRowsSinceFlush { 0 };  // accepted but not yet known to be on disk
    int64_t     measurementRowsLost { 0 };        // since the current failure began
    int64_t     measurementRowsLostTotal { 0 };   // whole session
    int64_t     measurementRowsDiscarded { 0 };   // accepted by the writer, then lost with its buffer
    int64_t     eventRowsOnDisk { 0 };
    double      lastElapsedSeen { 0.0 };          // newest record time, for rows written at stop
};

} // namespace pnam

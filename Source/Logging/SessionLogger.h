#pragma once
/*
  ==============================================================================
    SessionLogger.h
    Pink Noise Audio Monitor (pnam) — per-session CSV logging.

    MESSAGE THREAD ONLY. The SessionLogger never touches the audio thread; it
    is fed by the UI-owned juce::Timer (~30 Hz) which each tick does:

        measurementQueue.popAll ([&] (const Measurement& m) {
            sessionLogger.logMeasurement (m);          // if logging active
            /* also update meters/plots */ });
        eventQueue.popAll ([&] (const Event& e) {
            auto desc = SessionLogger::composeDescription (e);
            sessionLogger.logEvent (e, desc);          // if logging active
            /* also append to live log window, maybe notify */ });

    Note the deliberate decoupling in the product: "Start/Stop Audio" runs the
    engine and drains the queues for the live UI regardless; "Start/Stop
    Logging" (startSession/stopSession here) controls only whether drained
    records are also written to CSV.

    Session folder layout (created by startSession under the chosen base):
        <baseFolder>/<sessionId>/                e.g. .../20260725_231502/
            measurements.csv
            events.csv
            session_info.txt      (key: value dump of SessionInfo)

    CSV schemas (fixed contract — analysis scripts depend on these):

      measurements.csv columns:
        timestamp_utc, elapsed_s, sample_rate, buffer_size,
        ref_peak_db, ref_rms_db,
        in_peak_db_l, in_peak_db_r, in_rms_db_l, in_rms_db_r,
        gain_diff_db, correlation, dropout_flag, clip_count, status_code

      events.csv columns:
        timestamp_utc, elapsed_s, event_type, severity, channel,
        measured_value, expected_value, difference, threshold_used,
        code, description

      timestamp_utc is derived on the message thread as
      SessionInfo::startTimeUtc + record.elapsedSeconds, ISO 8601
      ("2026-07-25T23:15:02.481Z"). elapsed_s is printed with 3 decimals;
      dB values with 2; correlation with 3.

    Flush policy: measurements.csv is flushed at least every 5 seconds;
    events.csv is flushed after every row (events are precious).
    Write errors surface via onLoggingError so the app can raise a
    LoggingError event and a notification.
  ==============================================================================
*/

#include <functional>
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
    /** Appends one row to measurements.csv (no-op when inactive).
        Called from the UI timer's measurement drain. */
    void logMeasurement (const Measurement& m);

    /** Appends one row to events.csv and flushes (no-op when inactive).
        @param description  human-readable text, typically from
                            composeDescription(); quoted/escaped by CsvWriter. */
    void logEvent (const Event& e, const juce::String& description);

    //==========================================================================
    /** Builds the standard human-readable description for an event, e.g.
        "Dropout ended after 2.35 s (input -78.2 dB, expected -21.0 dB)".
        Static and pure — also used by the live log window and the
        NotificationEngine for consistent wording. */
    static juce::String composeDescription (const Event& e);

    /** Fixed CSV headers (exact contract; see file comment). */
    static juce::StringArray getMeasurementCsvHeader();
    static juce::StringArray getEventCsvHeader();

    //==========================================================================
    /** Invoked (on the message thread) when a CSV write/flush fails after
        startSession succeeded. The app uses this to raise a LoggingError
        event/notification. Fired at most once per session per file. */
    std::function<void (const juce::String& errorMessage)> onLoggingError;

    /** Seconds between measurement-file flushes (default 5.0). */
    void setMeasurementFlushIntervalSeconds (double seconds) noexcept;

    /** Rows written so far in this session (measurements, events). */
    int64_t getMeasurementRowsWritten() const noexcept;
    int64_t getEventRowsWritten() const noexcept;

private:
    //==========================================================================
    /** startTimeUtc + elapsedSeconds -> ISO 8601 UTC string with millis. */
    juce::String makeTimestampUtc (double elapsedSeconds) const;

    void reportErrorOnce (const juce::String& message);

    CsvWriter   measurementWriter;
    CsvWriter   eventWriter;
    SessionInfo activeInfo;
    bool        active { false };
    double      flushIntervalSeconds { 5.0 };
    juce::int64 lastFlushMillis { 0 };
    bool        measurementErrorReported { false };
    bool        eventErrorReported       { false };
};

} // namespace pnam

#pragma once
/*
  ==============================================================================
    DataModel.h
    Pink Noise Audio Monitor (pnam) — V1 core data model.

    This header defines the FIXED data contract shared between the audio
    thread, the lock-free FIFOs, the message-thread logger/UI, and the CSV
    files. Do not add non-trivially-copyable members to Measurement or Event:
    both structs travel through juce::AbstractFifo-backed queues and are
    memcpy'd by value on the real-time audio thread.

    Threading:
      - Measurement / Event: produced on the AUDIO thread, consumed on the
        MESSAGE thread. POD only. No juce::String, no heap, no destructors.
      - SessionInfo: MESSAGE thread only (never enters a FIFO), so it may use
        juce::String / juce::File freely.
  ==============================================================================
*/

#include <cstdint>
#include <type_traits>
#include <juce_core/juce_core.h>

namespace pnam
{

//==============================================================================
/** Category of a detected/logged event. Stable integer values — these are
    written to the event CSV as both a name and a numeric code, so never
    reorder or renumber existing entries; append only. */
enum class EventType : int32_t
{
    CompleteDropout = 0,  ///< Returned input fell to (near) silence while reference was active.
    Clipping        = 1,  ///< Input samples at/above the clip threshold.
    LevelChange     = 2,  ///< Sustained input/reference gain difference beyond tolerance.
    ManualMarker    = 3,  ///< User pressed "mark" in the UI (message thread origin).
    AudioDeviceLost = 4,  ///< Device stopped/changed/disappeared mid-session.
    LoggingError    = 5,  ///< CSV/file problem reported by SessionLogger (message thread origin).
    Other           = 6,
    /** User changed the audio device/rate/buffer/channels (message thread origin).
        Informational, not a fault: detection is suppressed across the
        reconfiguration so the resulting glitch is not logged as a dropout. */
    AudioSettingsChanged = 7
};

/** Number of EventType values — keep in step when appending. */
constexpr int numEventTypes = 8;

/** Severity used for UI colouring, CSV, and notification gating.
    Only Severity::Critical events may trigger desktop notifications. */
enum class Severity : int32_t
{
    Info     = 0,
    Warning  = 1,
    Critical = 2
};

/** Which channel(s) an event or value refers to. */
enum class ChannelId : int32_t
{
    Left  = 0,
    Right = 1,
    Both  = 2,
    None  = 3
};

//==============================================================================
/** Bit flags carried in Measurement::statusCode. Combine with bitwise OR. */
namespace StatusBits
{
    constexpr int32_t ok               = 0;        ///< Normal running block.
    constexpr int32_t outputMuted      = 1 << 0;   ///< Generator muted — comparisons suspended.
    constexpr int32_t fadingIn         = 1 << 1;   ///< Fade-in in progress — level checks suspended.
    constexpr int32_t fadingOut        = 1 << 2;   ///< Fade-out in progress — level checks suspended.
    constexpr int32_t inputInactive    = 1 << 3;   ///< No input channels available/enabled.
    constexpr int32_t settling         = 1 << 4;   ///< Warm-up window after start; detection suppressed.
    constexpr int32_t dropoutActive    = 1 << 5;   ///< A dropout condition is ongoing at snapshot time.
}

/** Sentinel dB value meaning "silence / below measurable floor".
    CSV writers emit this literally; UIs clamp their meters at/above it. */
constexpr float silenceFloorDb = -120.0f;

/** Hard ceiling on input/output channels carried through the audio→message
    FIFOs.

    Measurement must stay a fixed-size, trivially-copyable POD: that is what lets
    the audio thread hand data to the message thread without allocating or
    locking (see ARCHITECTURE.md and the real-time rule in CLAUDE.md). Channel
    counts are therefore runtime values bounded by this constant rather than
    unbounded. Devices exposing more than this are clamped silently. */
constexpr int maxChannels = 64;

//==============================================================================
/**
    One measurement snapshot, pushed from the AUDIO thread into the
    MeasurementQueue every measurement interval (default 1.0 s, see
    AnalysisProcessor). Field order matches the measurement CSV column order
    (after the wall-clock timestamp column, which the SessionLogger prepends
    on the message thread).

    Channel data lives in fixed-size arrays; only the first
    numOutputChannels / numInputChannels entries are meaningful.

    All dB values are dBFS. Silence is reported as silenceFloorDb.
*/
struct Measurement
{
    double  elapsedSeconds { 0.0 };   ///< Session-relative time (audio-clock: samples since session start / sampleRate).
    double  sampleRate     { 0.0 };   ///< Current device sample rate (Hz).
    int32_t bufferSize     { 0 };     ///< Current device buffer size in samples.

    /** Valid entries in the per-channel arrays below. Both are <= maxChannels. */
    int32_t numOutputChannels { 0 };
    int32_t numInputChannels  { 0 };

    float   refPeakDb[maxChannels] {};  ///< Per-output reference peak, dBFS.
    float   refRmsDb[maxChannels]  {};  ///< Per-output reference RMS, dBFS.

    float   inPeakDb[maxChannels]  {};  ///< Per-input returned peak, dBFS.
    float   inRmsDb[maxChannels]   {};  ///< Per-input returned RMS, dBFS.

    /** Per-input round-trip gain: inRmsDb[i] minus the reference RMS of the
        output channel that input is referenced against. */
    float   gainDiffDb[maxChannels] {};

    /** Clipped input samples during the interval, split per input channel. */
    int32_t clipCountPerChannel[maxChannels] {};

    float   correlation    { 0.0f };  ///< Ref/input correlation coefficient [-1..1]. May be 0.0 in V1 (unimplemented).

    int32_t dropoutFlag    { 0 };     ///< 1 if a dropout condition was active at any point in the interval, else 0.
    int32_t clipCountBlock { 0 };     ///< Number of input samples at/above clip threshold during the interval (all channels).
    int32_t statusCode     { StatusBits::ok }; ///< Bitwise OR of StatusBits.
};

static_assert (std::is_trivially_copyable<Measurement>::value,
               "Measurement must be trivially copyable: it is memcpy'd through a lock-free FIFO");

//==============================================================================
/**
    One detection event, pushed from the AUDIO thread into the EventQueue when
    the EventDetector fires (or from the MESSAGE thread for ManualMarker /
    AudioDeviceLost / LoggingError). POD only — the human-readable description
    string is composed later, on the message thread, from these fields.

    Convention for start/end pairs (dropout, level change): `code` carries
    eventCodeStart when the condition begins and eventCodeEnd when it clears;
    for end events, measuredValue holds the condition's duration in seconds.
*/
struct Event
{
    EventType type          { EventType::Other };
    Severity  severity      { Severity::Info };
    ChannelId channel       { ChannelId::None };

    /** Index of the channel this event belongs to, or -1 when it applies to no
        single channel. With more than two channels this — not `channel` — is the
        authoritative identifier; the CSV renders it as the user's channel name.
        `channel` is retained so existing two-channel logs stay readable. */
    int32_t   channelIndex  { -1 };
    /** True when channelIndex refers to an input channel, false for an output. */
    bool      channelIsInput { true };

    double    elapsedSeconds{ 0.0 };  ///< Session-relative time of the event (audio clock).

    double    measuredValue { 0.0 };  ///< The observed value (e.g. input RMS dB; duration seconds for end events).
    double    expectedValue { 0.0 };  ///< What was expected (e.g. reference RMS dB, or 0 when n/a).
    double    difference    { 0.0 };  ///< measuredValue - expectedValue (or condition-specific delta).
    double    thresholdUsed { 0.0 };  ///< The threshold that was crossed, same units as measuredValue.

    int32_t   code          { 0 };    ///< eventCodeStart / eventCodeEnd / eventCodeInstant (see below).
};

static_assert (std::is_trivially_copyable<Event>::value,
               "Event must be trivially copyable: it is memcpy'd through a lock-free FIFO");

/** Values for Event::code. */
constexpr int32_t eventCodeInstant = 0; ///< One-shot event (clipping burst, marker, device lost...).
constexpr int32_t eventCodeStart   = 1; ///< An ongoing condition began.
constexpr int32_t eventCodeEnd     = 2; ///< An ongoing condition ended (measuredValue = duration seconds).

//==============================================================================
/**
    Immutable description of one monitoring session. Created on the MESSAGE
    thread when logging starts; passed to SessionLogger::startSession() and
    displayed in the UI. Never crosses a FIFO — juce types are fine here.
*/
struct SessionInfo
{
    juce::String sessionId;                ///< e.g. "20260725_231502" — also the session folder name.
    juce::Time   startTimeUtc;             ///< Wall-clock UTC start; anchors elapsedSeconds -> timestamps.
    double       sampleRate      { 0.0 };  ///< Device sample rate at session start (Hz).
    int          bufferSize      { 0 };    ///< Device buffer size at session start (samples).
    int          numOutputChannels { 2 };  ///< Active output channels (<= maxChannels).
    int          numInputChannels  { 2 };  ///< Active input channels (<= maxChannels).

    /** User-facing names, one per active channel, in channel order. These name
        the per-channel columns in measurements.csv and the `channel` column in
        events.csv, so a log is readable without knowing the patch. Missing
        entries fall back to "Out n" / "In n". */
    juce::StringArray outputChannelNames;
    juce::StringArray inputChannelNames;

    /** referenceOutputForInput[i] = which output channel input i is compared
        against. Recorded so a log can be interpreted after the fact. */
    juce::Array<int>  referenceOutputForInput;

    /** inputMonitored[i] = whether input i takes part in fault detection. An
        unpatched input sits below the dropout threshold forever, so leaving it
        monitored would report a permanent critical dropout and set dropout_flag
        on every row. Levels are still measured and logged either way. */
    juce::Array<bool> inputMonitored;
    juce::String inputDeviceName;          ///< As reported by juce::AudioDeviceManager.
    juce::String outputDeviceName;
    uint64_t     pinkNoiseSeed   { 0 };    ///< Seed given to PinkNoiseGenerator (deterministic replay).
    double       outputLevelDb   { -20.0 };///< Generator level at session start, dBFS.
    juce::File   logFolder;                ///< Per-session folder holding the two CSV files.
};

//==============================================================================
// Message-thread helpers (CSV / UI text). NOT for use on the audio thread.

/** Returns a stable, CSV-safe name for an EventType (e.g. "CompleteDropout"). */
const char* toString (EventType) noexcept;

/** Returns "Info" / "Warning" / "Critical". */
const char* toString (Severity) noexcept;

/** Returns "L" / "R" / "Both" / "-". */
const char* toString (ChannelId) noexcept;

} // namespace pnam

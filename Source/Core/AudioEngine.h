#pragma once
/*
  ==============================================================================
    AudioEngine.h
    Pink Noise Audio Monitor (pnam) — audio-thread glue class.

    NOTE: This header did not exist as part of the fixed contract handed to
    this engineer; it is authored here strictly to match the "Expected
    AudioEngine surface" documented verbatim in Source/UI/MainComponent.h and
    the responsibilities described in ARCHITECTURE.md §5. No signature here
    was invented beyond what those two documents already specify; anything
    additional (private members, helper methods) is an implementation detail
    behind that public surface.

    Owns: juce::AudioDeviceManager, PinkNoiseGenerator, ReferenceHistory (ref
    + a second ring for input, so the UI can pull waveform data for both),
    AnalysisProcessor, EventDetector. Registers itself as the
    juce::AudioIODeviceCallback and runs the ARCHITECTURE.md §4 per-callback
    sequence entirely on the audio thread: never allocates, locks, or does
    file/UI work inside audioDeviceIOCallbackWithContext.
  ==============================================================================
*/

#include <atomic>
#include <cstdint>
#include <memory>
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "../Model/DataModel.h"
#include "MeasurementQueue.h"
#include "PinkNoiseGenerator.h"
#include "ReferenceHistory.h"
#include "AnalysisProcessor.h"
#include "EventDetector.h"
#include "ArtifactDetector.h"

namespace pnam
{

//==============================================================================
class AudioEngine : private juce::AudioIODeviceCallback
{
public:
    /** @param measurementQueueToUse  audio->message measurement FIFO (owned by
            the caller, e.g. MainComponent; must outlive this object).
        @param eventQueueToUse        audio->message event FIFO (same lifetime
            requirement). */
    AudioEngine (MeasurementQueue& measurementQueueToUse, EventQueue& eventQueueToUse);
    ~AudioEngine() override;

    AudioEngine (const AudioEngine&) = delete;
    AudioEngine& operator= (const AudioEngine&) = delete;

    //==========================================================================
    /** MESSAGE THREAD. Opens/configures the audio device WITHOUT registering the
        audio callback, so the device manager holds a real, user-editable device
        before Start Audio is ever pressed. Call once at construction.

        @param savedState  a previous createDeviceStateXml() result to restore
                           the user's saved selection, or nullptr for system
                           defaults.
        @returns an empty string on success, else the device error. */
    juce::String initialiseDevices (const juce::XmlElement* savedState);

    /** MESSAGE THREAD. Serialises the current device selection so the caller can
        persist it. @returns nullptr if there is nothing to save. */
    std::unique_ptr<juce::XmlElement> createDeviceStateXml() const;

    /** MESSAGE THREAD. Brackets a user-initiated device reconfiguration.

        Changing device/rate/buffer/channels stops and restarts the device, which
        would otherwise be reported as an AudioDeviceLost fault and would clear
        the running flag. While this is set the expected stop is ignored, the
        session keeps running, and MainComponent logs a single
        AudioSettingsChanged event instead. Genuine device failures (and
        audioDeviceError) are unaffected. */
    void setExpectedDeviceChange (bool inProgress) noexcept
    {
        expectedDeviceChange.store (inProgress, std::memory_order_relaxed);
    }

    bool isExpectedDeviceChange() const noexcept
    {
        return expectedDeviceChange.load (std::memory_order_relaxed);
    }

    /** MESSAGE THREAD. Registers this as the device callback and starts the
        generator's fade-in, reopening the last device if it was closed.

        Deliberately does NOT re-select default devices: doing so would discard
        whatever the user chose in Audio Device Setup. @returns false if no
        device could be opened.

        A no-op returning true while a session is already running: the session
        clock is only ever zeroed by a start that follows a stop(), never
        underneath a session (and a log) that is still open. */
    bool start();

    /** MESSAGE THREAD. Fades the generator out, waits (bounded) for the fade
        to complete, then silences the output (the device stays open so the
        input meters keep working). Always leaves the output silenced, including
        when the session was not running. */
    void stop();

    /** True from a successful start() until stop(). This is the SESSION state,
        not the device state: an unexpected device stop or restart does not end
        the session (the clock, the detector state and any open log carry on),
        so it does not clear this. See isDeviceActive() and
        isOutputSilencedForDeviceLoss() for what the hardware is doing. */
    bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }

    /** True while a device is started and calling the audio callback. False
        between a device stopping (for any reason) and the next one starting. */
    bool isDeviceActive() const noexcept { return deviceActive.load (std::memory_order_relaxed); }

    /** True after the device stopped WITHOUT being asked to (unplugged, clock
        loss, a CoreAudio renegotiation...). From that moment the output is held
        silent, because whatever device JUCE reopens next may be the system
        default — and pink noise must never start coming out of the Mac's own
        speakers. Input metering and the session clock continue.

        The hold is released automatically if the SAME device comes back, and
        by clearDeviceLossSilence() or a fresh start() otherwise. */
    bool isOutputSilencedForDeviceLoss() const noexcept
    {
        return deviceLossSilenced.load (std::memory_order_relaxed);
    }

    /** MESSAGE THREAD. Releases the device-loss hold so output resumes (with the
        usual fade-in and settling window) on whatever device is open now. Call
        once the device the user actually chose is back. No-op if not held. */
    void clearDeviceLossSilence() noexcept;

    /** MESSAGE THREAD. Forces the same silent hold an unexpected device stop
        sets, whatever the expected-change flag says — for when the UI finds the
        session running on a device the user did not choose (a loss that happened
        inside an expected-change window is swallowed, and JUCE's fallback device
        would otherwise play the noise). Released the same ways: by
        clearDeviceLossSilence(), by a fresh start(), or by the device the user
        chose (the device manager's explicit selection) starting again. Logs
        nothing. No-op if the hold is already in force. */
    void holdOutputForDeviceLoss() noexcept;

    /** Events that could not be queued for the message thread and are gone
        (queue full). Detector start/end events are held and retried rather than
        dropped, so in practice this counts lost device-lost/device-error
        records. Any thread. */
    int64_t getDroppedEventCount() const noexcept
    {
        return (int64_t) eventQueue.getTotalPushFailures();
    }

    /** Measurement snapshots dropped because their queue was full — i.e. rows
        that will be missing from measurements.csv. Any thread. */
    int64_t getDroppedMeasurementCount() const noexcept
    {
        return (int64_t) measurementQueue.getTotalPushFailures();
    }

    /** True while the device callback is attached but no noise is being emitted
        — the state the app sits in before Start Audio and after Stop Audio.
        Input is still captured and analysed so the meters stay live, which is
        what lets a user find a patch point without playing noise. */
    bool isOutputSilenced() const noexcept { return outputSilenced.load (std::memory_order_relaxed); }

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

    AnalysisProcessor&  getAnalysis()  noexcept { return analysisProcessor; }
    EventDetector&      getDetector()  noexcept { return eventDetector; }
    ArtifactDetector&   getArtifactDetector() noexcept { return artifactDetector; }
    PinkNoiseGenerator& getGenerator() noexcept { return generator; }

    double getSampleRate() const noexcept          { return currentSampleRate.load (std::memory_order_relaxed); }
    int    getBufferSize() const noexcept          { return currentBufferSize.load (std::memory_order_relaxed); }
    int    getNumInputChannels() const noexcept    { return currentNumInputChannels.load (std::memory_order_relaxed); }
    int    getNumOutputChannels() const noexcept   { return currentNumOutputChannels.load (std::memory_order_relaxed); }

    juce::String getInputDeviceName() const;
    juce::String getOutputDeviceName() const;

    //==========================================================================
    /** MESSAGE THREAD. Copies up to maxSamples of the most recent reference
        (generated) samples into dest. @returns the number of valid samples. */
    int readReferenceWaveform (int channel, float* dest, int maxSamples);

    /** MESSAGE THREAD. Copies up to maxSamples of the most recent input
        samples (channel 0 = L, 1 = R) into dest. @returns valid sample count. */
    int readInputWaveform (int channel, float* dest, int maxSamples);

private:
    //==========================================================================
    // juce::AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                            int numInputChannels,
                                            float* const* outputChannelData,
                                            int numOutputChannels,
                                            int numSamples,
                                            const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& errorMessage) override;

    //==========================================================================
    MeasurementQueue& measurementQueue;
    EventQueue&       eventQueue;

    juce::AudioDeviceManager deviceManager;

    PinkNoiseGenerator generator;
    ReferenceHistory   referenceHistory; // reference (generated) ring
    ReferenceHistory   inputHistory;     // input ring, for waveform display only
    AnalysisProcessor  analysisProcessor;
    EventDetector      eventDetector;
    ArtifactDetector   artifactDetector;

    /** Runs the artifact detector's cross-correlations (finding each return's
        delay) off the audio thread. It polls: the audio thread hands work over
        through atomics and must never signal or wake anything. */
    struct AcquisitionThread : public juce::Thread
    {
        explicit AcquisitionThread (ArtifactDetector& d) : juce::Thread ("Artifact alignment"), detector (d) {}

        void run() override
        {
            while (! threadShouldExit())
                if (! detector.serviceAcquisitions())
                    wait (20);
        }

        ArtifactDetector& detector;
    };

    AcquisitionThread acquisitionThread { artifactDetector };

    std::atomic<bool>   running { false };

    // Device state, tracked separately from the session state above.
    std::atomic<bool>   deviceActive { false };

    // Output hold after an unexpected device stop (see
    // isOutputSilencedForDeviceLoss). lostDeviceName is the device whose return
    // releases it — the one that was playing when it stopped, or the user's
    // chosen device for holdOutputForDeviceLoss() — guarded by nameLock;
    // lossWallMs is when (wall clock, so an outage spanning sleep is measured).
    std::atomic<bool>          deviceLossSilenced { false };
    juce::String               lostDeviceName;
    std::atomic<juce::int64>   lossWallMs { 0 };

    // Set by the message thread to make the callback restart its settling
    // window (samplesSinceStart is audio-thread state).
    std::atomic<bool>   settleRestartRequested { false };

    // True once initialiseDevices() has run, so start() knows the current
    // selection is the user's and must not be replaced with system defaults.
    bool devicesInitialised { false };

    // Set by MainComponent around a user-initiated device reconfiguration.
    std::atomic<bool> expectedDeviceChange { false };

    // True whenever the callback is running purely to meter the input. Starts
    // true: the callback is attached as soon as a device is open, so input
    // meters work from launch rather than only after Start Audio.
    std::atomic<bool> outputSilenced { true };

    std::atomic<double> currentSampleRate        { 0.0 };
    std::atomic<int>    currentBufferSize        { 0 };
    std::atomic<int>    currentNumInputChannels  { 0 };
    std::atomic<int>    currentNumOutputChannels { 0 };

    juce::CriticalSection nameLock;
    juce::String inputDeviceName, outputDeviceName;

    // Audio-thread-only state (no synchronisation needed: written/read only
    // from within the IO callback / audioDeviceAboutToStart, never message
    // thread).
    float   previousGainLinear   { 0.0f };
    int64_t samplesSinceStart    { 0 };

    // Copy ctor/assignment are already deleted explicitly above, so use the
    // leak-detector-only macro here (the NON_COPYABLE variant would redeclare
    // them and fail to compile).
    JUCE_LEAK_DETECTOR (AudioEngine)
};

} // namespace pnam

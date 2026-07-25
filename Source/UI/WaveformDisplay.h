#pragma once
/*
  ==============================================================================
    WaveformDisplay.h
    Pink Noise Audio Monitor (pnam) — scrolling reference vs. input waveform.

    Draws up to three traces (reference, input L, input R) over a shared time
    axis. All data arrives on the MESSAGE THREAD via pushSamples(): each UI
    tick, MainComponent reads the most recent audio from the engine (the
    engine exposes message-thread-safe snapshots of ReferenceHistory and its
    matching input history ring — see ARCHITECTURE.md §5 glue notes) and
    hands raw float samples here. The component immediately decimates them
    into per-pixel min/max pairs stored in small preallocated ring buffers,
    so memory is bounded regardless of session length.

    NO audio-thread access ever happens inside this class.

    Features (V1 scope):
      - toggle each trace on/off
      - linear or dB amplitude scale (dB maps |sample| through the range
        floorDb..0 and mirrors it around the centre line for shape)
      - freeze (stop consuming pushSamples; buffered picture stays put)
      - adjustable visible time window (default 2.0 s to match the 2 s
        ReferenceHistory sizing)
  ==============================================================================
*/

#include <juce_gui_basics/juce_gui_basics.h>

#include "DarkLookAndFeel.h"

namespace pnam
{

//==============================================================================
class WaveformDisplay : public juce::Component
{
public:
    //==========================================================================
    enum class Trace : int
    {
        reference = 0,   ///< generated pink-noise reference (post-gain)
        inputL    = 1,   ///< returned input, left / mono
        inputR    = 2    ///< returned input, right (hidden in mono sessions)
    };
    static constexpr int numTraces = 3;

    enum class AmplitudeScale
    {
        linear,          ///< -1..+1 full height
        decibels         ///< |x| in dB over floorDb..0, mirrored about centre
    };

    static constexpr double defaultTimeWindowSeconds = 2.0;
    static constexpr float  defaultDbFloor           = -60.0f;

    //==========================================================================
    WaveformDisplay();
    ~WaveformDisplay() override;

    //==========================================================================
    // Configuration
    /** Must be called before the first pushSamples() and again whenever the
        device sample rate changes; (re)sizes the internal min/max rings for
        the current time window. Message thread. */
    void setSampleRate (double newSampleRate);

    /** Visible history length in seconds (clamped to 0.1 .. 10.0).
        Resizes/re-decimates the internal rings. */
    void setTimeWindowSeconds (double seconds);
    double getTimeWindowSeconds() const noexcept        { return timeWindowSeconds; }

    void setAmplitudeScale (AmplitudeScale scale);
    AmplitudeScale getAmplitudeScale() const noexcept   { return amplitudeScale; }

    /** dB floor used when amplitudeScale == decibels (default -60). */
    void setDbFloor (float newFloorDb);

    //==========================================================================
    // Trace visibility
    void setTraceVisible (Trace trace, bool shouldBeVisible);
    bool isTraceVisible (Trace trace) const noexcept;

    /** Convenience for session start: mono hides inputR, stereo shows it. */
    void setNumInputChannels (int numChannels);

    //==========================================================================
    // Data input (message thread, called from MainComponent::timerCallback)
    /** Appends numSamples raw samples for one trace. Samples are decimated
        into min/max pixel columns on the spot; the raw pointer is not
        retained. Ignored while frozen. */
    void pushSamples (Trace trace, const float* samples, int numSamples);

    /** Clears all trace history (session reset / device change). */
    void clear();

    //==========================================================================
    // Freeze
    /** While frozen, pushSamples() is discarded and the display holds still
        so the user can inspect a glitch. */
    void setFrozen (bool shouldFreeze);
    bool isFrozen() const noexcept                      { return frozen; }

    //==========================================================================
    /** Repaints if new data arrived since the last paint. MainComponent calls
        this once per UI tick after its pushSamples() calls, keeping repaint
        cadence in one place. */
    void refresh();

    //==========================================================================
    /** Notified when the user toggles freeze by clicking the display
        (single-click toggles freeze; the freeze state is also settable from
        a settings/context menu). Optional. */
    std::function<void (bool nowFrozen)> onFreezeChanged;

    //==========================================================================
    // juce::Component
    void paint (juce::Graphics&) override;    // grid, centre line, traces
    void resized() override;                  // re-derive samples-per-pixel
    void mouseDown (const juce::MouseEvent&) override; // click = toggle freeze

private:
    //==========================================================================
    /** One decimated column: the min & max sample seen in that pixel's span. */
    struct MinMax { float mn, mx; };

    /** Fixed-capacity ring of MinMax columns for one trace, plus a partial-
        column accumulator (samples arrive in arbitrary block sizes). */
    struct TraceRing
    {
        std::vector<MinMax> columns;     // capacity = component width (>= 1)
        int   writeIndex        = 0;
        int   columnsFilled     = 0;
        float accumMin          = 0.0f, accumMax = 0.0f;
        int   accumCount        = 0;
        bool  visible           = true;
    };

    void rebuildRings();                 // on resize / rate / window change
    juce::Colour colourForTrace (Trace) const noexcept;

    double sampleRate        = 48000.0;
    double timeWindowSeconds = defaultTimeWindowSeconds;
    int    samplesPerColumn  = 1;        // derived: window * rate / width
    AmplitudeScale amplitudeScale = AmplitudeScale::linear;
    float  dbFloor           = defaultDbFloor;
    bool   frozen            = false;
    bool   dirty             = false;    // set by pushSamples, cleared by paint

    TraceRing rings[numTraces];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformDisplay)
};

} // namespace pnam

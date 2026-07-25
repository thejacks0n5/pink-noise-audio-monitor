#pragma once
/*
  ==============================================================================
    LevelMeterComponent.h
    Pink Noise Audio Monitor (pnam) — peak + RMS bar meter, dBFS scale.

    One LevelMeterComponent instance shows ONE signal (either the reference
    output or the returned input) with 1 or 2 channel bars, a dB ruler,
    peak-hold ticks, numeric dBFS readouts, and a latching clip indicator.

    Data flow (MESSAGE THREAD ONLY):
      - MainComponent gives the meter a pointer to the engine's
        AnalysisProcessor via setSource(). The meter itself never subscribes
        to a timer; MainComponent's single ~30 Hz juce::Timer calls
        refresh() on every meter each tick. refresh() polls the atomic
        getters (getRefPeakDb / getInPeakDb(ch) / ...) declared in
        AnalysisProcessor.h and repaints only when a displayed value moved.
      - Alternatively (e.g. for a preview with no engine), updateValues()
        pushes explicit numbers.
      - The clip indicator does NOT poll: MainComponent latches it by calling
        notifyClipped() when a drained Measurement has clipCountBlock > 0 or
        a Clipping event arrives. It stays lit until the user clicks the
        meter or clearClipIndicator() is called.

    Interaction: clicking anywhere on the meter clears the clip latch and
    invokes onResetRequest (MainComponent uses this to call
    AnalysisProcessor::resetPeakHold(), which resets peak-hold for BOTH
    meters since the hold state lives in the shared processor).

    Ballistics: refresh() applies instant attack and a fixed decibel-per-
    second release to the displayed bar values so the bars fall smoothly at
    the UI tick rate; the numeric readout shows the raw polled value.
  ==============================================================================
*/

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../Core/AnalysisProcessor.h"
#include "DarkLookAndFeel.h"

namespace pnam
{

//==============================================================================
class LevelMeterComponent : public juce::Component
{
public:
    //==========================================================================
    /** Which signal this meter instance displays. Selects which
        AnalysisProcessor getters refresh() polls. */
    enum class Role
    {
        reference,  ///< refPeak/refRms (single bar; reference is generated mono/linked)
        input       ///< inPeak/inRms per channel (1 or 2 bars)
    };

    /** Default visible range of the dB ruler. */
    static constexpr float defaultMinDb = -60.0f;
    static constexpr float defaultMaxDb =   0.0f;
    /** Display release rate applied by refresh() (dB per second). */
    static constexpr float defaultReleaseDbPerSecond = 20.0f;

    //==========================================================================
    /** @param role   reference or input signal.
        @param title  short caption drawn above the bars ("REF OUT", "IN"). */
    LevelMeterComponent (Role role, const juce::String& title);
    ~LevelMeterComponent() override;

    //==========================================================================
    // Wiring
    /** Points the meter at the engine's AnalysisProcessor (may be nullptr to
        detach, e.g. while the audio engine is being rebuilt). Not owned. */
    void setSource (AnalysisProcessor* source) noexcept;

    /** Number of channel bars to draw, 1..maxChannels. Role::reference now
        draws one bar per OUTPUT channel (each output carries its own
        independently seeded stream), Role::input one bar per input channel. */
    void setNumChannels (int numChannels);

    /** Per-bar captions drawn under each bar. Extra entries are ignored;
        missing ones fall back to the channel number. */
    void setChannelNames (const juce::StringArray& names);
    int  getNumChannels() const noexcept                     { return numChannels; }

    //==========================================================================
    // Per-tick update (called from MainComponent::timerCallback)
    /** Polls the source's atomic getters, applies release ballistics, and
        repaints if anything visible changed. No-op when no source is set. */
    void refresh();

    /** Manual value injection (dBFS); bypasses the source. Pass hold values
        of silenceFloorDb to hide peak-hold ticks. Mono: R args ignored. */
    void updateValues (float peakDbL, float rmsDbL,
                       float peakDbR, float rmsDbR,
                       float peakHoldDbL, float peakHoldDbR);

    //==========================================================================
    // Clip indicator (latching)
    /** Lights the clip lamp for the given channel (0 = L, 1 = R; pass
        ChannelId::Both semantics by calling for each channel). Latches until
        cleared. */
    void notifyClipped (int channel);
    void clearClipIndicator();
    bool isClipIndicatorLit() const noexcept;

    //==========================================================================
    // Appearance / scale
    /** Change the visible dB range (e.g. -80..0 for very quiet rigs). */
    void setRangeDb (float newMinDb, float newMaxDb);
    float getMinDb() const noexcept                          { return minDb; }
    float getMaxDb() const noexcept                          { return maxDb; }

    /** Show/hide the numeric dBFS readout row under the bars (default on). */
    void setReadoutVisible (bool shouldShow);

    /** 0..1 vertical proportion for a dB value within the current range,
        clamped; used by paint() and exposed for unit tests. */
    float proportionForDb (float db) const noexcept;

    //==========================================================================
    /** Invoked on mouse click (after the clip latch is cleared). MainComponent
        assigns this to call AnalysisProcessor::resetPeakHold(). */
    std::function<void()> onResetRequest;

    //==========================================================================
    // juce::Component
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    //==========================================================================
    // Layout (computed in resized()):
    //   [ title ]                       — top strip, ~16 px
    //   [ clip lamp(s) ]                — ~10 px, one lamp per channel
    //   [ bars | dB ruler ]             — bars fill remaining height; ruler
    //                                     ticks at 0, -3, -6, -12, -20, -30,
    //                                     -40, -50, -60 drawn beside the bars
    //   [ numeric dBFS readout ]        — bottom strip, mono font, ~14 px
    //
    // Each channel bar: trough (meterTrough), gradient/segments coloured by
    // Palette::meterColourForDb per zone, translucent cap at the RMS height,
    // 2 px peak-hold tick (meterPeakTick) at the held peak.

    Role role;
    juce::String title;
    AnalysisProcessor* analysis = nullptr;   // not owned
    int numChannels = 1;

    float minDb = defaultMinDb;
    float maxDb = defaultMaxDb;
    bool  readoutVisible = true;

    // Displayed (ballistic) values, dBFS, per channel [0]=L/mono [1]=R
    float shownPeakDb[maxChannels];
    float shownRmsDb[maxChannels];
    float shownHoldDb[maxChannels];
    bool  clipLit[maxChannels] {};
    juce::StringArray channelNames;
    juce::int64 lastRefreshMs = 0;           // for release ballistics

    juce::Rectangle<int> barArea, readoutArea, clipArea, titleArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeterComponent)
};

} // namespace pnam

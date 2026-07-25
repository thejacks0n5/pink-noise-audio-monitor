#pragma once
/*
  ==============================================================================
    TopStatusBar.h
    Pink Noise Audio Monitor (pnam) — one-line status strip across the top.

    Read-only display; MainComponent pushes values through the setters below
    from its ~30 Hz timer (cheap: setters no-op + skip repaint when the value
    is unchanged). MESSAGE THREAD ONLY.

    Visual layout (left → right), each cell = tiny caption over value text,
    separated by hairline dividers; the two lamp cells draw a coloured dot:

      [• AUDIO lamp+state] [• LOG lamp+state] [SESSION duration] [MODE]
      [OUT device] [IN device] [CH config] [RATE] [BUFFER]        [app status…]

    Colours: lamps use Palette::statusRunning / statusPaused / statusIdle /
    severityCritical (error); values textPrimary; captions textSecondary.
  ==============================================================================
*/

#include <juce_gui_basics/juce_gui_basics.h>

#include "DarkLookAndFeel.h"

namespace pnam
{

//==============================================================================
class TopStatusBar : public juce::Component
{
public:
    //==========================================================================
    enum class EngineState  { stopped, starting, running, deviceError };
    enum class LoggingState { inactive, active, paused, writeError };

    /** Preferred fixed height for MainComponent::resized(). */
    static constexpr int preferredHeight = 44;

    //==========================================================================
    TopStatusBar();
    ~TopStatusBar() override;

    //==========================================================================
    // Setters (all message thread; each repaints only its own cell on change)

    /** Free-text app status shown at the far right, e.g. "Idle",
        "Monitoring", "3 critical events", "Settling…". */
    void setAppStatus (const juce::String& text);

    /** Audio engine lamp + label ("Stopped" / "Starting…" / "Running" /
        "Device error"). */
    void setAudioEngineState (EngineState state);
    EngineState getAudioEngineState() const noexcept    { return engineState; }

    /** Logging lamp + label ("Off" / "Logging" / "Paused" / "Write error").
        When active, MainComponent should also pass the session id via
        setAppStatus or leave it to the session-duration cell. */
    void setLoggingState (LoggingState state);
    LoggingState getLoggingState() const noexcept       { return loggingState; }

    void setOutputDeviceName (const juce::String& name);   // "—" when none
    void setInputDeviceName  (const juce::String& name);

    /** Active channel layout, e.g. out=2 in=2 renders "2 out / 2 in
        (stereo)". */
    void setChannelConfig (int numOutputChannels, int numInputChannels);

    /** Hz; renders "48.0 kHz". Pass 0 to show "—". */
    void setSampleRate (double sampleRateHz);

    /** Samples; renders "512 smp (10.7 ms)" using the last set sample rate.
        Pass 0 to show "—". */
    void setBufferSize (int bufferSizeSamples);

    /** Session elapsed time; renders "HH:MM:SS". Negative hides the cell
        value ("—", no active session). Called every timer tick — cheap. */
    void setSessionDuration (double seconds);

    /** Current test mode text — V1 is always "Pink noise loopback"; kept as
        a setter so V2 modes drop in without a UI change. */
    void setTestMode (const juce::String& modeText);

    //==========================================================================
    // juce::Component
    void paint (juce::Graphics&) override;    // draws all cells + dividers
    void resized() override;                  // recomputes cell rectangles

private:
    //==========================================================================
    struct Cell                    // one caption/value pair
    {
        juce::String caption, value;
        juce::Rectangle<int> bounds;
        juce::Colour lamp { juce::Colour() };  // transparent = no lamp
        bool hasLamp = false;
    };

    void updateCell (Cell&, const juce::String& newValue,
                     juce::Colour lampColour, bool repaintNow);
    static juce::String formatDuration (double seconds);

    EngineState  engineState  = EngineState::stopped;
    LoggingState loggingState = LoggingState::inactive;
    double lastSampleRateHz   = 0.0;

    // Cell order matches the layout comment in the header block.
    Cell audioCell, loggingCell, durationCell, modeCell,
         outDeviceCell, inDeviceCell, channelsCell, rateCell, bufferCell,
         appStatusCell;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TopStatusBar)
};

} // namespace pnam

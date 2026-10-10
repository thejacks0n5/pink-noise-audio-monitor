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

      [• AUDIO lamp+state] [• LOG lamp+state] [SESSION duration]
      [OUT device] [IN device] [CH config] [RATE] [BUFFER]        [app status…]

    The MODE cell is no longer laid out: V1 has one mode, and its fixed text was
    taking the room the app-status text (where every error message lands) needs.

    Colours: lamps use Palette::statusRunning / statusPaused / statusIdle /
    severityCritical (error); values textPrimary; captions textSecondary.
  ==============================================================================
*/

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "DarkLookAndFeel.h"

namespace pnam
{

//==============================================================================
class TopStatusBar : public juce::Component
{
public:
    //==========================================================================
    /** deviceLost and outputHeld are the two states a running session can be in
        when the hardware is not doing its job: no device is delivering audio, or
        a device is but the engine is holding the output silent after a loss.
        Both (and deviceError) are drawn in red text, not just a red lamp. */
    enum class EngineState  { stopped, starting, running, deviceError, deviceLost, outputHeld };
    enum class LoggingState { inactive, active, paused, writeError };

    /** Identifies a cell for the onCellClicked callback. The device/rate/buffer
        cells are click-to-edit; the rest are display-only. */
    enum class CellId { none, audio, logging, duration, mode,
                        outDevice, inDevice, outChannels, inChannels, rate, buffer, appStatus };

    /** Assigned by MainComponent. Fired when the user clicks an interactive
        cell; the rectangle is that cell's bounds in screen coordinates, so the
        handler can drop a PopupMenu directly beneath it. MESSAGE THREAD. */
    std::function<void (CellId, juce::Rectangle<int> cellScreenBounds)> onCellClicked;

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

    /** A fault the user must not miss ("'UMC404HD' LOST: output held silent").
        While non-empty it replaces the app status text and is drawn in red;
        pass an empty string to go back to the normal status. Called every
        timer tick with the current worst problem, or nothing — cheap. */
    void setAlert (const juce::String& text);

    /** Audio engine lamp + label ("Stopped" / "Starting…" / "Running" /
        "Device error" / "NO DEVICE" / "Output held"). */
    void setAudioEngineState (EngineState state);
    EngineState getAudioEngineState() const noexcept    { return engineState; }

    /** Logging lamp + label ("Off" / "Logging" / "Paused" / "Write error").
        When active, MainComponent should also pass the session id via
        setAppStatus or leave it to the session-duration cell. */
    void setLoggingState (LoggingState state);
    LoggingState getLoggingState() const noexcept       { return loggingState; }

    /** While the logging state is `paused`, shows how long it has been paused
        ("Paused 12:34") so a forgotten pause is visible from across the room.
        Ignored in every other state. Called every timer tick — cheap. */
    void setLoggingPausedSeconds (double seconds);

    void setOutputDeviceName (const juce::String& name);   // "—" when none
    void setInputDeviceName  (const juce::String& name);

    /** Active channel counts. Rendered as two independent cells (OUT CH / IN CH)
        so the user can change each side on its own. Pass 0 to show "—". */
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
        a setter so V2 modes drop in without a UI change. The cell is not
        currently shown (see the layout note above). */
    void setTestMode (const juce::String& modeText);

    //==========================================================================
    // juce::Component
    void paint (juce::Graphics&) override;    // draws all cells + dividers
    void resized() override;                  // recomputes cell rectangles

    // Click-to-edit affordance for the interactive cells
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    //==========================================================================
    struct Cell                    // one caption/value pair
    {
        juce::String caption, value;
        juce::Rectangle<int> bounds;
        juce::Colour lamp { juce::Colour() };  // transparent = no lamp
        bool hasLamp = false;
        CellId id = CellId::none;
        bool interactive = false;  // draws a chevron + hover highlight
        bool alert = false;        // value drawn in red (a fault, not a status)
    };

    void updateCell (Cell&, const juce::String& newValue,
                     juce::Colour lampColour, bool repaintNow);
    static juce::String formatDuration (double seconds);

    /** @returns the cell under the point, or nullptr. */
    Cell* cellAt (juce::Point<int>);

    CellId hoveredCell = CellId::none;

    EngineState  engineState  = EngineState::stopped;
    LoggingState loggingState = LoggingState::inactive;

    // The app-status cell shows alertText (red) when there is one, else appStatusText.
    juce::String appStatusText { "Idle" }, alertText;
    void refreshAppStatusCell();
    double lastSampleRateHz   = 0.0;

    // Cell order matches the layout comment in the header block.
    Cell audioCell, loggingCell, durationCell, modeCell,
         outDeviceCell, inDeviceCell, outChannelsCell, inChannelsCell,
         rateCell, bufferCell, appStatusCell;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TopStatusBar)
};

} // namespace pnam

#pragma once
/*
  ==============================================================================
    DelayMonitorComponent.h
    Pink Noise Audio Monitor (pnam) — live delay readout, one cell per input.

    Shows, for every input, how long the pink noise takes to come back: the
    round-trip latency the ArtifactDetector measures, minus a per-input
    calibration offset. Calibrate with the output cabled straight to the input
    and the offset becomes the interface's own latency; from then on the readout
    is the delay of whatever is put in the path.

    This component only displays and asks. MainComponent feeds it readings each
    timer tick and owns the offsets (they are saved per device configuration,
    because an interface's latency changes with sample rate and buffer size).

    MESSAGE THREAD ONLY.
  ==============================================================================
*/

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

namespace pnam
{

//==============================================================================
class DelayMonitorComponent : public juce::Component
{
public:
    static constexpr int preferredHeight = 58;

    /** Why a cell has, or has not, a number in it. */
    enum class State
    {
        stopped,        ///< audio is not running
        detectionOff,   ///< artifact detection (which does the aligning) is switched off
        notMonitored,   ///< this input's Monitor box is unticked
        searching,      ///< running, but the return has not been matched to its reference
        aligned         ///< measuredMs is a live measurement
    };

    DelayMonitorComponent();

    /** One cell per input, in channel order, titled with the user's names. */
    void setChannels (const juce::StringArray& inputNames);

    /** The calibration offsets (ms) subtracted from each input's round trip. */
    void setOffsetsMs (const juce::Array<double>& offsetsMs);

    /** For the samples figure beside each delay. */
    void setSampleRate (double sampleRate);

    /** Called every timer tick. Repaints only when the shown text changes. */
    void setReading (int input, State state, double measuredRoundTripMs);

    /** The round trip currently shown for an input (lightly smoothed), and
        whether it is a live measurement. Calibration takes its value from here
        so it stores what the user was looking at. */
    bool   isAligned (int input) const noexcept;
    double getRoundTripMs (int input) const noexcept;

    /** "Calibrate": take every aligned input's current round trip as its offset. */
    std::function<void()> onCalibrate;
    /** "Offset...": the user wants to type an offset for this input. */
    std::function<void (int input)> onEditOffset;
    /** "Clear": back to no calibration. */
    std::function<void()> onClear;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Cell
    {
        juce::String name;
        State  state { State::stopped };
        double roundTripMs { 0.0 };
        double offsetMs { 0.0 };
        juce::String valueText, samplesText, detailText;   // what is painted
    };

    void refreshCellText (Cell&);
    void updateButtons();
    void showOffsetMenu();
    juce::Rectangle<int> getCellArea() const;

    juce::Array<Cell> cells;
    double sampleRate { 0.0 };

    juce::TextButton calibrateButton { "Calibrate" };
    juce::TextButton offsetButton    { "Offset..." };
    juce::TextButton clearButton     { "Clear" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayMonitorComponent)
};

} // namespace pnam

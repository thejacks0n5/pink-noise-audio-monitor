/*
  ==============================================================================
    TransportControls.cpp
    Pink Noise Audio Monitor (pnam)
  ==============================================================================
*/

#include "TransportControls.h"
#include "../Core/PinkNoiseGenerator.h"

namespace pnam
{

//==============================================================================
TransportControls::TransportControls()
{
    addAndMakeVisible (startAudioButton);
    startAudioButton.onClick = [this] { if (onStartAudio != nullptr) onStartAudio(); };

    addAndMakeVisible (stopAudioButton);
    stopAudioButton.onClick = [this] { if (onStopAudio != nullptr) onStopAudio(); };

    addAndMakeVisible (startLoggingButton);
    startLoggingButton.onClick = [this] { if (onStartLogging != nullptr) onStartLogging(); };

    addAndMakeVisible (pauseLoggingButton);
    pauseLoggingButton.setClickingTogglesState (false);
    pauseLoggingButton.onClick = [this]
    {
        if (onPauseLogging != nullptr)
            onPauseLogging (! loggingPaused);
    };

    addAndMakeVisible (stopLoggingButton);
    stopLoggingButton.onClick = [this] { if (onStopLogging != nullptr) onStopLogging(); };

    addAndMakeVisible (muteButton);
    muteButton.setClickingTogglesState (false);
    muteButton.onClick = [this]
    {
        if (onMuteChanged != nullptr)
            onMuteChanged (! muted);
    };

    addAndMakeVisible (outputLevelSlider);
    outputLevelSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    outputLevelSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
    outputLevelSlider.setTextValueSuffix (" dBFS");
    outputLevelSlider.setRange (levelSliderMinDb, levelSliderMaxDb, levelSliderStepDb);
    outputLevelSlider.setValue (PinkNoiseGenerator::defaultLevelDb, juce::dontSendNotification);
    outputLevelSlider.onValueChange = [this]
    {
        if (onOutputLevelChanged != nullptr)
            onOutputLevelChanged (outputLevelSlider.getValue());
    };

    addAndMakeVisible (outputLevelCaption);
    outputLevelCaption.setJustificationType (juce::Justification::centredLeft);
    outputLevelCaption.setFont (juce::Font (juce::FontOptions (12.0f)));

    addAndMakeVisible (markerButton);
    markerButton.onClick = [this] { if (onAddMarker != nullptr) onAddMarker(); };

    addAndMakeVisible (resetButton);
    resetButton.onClick = [this] { if (onResetSession != nullptr) onResetSession(); };

    addAndMakeVisible (exportButton);
    exportButton.onClick = [this] { if (onExportSession != nullptr) onExportSession(); };

    addAndMakeVisible (deviceSetupButton);
    deviceSetupButton.onClick = [this] { if (onOpenDeviceSetup != nullptr) onOpenDeviceSetup(); };

    // Settings deliberately has no button here — it lives in the macOS
    // application menu as Preferences (Cmd-,); see Main.cpp.

    updateEnablement();
}

TransportControls::~TransportControls() = default;

//==============================================================================
void TransportControls::setAudioRunning (bool running)
{
    if (audioRunning != running)
    {
        audioRunning = running;
        updateEnablement();
    }
}

void TransportControls::setLoggingActive (bool active)
{
    if (loggingActive != active)
    {
        loggingActive = active;
        updateEnablement();
    }
}

void TransportControls::setLoggingPaused (bool paused)
{
    if (loggingPaused != paused)
    {
        loggingPaused = paused;
        pauseLoggingButton.setToggleState (loggingPaused, juce::dontSendNotification);
        pauseLoggingButton.setButtonText (loggingPaused ? "Resume" : "Pause");
        updateEnablement();
    }
}

void TransportControls::setMuted (bool newMuted)
{
    if (muted != newMuted)
    {
        muted = newMuted;
        muteButton.setToggleState (muted, juce::dontSendNotification);
        updateEnablement();
    }
}

void TransportControls::setSessionAvailable (bool available)
{
    if (sessionAvailable != available)
    {
        sessionAvailable = available;
        updateEnablement();
    }
}

void TransportControls::setOutputLevelDb (double db)
{
    outputLevelSlider.setValue (db, juce::dontSendNotification);
}

double TransportControls::getOutputLevelDb() const noexcept
{
    return outputLevelSlider.getValue();
}

//==============================================================================
void TransportControls::updateEnablement()
{
    startAudioButton.setEnabled (! audioRunning);
    stopAudioButton.setEnabled (audioRunning);

    startLoggingButton.setEnabled (audioRunning && ! loggingActive);
    pauseLoggingButton.setEnabled (loggingActive);
    stopLoggingButton.setEnabled (loggingActive);

    markerButton.setEnabled (loggingActive);
    resetButton.setEnabled (! loggingActive);
    exportButton.setEnabled (sessionAvailable);

    outputLevelSlider.setEnabled (audioRunning);
    muteButton.setEnabled (audioRunning);
}

//==============================================================================
void TransportControls::resized()
{
    auto bounds = getLocalBounds().reduced (8, 6);

    // Claim the right-hand buttons BEFORE the left-to-right run. Laying them out
    // last meant that once the fixed widths ahead of them exceeded the window,
    // Audio Setup got whatever scraps were left and rendered as "A...".
    deviceSetupButton.setBounds (bounds.removeFromRight (110));
    bounds.removeFromRight (12);

    exportButton.setBounds (bounds.removeFromRight (70));
    bounds.removeFromRight (4);
    resetButton.setBounds (bounds.removeFromRight (70));
    bounds.removeFromRight (4);
    markerButton.setBounds (bounds.removeFromRight (70));
    bounds.removeFromRight (12);

    // Scale the left-hand run when the window is narrow. Fixed widths summed to
    // more than the 900 px minimum window, which starved Mute and left the
    // output-level slider with no width at all.
    const int wantLeft = 90 + 4 + 90 + 12          // audio group
                       + 100 + 4 + 70 + 4 + 100 + 12  // logging group
                       + 70 + 4 + 46 + 140;           // mute + caption + slider
    const double leftScale = bounds.getWidth() < wantLeft
                               ? (double) bounds.getWidth() / (double) wantLeft
                               : 1.0;

    const auto lw = [leftScale] (int w, int minimum)
    {
        return juce::jmax (minimum, juce::roundToInt (w * leftScale));
    };

    const auto take = [&bounds] (int w) { return bounds.removeFromLeft (juce::jmin (w, bounds.getWidth())); };

    startAudioButton.setBounds (take (lw (90, 62)));
    take (4);
    stopAudioButton.setBounds (take (lw (90, 62)));
    take (12);

    startLoggingButton.setBounds (take (lw (100, 66)));
    take (4);
    pauseLoggingButton.setBounds (take (lw (70, 50)));
    take (4);
    stopLoggingButton.setBounds (take (lw (100, 66)));
    take (12);

    muteButton.setBounds (take (lw (70, 50)));
    take (4);
    outputLevelCaption.setBounds (take (lw (46, 34)));

    // The slider (plus its dBFS text box) absorbs what is left, capped so the
    // readout never drifts far from the controls it belongs to.
    outputLevelSlider.setBounds (take (juce::jmin (240, juce::jmax (0, bounds.getWidth()))));
}

void TransportControls::paint (juce::Graphics& g)
{
    g.fillAll (Palette::get (Palette::panelBackground));

    g.setColour (Palette::get (Palette::outline));
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}

} // namespace pnam

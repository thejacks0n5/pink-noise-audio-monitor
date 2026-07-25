#pragma once
/*
  ==============================================================================
    TransportControls.h
    Pink Noise Audio Monitor (pnam) — main control strip.

    All user actions are exposed as std::function callbacks; the component
    holds NO engine/logger pointers and performs NO logic beyond local button
    enable/disable rules. MainComponent assigns every callback in its
    constructor and reflects real system state back through the set…()
    methods (the buttons never assume their click succeeded).

    Enable/disable rules (applied internally by updateEnablement()):
      - Stop Audio           enabled only while audio running
      - Start Audio          enabled only while audio stopped
      - Start Logging        enabled only while audio running & logging off
      - Stop / Pause Logging enabled only while logging active
      - Add Marker           enabled only while logging active
      - Reset Session        disabled while logging active (stop first)
      - Export Session       enabled when a completed or active session exists
        (MainComponent tells us via setSessionAvailable)
      - Output level slider + Mute always enabled while audio running

    Layout (single 56 px strip, left → right):
      [Start Audio][Stop Audio] | [Start Log][Pause Log][Stop Log] |
      [Mute][ Output Level ───────── -20.0 dBFS ] |
      [Marker][Reset][Export] | (spacer) | [Audio Setup][Settings]

    MESSAGE THREAD ONLY.
  ==============================================================================
*/

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "DarkLookAndFeel.h"

namespace pnam
{

//==============================================================================
class TransportControls : public juce::Component
{
public:
    /** Preferred fixed height for MainComponent::resized(). */
    static constexpr int preferredHeight = 56;

    /** Output level slider range (dBFS). Default value comes from
        PinkNoiseGenerator::defaultLevelDb (-20 dBFS). */
    static constexpr double levelSliderMinDb  = -60.0;
    static constexpr double levelSliderMaxDb  =   0.0;
    static constexpr double levelSliderStepDb =   0.5;

    //==========================================================================
    TransportControls();
    ~TransportControls() override;

    //==========================================================================
    // Action callbacks — MainComponent assigns all of these. Buttons whose
    // callback is unset simply do nothing when clicked.
    std::function<void()>            onStartAudio;
    std::function<void()>            onStopAudio;
    std::function<void()>            onStartLogging;
    std::function<void()>            onStopLogging;
    std::function<void (bool pause)> onPauseLogging;      ///< toggle; true = pause
    std::function<void (bool mute)>  onMuteChanged;       ///< toggle; true = muted
    std::function<void (double db)>  onOutputLevelChanged;///< slider drag/typed, dBFS
    std::function<void()>            onResetSession;
    std::function<void()>            onAddMarker;
    std::function<void()>            onExportSession;
    std::function<void()>            onOpenDeviceSetup;
    // NOTE: there is deliberately no onOpenSettings here. Preferences is a
    // standard macOS application-menu item (Cmd-,) driven from Main.cpp, not a
    // button in the transport bar.

    //==========================================================================
    // State reflection (message thread; MainComponent calls these when the
    // real state changes — including changes NOT initiated here, e.g. device
    // loss stopping audio). Each updates toggle visuals + enablement and
    // never fires the callbacks above.
    void setAudioRunning (bool running);
    void setLoggingActive (bool active);
    void setLoggingPaused (bool paused);
    void setMuted (bool muted);
    void setSessionAvailable (bool available);  ///< enables Export

    /** Moves the slider without firing onOutputLevelChanged
        (juce::dontSendNotification). */
    void setOutputLevelDb (double db);
    double getOutputLevelDb() const noexcept;

    bool isAudioRunning() const noexcept     { return audioRunning; }
    bool isLoggingActive() const noexcept    { return loggingActive; }
    bool isLoggingPaused() const noexcept    { return loggingPaused; }
    bool isMuted() const noexcept            { return muted; }

    //==========================================================================
    // juce::Component
    void resized() override;
    void paint (juce::Graphics&) override;   // section divider hairlines

private:
    //==========================================================================
    void updateEnablement();                 // applies the rules table above

    // -- Widgets (public API is the callbacks; widgets stay private so the
    //    layout can evolve without breaking callers) ---------------------------
    juce::TextButton startAudioButton   { "Start Audio" };
    juce::TextButton stopAudioButton    { "Stop Audio" };
    juce::TextButton startLoggingButton { "Start Logging" };
    juce::TextButton pauseLoggingButton { "Pause" };      // latching
    juce::TextButton stopLoggingButton  { "Stop Logging" };
    juce::TextButton muteButton         { "Mute" };       // latching
    juce::Slider     outputLevelSlider;                   // LinearHorizontal,
                                                          // textbox right, "dBFS" suffix
    juce::Label      outputLevelCaption { {}, "Output" };
    juce::TextButton markerButton       { "Marker" };
    juce::TextButton resetButton        { "Reset" };
    juce::TextButton exportButton       { "Export" };
    juce::TextButton deviceSetupButton  { "Audio Setup" };

    bool audioRunning     = false;
    bool loggingActive    = false;
    bool loggingPaused    = false;
    bool muted            = false;
    bool sessionAvailable = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportControls)
};

} // namespace pnam

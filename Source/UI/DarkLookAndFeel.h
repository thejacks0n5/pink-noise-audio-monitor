#pragma once
/*
  ==============================================================================
    DarkLookAndFeel.h
    Pink Noise Audio Monitor (pnam) — application-wide dark theme.

    A juce::LookAndFeel_V4 subclass tuned for an overnight test-monitoring
    tool: near-black backgrounds, a clear blue accent matching the app icon,
    and unambiguous meter/severity colours (green / amber / red).

    Ownership / usage:
      - MainComponent owns exactly one DarkLookAndFeel instance and calls
        juce::LookAndFeel::setDefaultLookAndFeel (&laf) in its constructor
        and setDefaultLookAndFeel (nullptr) in its destructor.
      - All UI components pull semantic colours through the static Palette
        helpers below rather than hard-coding values, so the theme is defined
        in exactly one place.

    MESSAGE THREAD ONLY (like all juce::Component / LookAndFeel code).
  ==============================================================================
*/

#include <juce_gui_basics/juce_gui_basics.h>

namespace pnam
{

//==============================================================================
/** Central colour + typography constants for the dark theme.

    Values are AARRGGBB. Components use Palette::get (Palette::accent) etc.
    (returns juce::Colour) so the raw constants stay POD and constexpr.
*/
struct Palette
{
    // -- Surfaces -------------------------------------------------------------
    static constexpr juce::uint32 windowBackground   = 0xff0a0b0d; // app window
    static constexpr juce::uint32 panelBackground    = 0xff15171b; // cards/panels
    static constexpr juce::uint32 panelBackgroundAlt = 0xff1a1c21; // alternating rows
    static constexpr juce::uint32 controlBackground  = 0xff23262d; // buttons, combos, editors
    static constexpr juce::uint32 controlHover       = 0xff2b2e37;
    static constexpr juce::uint32 controlPressed     = 0xff363a45;
    static constexpr juce::uint32 outline            = 0xff353942; // hairline borders
    static constexpr juce::uint32 outlineFocused     = 0xff55a4ff;

    // -- Text -----------------------------------------------------------------
    static constexpr juce::uint32 textPrimary        = 0xffe8eaee;
    static constexpr juce::uint32 textSecondary      = 0xffa4aab5;
    static constexpr juce::uint32 textDisabled       = 0xff6e7480;
    static constexpr juce::uint32 textOnAccent       = 0xff050d1a;

    // -- Accent (the icon's blue) ---------------------------------------------
    static constexpr juce::uint32 accent             = 0xff3391ff;
    static constexpr juce::uint32 accentDim          = 0xff3585e6;
    static constexpr juce::uint32 accentBright       = 0xff55a4ff;

    // -- Meters / signal ------------------------------------------------------
    static constexpr juce::uint32 meterGreen         = 0xff3bd1a9;
    static constexpr juce::uint32 meterAmber         = 0xffffd452;
    static constexpr juce::uint32 meterRed           = 0xffff6450;
    static constexpr juce::uint32 meterTrough        = 0xff0e0f12; // unlit bar area
    static constexpr juce::uint32 meterPeakTick      = 0xfff6f7f9; // peak-hold line
    static constexpr juce::uint32 meterRmsOverlay    = 0x4dffffff; // translucent RMS cap

    // -- Waveform traces ------------------------------------------------------
    static constexpr juce::uint32 traceReference     = 0xffcfd4e6; // near-white: kept clear of the blue accent
    static constexpr juce::uint32 traceInputL        = 0xffc7eb75; // yellow-green
    static constexpr juce::uint32 traceInputR        = 0xffa8eb75; // green
    static constexpr juce::uint32 traceGrid          = 0xff1c1e24;
    static constexpr juce::uint32 traceCentreLine    = 0xff2d3039;

    // -- Severity / status ----------------------------------------------------
    static constexpr juce::uint32 severityInfo       = 0xffbcc2cd;
    static constexpr juce::uint32 severityWarning    = 0xffffd452;
    static constexpr juce::uint32 severityCritical   = 0xffff6450;
    static constexpr juce::uint32 statusRunning      = 0xff3bd1a9; // lamp: running / logging
    static constexpr juce::uint32 statusIdle         = 0xff555a65; // lamp: stopped
    static constexpr juce::uint32 statusPaused       = 0xffffd452; // lamp: paused / settling

    // -- Meter dB colour-zone boundaries (dBFS) -------------------------------
    static constexpr float meterAmberFromDb = -12.0f; // >= this -> amber
    static constexpr float meterRedFromDb   =  -3.0f; // >= this -> red

    /** Convert one of the constants above to a juce::Colour. */
    static juce::Colour get (juce::uint32 argb) noexcept   { return juce::Colour (argb); }

    /** Colour for a bar segment at the given level (green/amber/red zones). */
    static juce::Colour meterColourForDb (float db) noexcept;

    /** Colour for a Severity value (pass the int32 enum value from DataModel). */
    static juce::Colour severityColour (int severity) noexcept;
};

//==============================================================================
class DarkLookAndFeel : public juce::LookAndFeel_V4
{
public:
    /** Installs the full dark ColourScheme and per-widget colour ids
        (buttons, sliders, combo boxes, labels, text editors, list boxes,
        table headers, scrollbars, popup menus, tooltips, alert windows). */
    DarkLookAndFeel();
    ~DarkLookAndFeel() override;

    //==========================================================================
    // Typography
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;

    /** Monospaced font used for numeric readouts (dBFS values, elapsed time,
        event-log cells). Centralised so every readout matches. */
    static juce::Font getMonoFont (float height);

    //==========================================================================
    // Buttons — flat rounded-rect fill, hairline outline, accent when
    // toggled-on ("latched" transport states such as Mute / Pause Logging).
    void drawButtonBackground (juce::Graphics&, juce::Button&,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted,
                         bool shouldDrawButtonAsDown) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted,
                           bool shouldDrawButtonAsDown) override;

    //==========================================================================
    // Sliders — slim dark track, accent-filled active portion, small flat
    // circular thumb; used by the Output Level (dBFS) slider.
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    int getSliderThumbRadius (juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;

    //==========================================================================
    // Combo boxes & popup menus (device lists, filter dropdowns)
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;

    //==========================================================================
    // Table / list chrome (event log)
    void drawTableHeaderBackground (juce::Graphics&, juce::TableHeaderComponent&) override;
    void drawTableHeaderColumn (juce::Graphics&, juce::TableHeaderComponent&,
                                const juce::String& columnName, int columnId,
                                int width, int height, bool isMouseOver,
                                bool isMouseDown, int columnFlags) override;
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y,
                        int width, int height, bool isScrollbarVertical,
                        int thumbStartPosition, int thumbSize, bool isMouseOver,
                        bool isMouseDown) override;

    //==========================================================================
    // Misc chrome
    void drawGroupComponentOutline (juce::Graphics&, int w, int h,
                                    const juce::String& text,
                                    const juce::Justification&,
                                    juce::GroupComponent&) override;
    void drawTooltip (juce::Graphics&, const juce::String& text,
                      int width, int height) override;
    void fillTextEditorBackground (juce::Graphics&, int width, int height,
                                   juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height,
                                juce::TextEditor&) override;

    //==========================================================================
    /** Shared corner radius for buttons/panels so every rounded rect matches. */
    static constexpr float cornerRadius = 4.0f;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DarkLookAndFeel)
};

} // namespace pnam

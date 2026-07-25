/*
  ==============================================================================
    DarkLookAndFeel.cpp
    Pink Noise Audio Monitor (pnam)
  ==============================================================================
*/

#include "DarkLookAndFeel.h"

namespace pnam
{

//==============================================================================
juce::Colour Palette::meterColourForDb (float db) noexcept
{
    if (db >= meterRedFromDb)
        return get (meterRed);
    if (db >= meterAmberFromDb)
        return get (meterAmber);
    return get (meterGreen);
}

juce::Colour Palette::severityColour (int severity) noexcept
{
    switch (severity)
    {
        case (int) Severity::Warning:  return get (severityWarning);
        case (int) Severity::Critical: return get (severityCritical);
        case (int) Severity::Info:
        default:                       return get (severityInfo);
    }
}

//==============================================================================
DarkLookAndFeel::DarkLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, Palette::get (Palette::windowBackground));

    setColour (juce::TextButton::buttonColourId,   Palette::get (Palette::controlBackground));
    setColour (juce::TextButton::buttonOnColourId, Palette::get (Palette::accentDim));
    setColour (juce::TextButton::textColourOffId,  Palette::get (Palette::textPrimary));
    setColour (juce::TextButton::textColourOnId,   Palette::get (Palette::textOnAccent));

    setColour (juce::ToggleButton::textColourId,        Palette::get (Palette::textPrimary));
    setColour (juce::ToggleButton::tickColourId,         Palette::get (Palette::accent));
    setColour (juce::ToggleButton::tickDisabledColourId, Palette::get (Palette::textDisabled));

    setColour (juce::Slider::backgroundColourId,     Palette::get (Palette::controlBackground));
    setColour (juce::Slider::trackColourId,          Palette::get (Palette::controlBackground));
    setColour (juce::Slider::thumbColourId,           Palette::get (Palette::accent));
    setColour (juce::Slider::textBoxTextColourId,     Palette::get (Palette::textPrimary));
    setColour (juce::Slider::textBoxBackgroundColourId, Palette::get (Palette::controlBackground));
    setColour (juce::Slider::textBoxOutlineColourId,  Palette::get (Palette::outline));

    setColour (juce::ComboBox::backgroundColourId, Palette::get (Palette::controlBackground));
    setColour (juce::ComboBox::textColourId,       Palette::get (Palette::textPrimary));
    setColour (juce::ComboBox::outlineColourId,    Palette::get (Palette::outline));
    setColour (juce::ComboBox::arrowColourId,      Palette::get (Palette::textSecondary));

    setColour (juce::PopupMenu::backgroundColourId,        Palette::get (Palette::panelBackground));
    setColour (juce::PopupMenu::textColourId,              Palette::get (Palette::textPrimary));
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Palette::get (Palette::controlHover));
    setColour (juce::PopupMenu::highlightedTextColourId,   Palette::get (Palette::textPrimary));

    setColour (juce::Label::textColourId, Palette::get (Palette::textPrimary));

    setColour (juce::TextEditor::backgroundColourId, Palette::get (Palette::controlBackground));
    setColour (juce::TextEditor::textColourId,        Palette::get (Palette::textPrimary));
    setColour (juce::TextEditor::outlineColourId,     Palette::get (Palette::outline));
    setColour (juce::TextEditor::focusedOutlineColourId, Palette::get (Palette::outlineFocused));

    setColour (juce::ListBox::backgroundColourId, Palette::get (Palette::panelBackground));
    setColour (juce::ListBox::textColourId,       Palette::get (Palette::textPrimary));

    setColour (juce::TableHeaderComponent::backgroundColourId, Palette::get (Palette::panelBackgroundAlt));
    setColour (juce::TableHeaderComponent::textColourId,       Palette::get (Palette::textSecondary));
    setColour (juce::TableHeaderComponent::outlineColourId,    Palette::get (Palette::outline));

    setColour (juce::ScrollBar::backgroundColourId, Palette::get (Palette::panelBackground));
    setColour (juce::ScrollBar::thumbColourId,      Palette::get (Palette::controlHover));

    setColour (juce::GroupComponent::outlineColourId, Palette::get (Palette::outline));
    setColour (juce::GroupComponent::textColourId,    Palette::get (Palette::textSecondary));

    setColour (juce::TooltipWindow::backgroundColourId, Palette::get (Palette::panelBackground));
    setColour (juce::TooltipWindow::textColourId,       Palette::get (Palette::textPrimary));
    setColour (juce::TooltipWindow::outlineColourId,    Palette::get (Palette::outline));

    setColour (juce::AlertWindow::backgroundColourId, Palette::get (Palette::panelBackground));
    setColour (juce::AlertWindow::textColourId,       Palette::get (Palette::textPrimary));
    setColour (juce::AlertWindow::outlineColourId,    Palette::get (Palette::outline));
}

DarkLookAndFeel::~DarkLookAndFeel() = default;

//==============================================================================
juce::Font DarkLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::Font (juce::FontOptions (juce::jmin (16.0f, (float) buttonHeight * 0.5f)));
}

juce::Font DarkLookAndFeel::getLabelFont (juce::Label& label)
{
    return label.getFont();
}

juce::Font DarkLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return juce::Font (juce::FontOptions (juce::jmin (16.0f, (float) box.getHeight() * 0.55f)));
}

juce::Font DarkLookAndFeel::getPopupMenuFont()
{
    return juce::Font (juce::FontOptions (15.0f));
}

juce::Font DarkLookAndFeel::getMonoFont (float height)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height, juce::Font::plain));
}

//==============================================================================
void DarkLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                            const juce::Colour& backgroundColour,
                                            bool shouldDrawButtonAsHighlighted,
                                            bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);

    juce::Colour fill = backgroundColour;

    if (button.getToggleState())
        fill = Palette::get (Palette::accentDim);

    if (shouldDrawButtonAsDown)
        fill = fill.darker (0.2f);
    else if (shouldDrawButtonAsHighlighted)
        fill = fill.brighter (0.08f);

    g.setColour (fill);
    g.fillRoundedRectangle (bounds, cornerRadius);

    g.setColour (button.hasKeyboardFocus (true) ? Palette::get (Palette::outlineFocused)
                                                 : Palette::get (Palette::outline));
    g.drawRoundedRectangle (bounds, cornerRadius, 1.0f);
}

void DarkLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                      bool /*shouldDrawButtonAsHighlighted*/,
                                      bool /*shouldDrawButtonAsDown*/)
{
    auto font = getTextButtonFont (button, button.getHeight());
    g.setFont (font);

    g.setColour (button.isEnabled()
                     ? (button.getToggleState() ? Palette::get (Palette::textOnAccent)
                                                 : Palette::get (Palette::textPrimary))
                     : Palette::get (Palette::textDisabled));

    g.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (6, 0),
                      juce::Justification::centred, 1);
}

void DarkLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                        bool shouldDrawButtonAsHighlighted,
                                        bool /*shouldDrawButtonAsDown*/)
{
    auto bounds = button.getLocalBounds();
    const int tickSize = juce::jmin (18, bounds.getHeight() - 4);
    auto tickBounds = juce::Rectangle<int> (bounds.getX(), bounds.getCentreY() - tickSize / 2, tickSize, tickSize);

    g.setColour (Palette::get (Palette::controlBackground));
    g.fillRoundedRectangle (tickBounds.toFloat(), 3.0f);

    g.setColour (shouldDrawButtonAsHighlighted ? Palette::get (Palette::outlineFocused)
                                                : Palette::get (Palette::outline));
    g.drawRoundedRectangle (tickBounds.toFloat(), 3.0f, 1.0f);

    if (button.getToggleState())
    {
        g.setColour (Palette::get (Palette::accent));
        g.fillRoundedRectangle (tickBounds.toFloat().reduced (3.0f), 2.0f);
    }

    g.setColour (button.isEnabled() ? Palette::get (Palette::textPrimary) : Palette::get (Palette::textDisabled));
    g.setFont (juce::Font (juce::FontOptions (14.0f)));
    auto textBounds = bounds.withTrimmedLeft (tickSize + 8);
    g.drawFittedText (button.getButtonText(), textBounds, juce::Justification::centredLeft, 1);
}

//==============================================================================
void DarkLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                        float sliderPos, float minSliderPos, float maxSliderPos,
                                        juce::Slider::SliderStyle style, juce::Slider& slider)
{
    juce::ignoreUnused (minSliderPos, maxSliderPos);

    if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearBar)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const float trackY = (float) y + (float) height * 0.5f;
    const float trackH = 4.0f;
    juce::Rectangle<float> track ((float) x, trackY - trackH * 0.5f, (float) width, trackH);

    g.setColour (Palette::get (Palette::controlBackground));
    g.fillRoundedRectangle (track, trackH * 0.5f);

    juce::Rectangle<float> active ((float) x, trackY - trackH * 0.5f, sliderPos - (float) x, trackH);
    g.setColour (Palette::get (Palette::accent));
    g.fillRoundedRectangle (active, trackH * 0.5f);

    const float thumbRadius = (float) getSliderThumbRadius (slider);
    g.setColour (Palette::get (Palette::accentBright));
    g.fillEllipse (sliderPos - thumbRadius, trackY - thumbRadius, thumbRadius * 2.0f, thumbRadius * 2.0f);
    g.setColour (Palette::get (Palette::outline));
    g.drawEllipse (sliderPos - thumbRadius, trackY - thumbRadius, thumbRadius * 2.0f, thumbRadius * 2.0f, 1.0f);
}

int DarkLookAndFeel::getSliderThumbRadius (juce::Slider&)
{
    return 6;
}

juce::Label* DarkLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* label = LookAndFeel_V4::createSliderTextBox (slider);
    label->setFont (getMonoFont (14.0f));
    label->setColour (juce::Label::textColourId, Palette::get (Palette::textPrimary));
    label->setColour (juce::Label::backgroundColourId, Palette::get (Palette::controlBackground));
    label->setColour (juce::Label::outlineColourId, Palette::get (Palette::outline));
    return label;
}

//==============================================================================
void DarkLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool /*isButtonDown*/,
                                    int buttonX, int buttonY, int buttonW, int buttonH,
                                    juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);

    g.setColour (Palette::get (Palette::controlBackground));
    g.fillRoundedRectangle (bounds, cornerRadius);

    g.setColour (box.hasKeyboardFocus (true) ? Palette::get (Palette::outlineFocused)
                                              : Palette::get (Palette::outline));
    g.drawRoundedRectangle (bounds, cornerRadius, 1.0f);

    juce::Rectangle<float> arrowZone ((float) buttonX, (float) buttonY, (float) buttonW, (float) buttonH);
    juce::Path arrow;
    const float cx = arrowZone.getCentreX();
    const float cy = arrowZone.getCentreY();
    arrow.addTriangle (cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 3.0f);
    g.setColour (Palette::get (Palette::textSecondary));
    g.fillPath (arrow);
}

void DarkLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.setColour (Palette::get (Palette::panelBackground));
    g.fillRect (0, 0, width, height);
    g.setColour (Palette::get (Palette::outline));
    g.drawRect (0, 0, width, height, 1);
}

//==============================================================================
void DarkLookAndFeel::drawTableHeaderBackground (juce::Graphics& g, juce::TableHeaderComponent& header)
{
    auto bounds = header.getLocalBounds();
    g.setColour (Palette::get (Palette::panelBackgroundAlt));
    g.fillRect (bounds);
    g.setColour (Palette::get (Palette::outline));
    g.drawLine ((float) bounds.getX(), (float) bounds.getBottom() - 0.5f,
               (float) bounds.getRight(), (float) bounds.getBottom() - 0.5f);
}

void DarkLookAndFeel::drawTableHeaderColumn (juce::Graphics& g, juce::TableHeaderComponent&,
                                             const juce::String& columnName, int /*columnId*/,
                                             int width, int height, bool isMouseOver,
                                             bool isMouseDown, int /*columnFlags*/)
{
    if (isMouseDown)
        g.setColour (Palette::get (Palette::controlPressed));
    else if (isMouseOver)
        g.setColour (Palette::get (Palette::controlHover));
    else
        g.setColour (Palette::get (Palette::panelBackgroundAlt));

    g.fillRect (0, 0, width, height);

    g.setColour (Palette::get (Palette::textSecondary));
    g.setFont (juce::Font (juce::FontOptions (13.0f)));
    g.drawFittedText (columnName, juce::Rectangle<int> (8, 0, width - 12, height),
                      juce::Justification::centredLeft, 1);

    g.setColour (Palette::get (Palette::outline));
    g.drawVerticalLine (width - 1, 0.0f, (float) height);
}

void DarkLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar& scrollbar, int x, int y,
                                     int width, int height, bool isScrollbarVertical,
                                     int thumbStartPosition, int thumbSize, bool isMouseOver,
                                     bool isMouseDown)
{
    juce::ignoreUnused (scrollbar);
    juce::Rectangle<int> trackBounds (x, y, width, height);
    g.setColour (Palette::get (Palette::panelBackground));
    g.fillRect (trackBounds);

    juce::Rectangle<int> thumbBounds = isScrollbarVertical
        ? juce::Rectangle<int> (x + 2, thumbStartPosition, width - 4, thumbSize)
        : juce::Rectangle<int> (thumbStartPosition, y + 2, thumbSize, height - 4);

    auto colour = Palette::get (Palette::controlHover);
    if (isMouseDown)
        colour = Palette::get (Palette::controlPressed);
    else if (isMouseOver)
        colour = colour.brighter (0.1f);

    g.setColour (colour);
    g.fillRoundedRectangle (thumbBounds.toFloat(), 3.0f);
}

//==============================================================================
void DarkLookAndFeel::drawGroupComponentOutline (juce::Graphics& g, int w, int h,
                                                 const juce::String& text,
                                                 const juce::Justification& justification,
                                                 juce::GroupComponent& group)
{
    const float textH = 15.0f;
    juce::Font font (juce::FontOptions (textH));
    juce::Rectangle<int> bounds (0, 0, w, h);
    juce::Rectangle<int> top = bounds.removeFromTop ((int) textH + 4);

    g.setColour (Palette::get (Palette::outline));
    juce::Rectangle<float> outlineBounds = bounds.toFloat().reduced (0.5f);
    g.drawRoundedRectangle (outlineBounds, cornerRadius, 1.0f);

    if (text.isNotEmpty())
    {
        g.setColour (group.findColour (juce::GroupComponent::textColourId));
        g.setFont (font);
        g.drawFittedText (text, top.reduced (6, 0), justification.getFlags(), 1);
    }
}

void DarkLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    juce::Rectangle<float> bounds (0.0f, 0.0f, (float) width, (float) height);
    g.setColour (Palette::get (Palette::panelBackground));
    g.fillRoundedRectangle (bounds, cornerRadius);
    g.setColour (Palette::get (Palette::outline));
    g.drawRoundedRectangle (bounds.reduced (0.5f), cornerRadius, 1.0f);

    g.setColour (Palette::get (Palette::textPrimary));
    g.setFont (juce::Font (juce::FontOptions (13.0f)));
    g.drawFittedText (text, bounds.reduced (6.0f, 4.0f).toNearestInt(), juce::Justification::centredLeft, 4);
}

void DarkLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    g.setColour (editor.findColour (juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle (0.0f, 0.0f, (float) width, (float) height, cornerRadius);
}

void DarkLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (editor.isEnabled())
    {
        juce::Rectangle<float> bounds (0.0f, 0.0f, (float) width, (float) height);
        g.setColour (editor.hasKeyboardFocus (true)
                         ? editor.findColour (juce::TextEditor::focusedOutlineColourId)
                         : editor.findColour (juce::TextEditor::outlineColourId));
        g.drawRoundedRectangle (bounds.reduced (0.5f), cornerRadius, 1.0f);
    }
}

} // namespace pnam

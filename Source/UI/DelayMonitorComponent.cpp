/*
  ==============================================================================
    DelayMonitorComponent.cpp
    Pink Noise Audio Monitor (pnam)
  ==============================================================================
*/

#include "DelayMonitorComponent.h"
#include "DarkLookAndFeel.h"

#include <cmath>

namespace pnam
{

namespace
{
    constexpr int titleWidth   = 58;
    constexpr int minCellWidth = 190;
    constexpr int maxCellWidth = 300;
    constexpr int overflowWidth = 44;     // room for "+N" when not every input fits

    // A reading that has moved less than this is the same delay settling, and
    // is eased towards; a bigger move is a different delay, and is shown at once.
    constexpr double sameDelayMs = 0.5;
    constexpr double easing      = 0.15;

    // (A calibrated reading sits a hair either side of zero; never show "-0.00".)
    juce::String formatMs (double ms)   { return juce::String (std::abs (ms) < 0.005 ? 0.0 : ms, 2); }
}

//==============================================================================
DelayMonitorComponent::DelayMonitorComponent()
{
    for (auto* b : { &calibrateButton, &offsetButton, &clearButton })
        addAndMakeVisible (b);

    calibrateButton.onClick = [this] { if (onCalibrate) onCalibrate(); };
    clearButton.onClick     = [this] { if (onClear) onClear(); };
    offsetButton.onClick    = [this] { showOffsetMenu(); };

    updateButtons();
}

//==============================================================================
void DelayMonitorComponent::setChannels (const juce::StringArray& inputNames)
{
    bool changed = cells.size() != inputNames.size();

    while (cells.size() > inputNames.size())
        cells.removeLast();

    while (cells.size() < inputNames.size())
        cells.add ({});

    for (int i = 0; i < cells.size(); ++i)
    {
        if (cells.getReference (i).name != inputNames[i])
        {
            cells.getReference (i).name = inputNames[i];
            changed = true;
        }
    }

    if (changed)
    {
        for (auto& cell : cells)
            refreshCellText (cell);

        updateButtons();
        repaint();
    }
}

void DelayMonitorComponent::setOffsetsMs (const juce::Array<double>& offsetsMs)
{
    bool changed = false;

    for (int i = 0; i < cells.size(); ++i)
    {
        const double offset = i < offsetsMs.size() ? offsetsMs[i] : 0.0;
        auto& cell = cells.getReference (i);

        if (! juce::exactlyEqual (cell.offsetMs, offset))
        {
            cell.offsetMs = offset;
            refreshCellText (cell);
            changed = true;
        }
    }

    if (changed)
    {
        updateButtons();
        repaint();
    }
}

void DelayMonitorComponent::setSampleRate (double newSampleRate)
{
    if (juce::exactlyEqual (sampleRate, newSampleRate))
        return;

    sampleRate = newSampleRate;

    for (auto& cell : cells)
        refreshCellText (cell);

    repaint();
}

void DelayMonitorComponent::setReading (int input, State state, double measuredRoundTripMs)
{
    if (! juce::isPositiveAndBelow (input, cells.size()))
        return;

    auto& cell = cells.getReference (input);
    const bool wasAligned = cell.state == State::aligned;
    const auto before = cell.valueText + cell.samplesText + cell.detailText;
    const auto stateBefore = cell.state;

    cell.state = state;

    if (state == State::aligned)
    {
        if (wasAligned && std::abs (measuredRoundTripMs - cell.roundTripMs) < sameDelayMs)
            cell.roundTripMs += easing * (measuredRoundTripMs - cell.roundTripMs);
        else
            cell.roundTripMs = measuredRoundTripMs;
    }

    refreshCellText (cell);

    if (stateBefore != state)
        updateButtons();

    if (stateBefore != state || before != cell.valueText + cell.samplesText + cell.detailText)
        repaint();
}

bool DelayMonitorComponent::isAligned (int input) const noexcept
{
    return juce::isPositiveAndBelow (input, cells.size()) && cells.getReference (input).state == State::aligned;
}

double DelayMonitorComponent::getRoundTripMs (int input) const noexcept
{
    return juce::isPositiveAndBelow (input, cells.size()) ? cells.getReference (input).roundTripMs : 0.0;
}

//==============================================================================
void DelayMonitorComponent::refreshCellText (Cell& cell)
{
    const bool calibrated = ! juce::exactlyEqual (cell.offsetMs, 0.0);

    if (cell.state == State::aligned)
    {
        double delayMs = cell.roundTripMs - cell.offsetMs;

        if (std::abs (delayMs) < 0.005)
            delayMs = 0.0;

        cell.valueText   = formatMs (delayMs) + " ms";
        cell.samplesText = sampleRate > 0.0 ? juce::String (juce::roundToInt (delayMs * sampleRate / 1000.0)) + " smp"
                                            : juce::String();
        cell.detailText  = calibrated ? "round trip " + formatMs (cell.roundTripMs) + " - cal " + formatMs (cell.offsetMs)
                                      : juce::String ("round trip, not calibrated");
        return;
    }

    cell.valueText   = juce::String::fromUTF8 ("\xe2\x80\x94");
    cell.samplesText = {};

    switch (cell.state)
    {
        case State::stopped:       cell.detailText = "audio stopped"; break;
        case State::detectionOff:  cell.detailText = "detection is off in Settings"; break;
        case State::notMonitored:  cell.detailText = "not monitored"; break;
        case State::searching:     cell.detailText = "finding the return..."; break;
        case State::aligned:       break;
    }

    if (calibrated)
        cell.detailText << "  (cal " << formatMs (cell.offsetMs) << ")";
}

void DelayMonitorComponent::updateButtons()
{
    bool anyAligned = false, anyOffset = false;

    for (const auto& cell : cells)
    {
        anyAligned = anyAligned || cell.state == State::aligned;
        anyOffset  = anyOffset  || ! juce::exactlyEqual (cell.offsetMs, 0.0);
    }

    calibrateButton.setEnabled (anyAligned);
    offsetButton.setEnabled (! cells.isEmpty());
    clearButton.setEnabled (anyOffset);
}

void DelayMonitorComponent::showOffsetMenu()
{
    if (cells.isEmpty() || onEditOffset == nullptr)
        return;

    if (cells.size() == 1)
    {
        onEditOffset (0);
        return;
    }

    juce::PopupMenu menu;

    for (int i = 0; i < cells.size(); ++i)
        menu.addItem (i + 1, cells.getReference (i).name + "   (" + juce::String (cells.getReference (i).offsetMs, 3) + " ms)");

    juce::Component::SafePointer<DelayMonitorComponent> safeThis (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&offsetButton),
                        [safeThis] (int chosen)
                        {
                            if (chosen > 0 && safeThis != nullptr && safeThis->onEditOffset)
                                safeThis->onEditOffset (chosen - 1);
                        });
}

//==============================================================================
juce::Rectangle<int> DelayMonitorComponent::getCellArea() const
{
    auto area = getLocalBounds().reduced (6, 4);
    area.removeFromLeft (titleWidth);
    area.setRight (calibrateButton.getX() - 8);
    return area;
}

void DelayMonitorComponent::resized()
{
    auto area = getLocalBounds().reduced (6, 12);

    clearButton.setBounds (area.removeFromRight (58));
    area.removeFromRight (6);
    offsetButton.setBounds (area.removeFromRight (78));
    area.removeFromRight (6);
    calibrateButton.setBounds (area.removeFromRight (86));
}

void DelayMonitorComponent::paint (juce::Graphics& g)
{
    g.fillAll (Palette::get (Palette::panelBackground));

    g.setColour (Palette::get (Palette::textSecondary));
    g.setFont (juce::Font (juce::FontOptions (13.0f)));
    g.drawText ("DELAY", getLocalBounds().reduced (6, 4).removeFromLeft (titleWidth), juce::Justification::centredLeft, false);

    auto area = getCellArea();

    if (cells.isEmpty() || area.getWidth() < 60)
        return;

    // As many cells as fit at a readable width; the rest are counted.
    int shown = juce::jmin (cells.size(), juce::jmax (1, area.getWidth() / minCellWidth));

    if (shown < cells.size())
        shown = juce::jmin (cells.size(), juce::jmax (1, (area.getWidth() - overflowWidth) / minCellWidth));

    const int available = area.getWidth() - (shown < cells.size() ? overflowWidth : 0);
    const int cellWidth = juce::jlimit (juce::jmin (minCellWidth, available), maxCellWidth, available / shown);

    for (int i = 0; i < shown; ++i)
    {
        const auto& cell = cells.getReference (i);
        auto r = area.removeFromLeft (cellWidth).reduced (4, 0);
        const bool live = cell.state == State::aligned;

        if (i > 0)
        {
            g.setColour (Palette::get (Palette::outline));
            g.fillRect (r.getX() - 4, r.getY() + 4, 1, r.getHeight() - 8);
        }

        g.setColour (Palette::get (Palette::textSecondary));
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (cell.name, r.removeFromTop (13), juce::Justification::centredLeft, true);

        auto detail = r.removeFromBottom (13);
        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        g.drawText (cell.detailText, detail, juce::Justification::centredLeft, true);

        const auto valueFont = DarkLookAndFeel::getMonoFont (21.0f);
        g.setFont (valueFont);
        g.setColour (Palette::get (live ? Palette::textPrimary : Palette::textDisabled));

        const int valueWidth = juce::jmin (r.getWidth(), juce::GlyphArrangement::getStringWidthInt (valueFont, cell.valueText) + 10);
        g.drawText (cell.valueText, r.removeFromLeft (valueWidth), juce::Justification::centredLeft, false);

        if (cell.samplesText.isNotEmpty())
        {
            g.setColour (Palette::get (Palette::textSecondary));
            g.setFont (DarkLookAndFeel::getMonoFont (12.0f));
            g.drawText (cell.samplesText, r.withTrimmedTop (5), juce::Justification::centredLeft, true);
        }
    }

    if (shown < cells.size())
    {
        g.setColour (Palette::get (Palette::textSecondary));
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText ("+" + juce::String (cells.size() - shown), area, juce::Justification::centred, false);
    }
}

} // namespace pnam

/*
  ==============================================================================
    TopStatusBar.cpp
    Pink Noise Audio Monitor (pnam)
  ==============================================================================
*/

#include "TopStatusBar.h"

namespace pnam
{

namespace
{
    /*  juce::String's const char* constructor asserts on any byte > 127, so the
        non-ASCII glyphs used in the status bar must be built via fromUTF8.
        These fired ~30x/sec (once per timer tick) before being routed here. */
    static const juce::String kEmDash   = juce::String::fromUTF8 ("\xe2\x80\x94");  // U+2014 —
    static const juce::String kEllipsis = juce::String::fromUTF8 ("\xe2\x80\xa6");  // U+2026 …
}

//==============================================================================
TopStatusBar::TopStatusBar()
{
    audioCell.caption      = "AUDIO";
    loggingCell.caption    = "LOG";
    durationCell.caption   = "SESSION";
    modeCell.caption       = "MODE";
    outDeviceCell.caption  = "OUT";
    inDeviceCell.caption   = "IN";
    outChannelsCell.caption = "OUT CH";
    inChannelsCell.caption  = "IN CH";
    rateCell.caption       = "RATE";
    bufferCell.caption     = "BUFFER";
    appStatusCell.caption  = {};

    audioCell.value     = "Stopped";
    audioCell.hasLamp   = true;
    audioCell.lamp      = Palette::get (Palette::statusIdle);

    loggingCell.value   = "Off";
    loggingCell.hasLamp = true;
    loggingCell.lamp    = Palette::get (Palette::statusIdle);

    durationCell.value  = kEmDash;
    modeCell.value       = "Pink noise loopback";
    outDeviceCell.value  = kEmDash;
    inDeviceCell.value   = kEmDash;
    outChannelsCell.value = kEmDash;
    inChannelsCell.value  = kEmDash;
    rateCell.value       = kEmDash;
    bufferCell.value     = kEmDash;
    appStatusCell.value  = "Idle";

    audioCell.id     = CellId::audio;
    loggingCell.id   = CellId::logging;
    durationCell.id  = CellId::duration;
    modeCell.id      = CellId::mode;
    outDeviceCell.id = CellId::outDevice;
    inDeviceCell.id  = CellId::inDevice;
    outChannelsCell.id = CellId::outChannels;
    inChannelsCell.id  = CellId::inChannels;
    rateCell.id      = CellId::rate;
    bufferCell.id    = CellId::buffer;
    appStatusCell.id = CellId::appStatus;

    // The audio-device cells are click-to-edit; everything else is display-only.
    outDeviceCell.interactive = true;
    inDeviceCell.interactive  = true;
    outChannelsCell.interactive = true;
    inChannelsCell.interactive  = true;
    rateCell.interactive      = true;
    bufferCell.interactive    = true;
}

//==============================================================================
TopStatusBar::Cell* TopStatusBar::cellAt (juce::Point<int> p)
{
    Cell* cells[] = { &audioCell, &loggingCell, &durationCell, &modeCell,
                      &outDeviceCell, &inDeviceCell, &outChannelsCell, &inChannelsCell,
                      &rateCell, &bufferCell,
                      &appStatusCell };

    for (auto* cell : cells)
        if (! cell->bounds.isEmpty() && cell->bounds.contains (p))
            return cell;

    return nullptr;
}

void TopStatusBar::mouseDown (const juce::MouseEvent& e)
{
    auto* cell = cellAt (e.getPosition());

    if (cell == nullptr || ! cell->interactive || onCellClicked == nullptr)
        return;

    onCellClicked (cell->id, localAreaToGlobal (cell->bounds));
}

void TopStatusBar::mouseMove (const juce::MouseEvent& e)
{
    auto* cell = cellAt (e.getPosition());
    const auto nowHovered = (cell != nullptr && cell->interactive) ? cell->id : CellId::none;

    if (nowHovered == hoveredCell)
        return;

    hoveredCell = nowHovered;
    setMouseCursor (hoveredCell == CellId::none ? juce::MouseCursor::NormalCursor
                                                : juce::MouseCursor::PointingHandCursor);
    repaint();
}

void TopStatusBar::mouseExit (const juce::MouseEvent&)
{
    if (hoveredCell == CellId::none)
        return;

    hoveredCell = CellId::none;
    setMouseCursor (juce::MouseCursor::NormalCursor);
    repaint();
}

TopStatusBar::~TopStatusBar() = default;

//==============================================================================
void TopStatusBar::updateCell (Cell& cell, const juce::String& newValue, juce::Colour lampColour, bool repaintNow)
{
    bool changed = false;

    if (cell.value != newValue)
    {
        cell.value = newValue;
        changed = true;
    }

    if (cell.hasLamp && cell.lamp != lampColour)
    {
        cell.lamp = lampColour;
        changed = true;
    }

    if (changed && repaintNow)
        repaint (cell.bounds);
}

juce::String TopStatusBar::formatDuration (double seconds)
{
    if (seconds < 0.0)
        return kEmDash;

    const int totalSeconds = (int) seconds;
    const int hh = totalSeconds / 3600;
    const int mm = (totalSeconds % 3600) / 60;
    const int ss = totalSeconds % 60;

    return juce::String::formatted ("%02d:%02d:%02d", hh, mm, ss);
}

//==============================================================================
void TopStatusBar::setAppStatus (const juce::String& text)
{
    appStatusText = text;
    refreshAppStatusCell();
}

void TopStatusBar::setAlert (const juce::String& text)
{
    if (alertText == text)
        return;

    alertText = text;
    refreshAppStatusCell();
}

void TopStatusBar::refreshAppStatusCell()
{
    const bool showAlert = alertText.isNotEmpty();

    if (appStatusCell.alert != showAlert)
    {
        appStatusCell.alert = showAlert;
        repaint (appStatusCell.bounds);
    }

    updateCell (appStatusCell, showAlert ? alertText : appStatusText, {}, true);
}

void TopStatusBar::setAudioEngineState (EngineState state)
{
    if (engineState == state)
        return;

    engineState = state;

    juce::String text;
    juce::Colour lamp;

    switch (state)
    {
        case EngineState::stopped:     text = "Stopped";     lamp = Palette::get (Palette::statusIdle);       break;
        case EngineState::starting:    text = ("Starting" + kEllipsis);   lamp = Palette::get (Palette::statusPaused);     break;
        case EngineState::running:     text = "Running";     lamp = Palette::get (Palette::statusRunning);    break;
        case EngineState::deviceError: text = "Device error"; lamp = Palette::get (Palette::severityCritical); break;
        case EngineState::deviceLost:  text = "NO DEVICE";    lamp = Palette::get (Palette::severityCritical); break;
        case EngineState::outputHeld:  text = "Output held";  lamp = Palette::get (Palette::severityCritical); break;
    }

    audioCell.hasLamp = true;
    audioCell.alert   = (state == EngineState::deviceError || state == EngineState::deviceLost
                           || state == EngineState::outputHeld);
    updateCell (audioCell, text, lamp, true);
}

void TopStatusBar::setLoggingState (LoggingState state)
{
    if (loggingState == state)
        return;

    loggingState = state;

    juce::String text;
    juce::Colour lamp;

    switch (state)
    {
        case LoggingState::inactive:   text = "Off";         lamp = Palette::get (Palette::statusIdle);       break;
        case LoggingState::active:     text = "Logging";     lamp = Palette::get (Palette::statusRunning);    break;
        case LoggingState::paused:     text = "Paused";      lamp = Palette::get (Palette::statusPaused);     break;
        case LoggingState::writeError: text = "Write error"; lamp = Palette::get (Palette::severityCritical); break;
    }

    loggingCell.hasLamp = true;
    loggingCell.alert   = (state == LoggingState::writeError);
    updateCell (loggingCell, text, lamp, true);
}

void TopStatusBar::setLoggingPausedSeconds (double seconds)
{
    if (loggingState != LoggingState::paused)
        return;

    const int total = juce::jmax (0, (int) seconds);

    // Compact on purpose: the cell is narrow, and h:mm:ss only once it matters.
    const auto text = total >= 3600
                        ? juce::String::formatted ("Paused %d:%02d:%02d", total / 3600, (total % 3600) / 60, total % 60)
                        : juce::String::formatted ("Paused %d:%02d", total / 60, total % 60);

    updateCell (loggingCell, text, loggingCell.lamp, true);
}

void TopStatusBar::setOutputDeviceName (const juce::String& name)
{
    updateCell (outDeviceCell, name.isNotEmpty() ? name : kEmDash, {}, true);
}

void TopStatusBar::setInputDeviceName (const juce::String& name)
{
    updateCell (inDeviceCell, name.isNotEmpty() ? name : kEmDash, {}, true);
}

void TopStatusBar::setChannelConfig (int numOutputChannels, int numInputChannels)
{
    const auto describe = [] (int n) -> juce::String
    {
        if (n <= 0)  return kEmDash;
        if (n == 1)  return "1 ch (mono)";
        if (n == 2)  return "2 ch (stereo)";
        return juce::String (n) + " ch";
    };

    updateCell (outChannelsCell, describe (numOutputChannels), {}, true);
    updateCell (inChannelsCell,  describe (numInputChannels),  {}, true);
}

void TopStatusBar::setSampleRate (double sampleRateHz)
{
    lastSampleRateHz = sampleRateHz;

    juce::String text = sampleRateHz > 0.0
        ? juce::String (sampleRateHz / 1000.0, 1) + " kHz"
        : kEmDash;

    updateCell (rateCell, text, {}, true);
}

void TopStatusBar::setBufferSize (int bufferSizeSamples)
{
    if (bufferSizeSamples <= 0)
    {
        updateCell (bufferCell, kEmDash, {}, true);
        return;
    }

    juce::String text = juce::String (bufferSizeSamples) + " smp";

    if (lastSampleRateHz > 0.0)
    {
        const double ms = 1000.0 * (double) bufferSizeSamples / lastSampleRateHz;
        text << " (" << juce::String (ms, 1) << " ms)";
    }

    updateCell (bufferCell, text, {}, true);
}

void TopStatusBar::setSessionDuration (double seconds)
{
    updateCell (durationCell, formatDuration (seconds), {}, true);
}

void TopStatusBar::setTestMode (const juce::String& modeText)
{
    updateCell (modeCell, modeText, {}, true);
}

//==============================================================================
void TopStatusBar::paint (juce::Graphics& g)
{
    g.fillAll (Palette::get (Palette::panelBackground));

    Cell* cells[] = { &audioCell, &loggingCell, &durationCell, &modeCell,
                      &outDeviceCell, &inDeviceCell, &outChannelsCell, &inChannelsCell,
                      &rateCell, &bufferCell,
                      &appStatusCell };

    for (auto* cellPtr : cells)
    {
        auto& cell = *cellPtr;
        if (cell.bounds.isEmpty())
            continue;

        // Hover highlight so the click-to-edit cells announce themselves.
        if (cell.interactive && cell.id == hoveredCell)
        {
            g.setColour (Palette::get (Palette::controlHover));
            g.fillRoundedRectangle (cell.bounds.reduced (3, 4).toFloat(), 3.0f);
        }

        auto area = cell.bounds.reduced (6, 4);

        // Reserve room on the right for the disclosure chevron.
        juce::Rectangle<int> chevronArea;
        if (cell.interactive)
            chevronArea = area.removeFromRight (12);

        auto captionArea = area.removeFromTop (12);

        if (cell.interactive)
        {
            const float cx = (float) chevronArea.getCentreX();
            const float cy = (float) chevronArea.getCentreY() + 3.0f;
            const float w  = 3.5f;

            juce::Path chevron;
            chevron.startNewSubPath (cx - w, cy - w * 0.6f);
            chevron.lineTo          (cx,     cy + w * 0.6f);
            chevron.lineTo          (cx + w, cy - w * 0.6f);

            g.setColour (Palette::get (cell.id == hoveredCell ? Palette::textPrimary
                                                              : Palette::textSecondary));
            g.strokePath (chevron, juce::PathStrokeType (1.2f));
        }

        if (cell.hasLamp)
        {
            const int lampSize = 7;
            juce::Rectangle<int> lamp (captionArea.getX(), captionArea.getCentreY() - lampSize / 2, lampSize, lampSize);
            g.setColour (cell.lamp);
            g.fillEllipse (lamp.toFloat());
            captionArea.removeFromLeft (lampSize + 4);
        }

        if (cell.caption.isNotEmpty())
        {
            g.setColour (Palette::get (Palette::textSecondary));
            g.setFont (juce::Font (juce::FontOptions (9.5f)));
            g.drawFittedText (cell.caption, captionArea, juce::Justification::centredLeft, 1);
        }

        // A fault is red text, not just a red lamp: it has to be readable as
        // "something is wrong" from across the room.
        g.setColour (Palette::get (cell.alert ? Palette::severityCritical : Palette::textPrimary));
        g.setFont (juce::Font (juce::FontOptions (13.0f, cell.alert ? juce::Font::bold : juce::Font::plain)));

        // The app-status alert has no caption above it, so it takes the whole
        // cell height and may wrap onto a second line.
        const bool tallAlert = cell.alert && cell.caption.isEmpty();

        g.drawFittedText (cell.value, tallAlert ? cell.bounds.reduced (6, 4) : area,
                          juce::Justification::centredLeft, tallAlert ? 2 : 1);
    }

    // Divider hairlines between cells (skip before the first and the trailing spacer)
    g.setColour (Palette::get (Palette::outline));
    for (auto* cellPtr : cells)
    {
        if (cellPtr == &appStatusCell || cellPtr->bounds.isEmpty())
            continue;
        g.drawVerticalLine (cellPtr->bounds.getRight() - 1, 6.0f, (float) getHeight() - 6.0f);
    }
}

void TopStatusBar::resized()
{
    auto bounds = getLocalBounds();

    // Cells shrink proportionally rather than being clipped. The previous fixed
    // widths summed to more than the 1120 px default window, so the app-status
    // cell collapsed to nothing and BUFFER ran off the right edge — and the
    // interactive cells now also need room for their chevron.
    struct Spec { Cell* cell; int preferred; int minimum; };

    // The MODE cell is deliberately absent (and so keeps empty bounds, which
    // paint() and cellAt() skip): its text never changes in V1, and its 160 px
    // are what the app-status text needs to be readable. LOG is wider than it
    // was so "Paused 12:34" fits.
    const Spec specs[] =
    {
        { &audioCell,     105,  72 },
        { &loggingCell,   130,  96 },
        { &durationCell,   95,  62 },
        { &outDeviceCell, 165,  88 },
        { &inDeviceCell,  175,  88 },
        { &outChannelsCell, 120,  72 },
        { &inChannelsCell,  120,  72 },
        { &rateCell,      100,  70 },
        { &bufferCell,    155,  92 },
    };

    // Every failure message ends up in the app-status text. At 90 px it showed
    // about a dozen characters of "Could not start logging session".
    constexpr int appStatusReserve = 230;

    int preferredTotal = 0;
    for (const auto& spec : specs)
        preferredTotal += spec.preferred;

    const int available = juce::jmax (0, bounds.getWidth() - appStatusReserve);
    const double scale  = (available < preferredTotal && preferredTotal > 0)
                            ? (double) available / (double) preferredTotal
                            : 1.0;

    // Clamping each cell at its minimum can push the total back over the space
    // available, which previously starved the last cells and left the app-status
    // cell zero-width. Measure the clamped total first and, if it overflows,
    // shave the excess off the cells that still have slack above their minimum.
    int widths[std::size (specs)] {};
    int clampedTotal = 0;

    for (size_t i = 0; i < std::size (specs); ++i)
    {
        widths[i] = juce::jmax (specs[i].minimum, juce::roundToInt (specs[i].preferred * scale));
        clampedTotal += widths[i];
    }

    int excess = clampedTotal - available;

    while (excess > 0)
    {
        int reclaimedThisPass = 0;

        for (size_t i = 0; i < std::size (specs) && excess > 0; ++i)
        {
            if (widths[i] > specs[i].minimum)
            {
                --widths[i];
                --excess;
                ++reclaimedThisPass;
            }
        }

        if (reclaimedThisPass == 0)
            break;   // everything is at its minimum; nothing left to give
    }

    for (size_t i = 0; i < std::size (specs); ++i)
        specs[i].cell->bounds = bounds.removeFromLeft (juce::jmin (bounds.getWidth(), widths[i]));

    modeCell.bounds      = {};       // not laid out (see above)
    appStatusCell.bounds = bounds;   // whatever remains
}

} // namespace pnam

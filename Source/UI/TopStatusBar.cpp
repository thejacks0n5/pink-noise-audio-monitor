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
    channelsCell.caption   = "CH";
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
    channelsCell.value   = kEmDash;
    rateCell.value       = kEmDash;
    bufferCell.value     = kEmDash;
    appStatusCell.value  = "Idle";
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
    updateCell (appStatusCell, text, {}, true);
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
    }

    audioCell.hasLamp = true;
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
    updateCell (loggingCell, text, lamp, true);
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
    juce::String modeText = (numOutputChannels <= 1 && numInputChannels <= 1) ? "mono" : "stereo";
    juce::String text = juce::String (numOutputChannels) + " out / " + juce::String (numInputChannels)
                       + " in (" + modeText + ")";
    updateCell (channelsCell, text, {}, true);
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
                      &outDeviceCell, &inDeviceCell, &channelsCell, &rateCell, &bufferCell,
                      &appStatusCell };

    for (auto* cellPtr : cells)
    {
        auto& cell = *cellPtr;
        if (cell.bounds.isEmpty())
            continue;

        auto area = cell.bounds.reduced (6, 4);
        auto captionArea = area.removeFromTop (12);

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

        g.setColour (Palette::get (Palette::textPrimary));
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawFittedText (cell.value, area, juce::Justification::centredLeft, 1);
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

    // Fixed-width cells left-to-right, appStatus takes the remaining space.
    audioCell.bounds     = bounds.removeFromLeft (110);
    loggingCell.bounds   = bounds.removeFromLeft (110);
    durationCell.bounds  = bounds.removeFromLeft (100);
    modeCell.bounds      = bounds.removeFromLeft (160);
    outDeviceCell.bounds = bounds.removeFromLeft (140);
    inDeviceCell.bounds  = bounds.removeFromLeft (140);
    channelsCell.bounds  = bounds.removeFromLeft (150);
    rateCell.bounds      = bounds.removeFromLeft (90);
    bufferCell.bounds    = bounds.removeFromLeft (130);
    appStatusCell.bounds = bounds;
}

} // namespace pnam

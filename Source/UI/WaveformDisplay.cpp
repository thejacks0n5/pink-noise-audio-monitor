/*
  ==============================================================================
    WaveformDisplay.cpp
    Pink Noise Audio Monitor (pnam)
  ==============================================================================
*/

#include "WaveformDisplay.h"

#include <cmath>

namespace pnam
{

//==============================================================================
WaveformDisplay::WaveformDisplay()
{
    rings[(int) Trace::inputR].visible = false; // hidden until stereo confirmed
    setWantsKeyboardFocus (false);
    rebuildRings();
}

WaveformDisplay::~WaveformDisplay() = default;

//==============================================================================
void WaveformDisplay::setSampleRate (double newSampleRate)
{
    if (newSampleRate > 0.0 && ! juce::approximatelyEqual (sampleRate, newSampleRate))
    {
        sampleRate = newSampleRate;
        rebuildRings();
    }
}

void WaveformDisplay::setTimeWindowSeconds (double seconds)
{
    const double clamped = juce::jlimit (0.1, 10.0, seconds);
    if (! juce::approximatelyEqual (clamped, timeWindowSeconds))
    {
        timeWindowSeconds = clamped;
        rebuildRings();
    }
}

void WaveformDisplay::setAmplitudeScale (AmplitudeScale scale)
{
    if (amplitudeScale != scale)
    {
        amplitudeScale = scale;
        dirty = true;
    }
}

void WaveformDisplay::setDbFloor (float newFloorDb)
{
    dbFloor = juce::jmin (-1.0f, newFloorDb);
    dirty = true;
}

//==============================================================================
void WaveformDisplay::setTraceVisible (Trace trace, bool shouldBeVisible)
{
    auto& ring = rings[(int) trace];
    if (ring.visible != shouldBeVisible)
    {
        ring.visible = shouldBeVisible;
        dirty = true;
    }
}

bool WaveformDisplay::isTraceVisible (Trace trace) const noexcept
{
    return rings[(int) trace].visible;
}

void WaveformDisplay::setNumInputChannels (int numChannels)
{
    setTraceVisible (Trace::inputR, numChannels >= 2);
}

//==============================================================================
void WaveformDisplay::pushSamples (Trace trace, const float* samples, int numSamplesIn)
{
    if (frozen || samples == nullptr || numSamplesIn <= 0)
        return;

    auto& ring = rings[(int) trace];
    if (ring.columns.empty())
        return;

    const int perColumn = juce::jmax (1, samplesPerColumn);

    for (int i = 0; i < numSamplesIn; ++i)
    {
        const float s = samples[i];

        if (ring.accumCount == 0)
        {
            ring.accumMin = s;
            ring.accumMax = s;
        }
        else
        {
            ring.accumMin = juce::jmin (ring.accumMin, s);
            ring.accumMax = juce::jmax (ring.accumMax, s);
        }

        ++ring.accumCount;

        if (ring.accumCount >= perColumn)
        {
            ring.columns[(size_t) ring.writeIndex] = { ring.accumMin, ring.accumMax };
            ring.writeIndex = (ring.writeIndex + 1) % (int) ring.columns.size();
            ring.columnsFilled = juce::jmin ((int) ring.columns.size(), ring.columnsFilled + 1);

            ring.accumMin = 0.0f;
            ring.accumMax = 0.0f;
            ring.accumCount = 0;
        }
    }

    dirty = true;
}

void WaveformDisplay::clear()
{
    for (auto& ring : rings)
    {
        std::fill (ring.columns.begin(), ring.columns.end(), MinMax { 0.0f, 0.0f });
        ring.writeIndex = 0;
        ring.columnsFilled = 0;
        ring.accumMin = ring.accumMax = 0.0f;
        ring.accumCount = 0;
    }

    dirty = true;
}

//==============================================================================
void WaveformDisplay::setFrozen (bool shouldFreeze)
{
    if (frozen != shouldFreeze)
    {
        frozen = shouldFreeze;

        if (onFreezeChanged != nullptr)
            onFreezeChanged (frozen);

        dirty = true;
    }
}

//==============================================================================
void WaveformDisplay::refresh()
{
    if (dirty)
    {
        repaint();
        dirty = false;
    }
}

//==============================================================================
void WaveformDisplay::rebuildRings()
{
    const int width = juce::jmax (1, getWidth());
    const double totalSamples = timeWindowSeconds * sampleRate;
    samplesPerColumn = juce::jmax (1, (int) (totalSamples / (double) width));

    for (auto& ring : rings)
    {
        ring.columns.assign ((size_t) width, MinMax { 0.0f, 0.0f });
        ring.writeIndex = 0;
        ring.columnsFilled = 0;
        ring.accumMin = ring.accumMax = 0.0f;
        ring.accumCount = 0;
    }

    dirty = true;
}

juce::Colour WaveformDisplay::colourForTrace (Trace trace) const noexcept
{
    switch (trace)
    {
        case Trace::reference: return Palette::get (Palette::traceReference);
        case Trace::inputL:    return Palette::get (Palette::traceInputL);
        case Trace::inputR:    return Palette::get (Palette::traceInputR);
        default:               return Palette::get (Palette::traceReference);
    }
}

//==============================================================================
void WaveformDisplay::paint (juce::Graphics& g)
{
    g.fillAll (Palette::get (Palette::windowBackground));

    auto bounds = getLocalBounds();
    const int w = bounds.getWidth();
    const int h = bounds.getHeight();
    const float centreY = (float) h * 0.5f;

    // Grid
    g.setColour (Palette::get (Palette::traceGrid));
    for (int i = 1; i < 4; ++i)
    {
        const float y = (float) h * (float) i / 4.0f;
        g.drawHorizontalLine ((int) y, 0.0f, (float) w);
    }

    // Centre line
    g.setColour (Palette::get (Palette::traceCentreLine));
    g.drawHorizontalLine ((int) centreY, 0.0f, (float) w);

    for (int t = 0; t < numTraces; ++t)
    {
        const auto& ring = rings[(size_t) t];
        if (! ring.visible || ring.columns.empty())
            continue;

        const int numCols = (int) ring.columns.size();
        const int filled = ring.columnsFilled;
        if (filled <= 0)
            continue;

        juce::Path minPath, maxPath;
        bool started = false;

        for (int i = 0; i < filled; ++i)
        {
            // Oldest-to-newest ordering across the ring, right-aligned so the
            // newest column sits at the right edge of the component.
            const int idx = (ring.writeIndex - filled + i % numCols + numCols) % numCols;
            const int x = w - filled + i;
            if (x < 0 || x >= w)
                continue;

            const MinMax& mm = ring.columns[(size_t) idx];

            float yMin, yMax;

            if (amplitudeScale == AmplitudeScale::linear)
            {
                yMin = centreY - mm.mn * centreY;
                yMax = centreY - mm.mx * centreY;
            }
            else
            {
                auto dbFor = [this] (float sample)
                {
                    const float mag = std::abs (sample);
                    const float db = mag > 0.0f ? 20.0f * std::log10 (mag) : dbFloor;
                    return juce::jlimit (dbFloor, 0.0f, db);
                };

                auto propFor = [this] (float db)
                {
                    return (db - dbFloor) / (0.0f - dbFloor);
                };

                const float propMin = propFor (dbFor (mm.mn));
                const float propMax = propFor (dbFor (mm.mx));

                const float signMin = mm.mn < 0.0f ? -1.0f : 1.0f;
                const float signMax = mm.mx < 0.0f ? -1.0f : 1.0f;

                yMin = centreY - signMin * propMin * centreY;
                yMax = centreY - signMax * propMax * centreY;
            }

            if (! started)
            {
                maxPath.startNewSubPath ((float) x, yMax);
                minPath.startNewSubPath ((float) x, yMin);
                started = true;
            }
            else
            {
                maxPath.lineTo ((float) x, yMax);
                minPath.lineTo ((float) x, yMin);
            }
        }

        if (started)
        {
            g.setColour (colourForTrace ((Trace) t));
            g.strokePath (maxPath, juce::PathStrokeType (1.0f));
            g.strokePath (minPath, juce::PathStrokeType (1.0f));
        }
    }

    if (frozen)
    {
        g.setColour (Palette::get (Palette::severityWarning).withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawFittedText ("FROZEN", bounds.removeFromTop (18).removeFromRight (70),
                          juce::Justification::centred, 1);
    }
}

void WaveformDisplay::resized()
{
    rebuildRings();
}

void WaveformDisplay::mouseDown (const juce::MouseEvent&)
{
    setFrozen (! frozen);
}

} // namespace pnam

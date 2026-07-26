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
    setWantsKeyboardFocus (false);

    // Sensible default: lane 0 watches the first output (the reference), lane 1
    // the first input, so a plain loopback shows send vs return out of the box.
    laneSources[0] = { false, 0, true };
    laneSources[1] = { true,  0, true };

    // Lane count needs a UI entry point; without one setNumLanes() was dead code
    // and the display was permanently stuck at two lanes.
    for (int n : laneCountOptions)
        laneCountSelector.addItem (juce::String (n) + (n == 1 ? " lane" : " lanes"), n);

    laneCountSelector.setSelectedId (numLanes, juce::dontSendNotification);
    laneCountSelector.onChange = [this]
    {
        setNumLanes (laneCountSelector.getSelectedId());

        if (onLaneConfigChanged != nullptr)
            onLaneConfigChanged();
    };
    addAndMakeVisible (laneCountSelector);

    rebuildLaneSelectors();
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
void WaveformDisplay::setNumLanes (int newNumLanes)
{
    newNumLanes = juce::jlimit (1, maxLanes, newNumLanes);

    if (numLanes == newNumLanes)
        return;

    numLanes = newNumLanes;
    rebuildLaneSelectors();
    rebuildRings();
    resized();
    dirty = true;
    repaint();
}

void WaveformDisplay::setAvailableChannels (const juce::StringArray& outputNames,
                                            const juce::StringArray& inputNames)
{
    if (availableOutputNames == outputNames && availableInputNames == inputNames)
        return;

    availableOutputNames = outputNames;
    availableInputNames  = inputNames;

    // Drop any lane pointing at a channel the new device no longer has.
    for (auto& src : laneSources)
    {
        const int limit = src.isInput ? availableInputNames.size() : availableOutputNames.size();
        if (src.active && src.channel >= limit)
            src.active = false;
    }

    rebuildLaneSelectors();
    dirty = true;
}

WaveformDisplay::LaneSource WaveformDisplay::getLaneSource (int lane) const noexcept
{
    if (! juce::isPositiveAndBelow (lane, maxLanes))
        return {};

    return laneSources[lane];
}

void WaveformDisplay::setLaneSource (int lane, LaneSource source)
{
    if (! juce::isPositiveAndBelow (lane, maxLanes))
        return;

    laneSources[lane] = source;

    auto& ring = rings[lane];
    std::fill (ring.columns.begin(), ring.columns.end(), MinMax { 0.0f, 0.0f });
    ring.writeIndex = ring.columnsFilled = ring.accumCount = 0;

    rebuildLaneSelectors();   // reflects the new selection in the combo
    dirty = true;
}

void WaveformDisplay::rebuildLaneSelectors()
{
    laneSelectors.clear();

    for (int lane = 0; lane < numLanes; ++lane)
    {
        auto* box = laneSelectors.add (new juce::ComboBox());

        // IDs: 1 = Off, 2..(1+outs) = outputs, then inputs.
        box->addItem ("Off", 1);

        for (int ch = 0; ch < availableOutputNames.size(); ++ch)
            box->addItem (availableOutputNames[ch], 2 + ch);

        for (int ch = 0; ch < availableInputNames.size(); ++ch)
            box->addItem (availableInputNames[ch], 2 + availableOutputNames.size() + ch);

        const auto& src = laneSources[lane];
        int id = 1;
        if (src.active)
            id = src.isInput ? 2 + availableOutputNames.size() + src.channel
                             : 2 + src.channel;

        box->setSelectedId (id, juce::dontSendNotification);

        box->onChange = [this, lane, box]
        {
            const int chosen = box->getSelectedId();
            auto& target = laneSources[lane];

            if (chosen <= 1)
            {
                target.active = false;
            }
            else
            {
                const int index = chosen - 2;
                target.isInput = index >= availableOutputNames.size();
                target.channel = target.isInput ? index - availableOutputNames.size() : index;
                target.active  = true;
            }

            // Old content belongs to a different signal — don't let it linger.
            auto& ring = rings[lane];
            std::fill (ring.columns.begin(), ring.columns.end(), MinMax { 0.0f, 0.0f });
            ring.writeIndex = ring.columnsFilled = ring.accumCount = 0;

            dirty = true;
            repaint();

            if (onLaneConfigChanged != nullptr)
                onLaneConfigChanged();
        };

        addAndMakeVisible (box);
    }

    resized();
}

//==============================================================================
void WaveformDisplay::pushSamples (int lane, const float* samples, int numSamplesIn)
{
    if (frozen || samples == nullptr || numSamplesIn <= 0
        || ! juce::isPositiveAndBelow (lane, maxLanes))
        return;

    auto& ring = rings[lane];
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

juce::Colour WaveformDisplay::colourForLane (int lane) const noexcept
{
    // Outputs keep the reference colour family, inputs the input family, so a
    // send and its return stay visually distinct however lanes are arranged.
    const auto& src = laneSources[juce::jlimit (0, maxLanes - 1, lane)];

    const auto base = src.isInput ? Palette::get (Palette::traceInputL)
                                  : Palette::get (Palette::traceReference);

    // Shift the hue slightly per channel so several inputs remain tellable apart.
    return base.withRotatedHue (0.045f * (float) src.channel);
}

juce::Rectangle<int> WaveformDisplay::laneBounds (int lane) const
{
    auto bounds = getLocalBounds();
    const int lanes      = juce::jmax (1, numLanes);
    const int laneHeight = juce::jmax (1, bounds.getHeight() / lanes);

    // Give the last lane the remainder so integer division leaves no dead strip.
    const int height = (lane == lanes - 1) ? bounds.getHeight() - laneHeight * (lanes - 1)
                                           : laneHeight;

    return { bounds.getX(), bounds.getY() + lane * laneHeight, bounds.getWidth(), height };
}

juce::Rectangle<int> WaveformDisplay::laneTraceBounds (int lane) const
{
    // The selector sits in a reserved top strip rather than on top of the trace.
    return laneBounds (lane).withTrimmedTop (24);
}

//==============================================================================
void WaveformDisplay::paint (juce::Graphics& g)
{
    g.fillAll (Palette::get (Palette::windowBackground));

    const int w = getWidth();

    for (int lane = 0; lane < numLanes; ++lane)
    {
        const auto band  = laneBounds (lane);
        const auto trace = laneTraceBounds (lane);
        const float centreY = (float) trace.getCentreY();

        // Lane separator
        if (lane > 0)
        {
            g.setColour (Palette::get (Palette::outline));
            g.drawHorizontalLine (band.getY(), 0.0f, (float) w);
        }

        g.setColour (Palette::get (Palette::traceCentreLine));
        g.drawHorizontalLine ((int) centreY, 0.0f, (float) w);

        const auto& src = laneSources[lane];

        if (! src.active)
        {
            g.setColour (Palette::get (Palette::textSecondary).withAlpha (0.5f));
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawFittedText ("(no channel selected)", trace, juce::Justification::centred, 1);
            continue;
        }

        const auto& ring = rings[lane];
        if (ring.columns.empty() || ring.columnsFilled <= 0)
            continue;

        const int numCols = (int) ring.columns.size();
        const int filled  = ring.columnsFilled;

        // Half-height of this lane's trace, leaving room for the selector.
        const float halfHeight = (float) trace.getHeight() * 0.5f - 2.0f;
        if (halfHeight <= 1.0f)
            continue;

        juce::Path minPath, maxPath;
        bool started = false;

        for (int i = 0; i < filled; ++i)
        {
            const int idx = (ring.writeIndex - filled + i % numCols + numCols) % numCols;
            const int x = w - filled + i;
            if (x < 0 || x >= w)
                continue;

            const MinMax& mm = ring.columns[(size_t) idx];

            float yMin, yMax;

            if (amplitudeScale == AmplitudeScale::linear)
            {
                yMin = centreY - mm.mn * halfHeight;
                yMax = centreY - mm.mx * halfHeight;
            }
            else
            {
                auto dbFor = [this] (float sample)
                {
                    const float mag = std::abs (sample);
                    const float db = mag > 0.0f ? 20.0f * std::log10 (mag) : dbFloor;
                    return juce::jlimit (dbFloor, 0.0f, db);
                };

                auto propFor = [this] (float db) { return (db - dbFloor) / (0.0f - dbFloor); };

                const float propMin = propFor (dbFor (mm.mn));
                const float propMax = propFor (dbFor (mm.mx));

                yMin = centreY - (mm.mn < 0.0f ? -1.0f : 1.0f) * propMin * halfHeight;
                yMax = centreY - (mm.mx < 0.0f ? -1.0f : 1.0f) * propMax * halfHeight;
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
            g.setColour (colourForLane (lane));
            g.strokePath (maxPath, juce::PathStrokeType (1.0f));
            g.strokePath (minPath, juce::PathStrokeType (1.0f));
        }
    }

    if (frozen)
    {
        g.setColour (Palette::get (Palette::severityWarning).withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawFittedText ("FROZEN", getLocalBounds().removeFromBottom (18).removeFromRight (70),
                          juce::Justification::centred, 1);
    }
}

void WaveformDisplay::resized()
{
    // Each lane's channel selector sits in its own band's reserved top strip, so
    // the dropdown is unambiguously attached to the trace it controls.
    for (int lane = 0; lane < laneSelectors.size(); ++lane)
    {
        const auto band = laneBounds (lane);
        laneSelectors[lane]->setBounds (band.getX() + 6, band.getY() + 3,
                                        juce::jmin (150, juce::jmax (90, band.getWidth() / 5)), 18);
    }

    laneCountSelector.setBounds (juce::jmax (0, getWidth() - 96), 3, 90, 18);

    // Ring capacity is a function of width alone. Rebuilding on every resize
    // wiped all history whenever a lane was added or a name was typed.
    if (getWidth() != lastRingWidth)
    {
        lastRingWidth = getWidth();
        rebuildRings();
    }
}

void WaveformDisplay::mouseDown (const juce::MouseEvent& e)
{
    // Clicks on a lane's selector belong to the selector, not to freeze.
    if (e.eventComponent != this)
        return;

    setFrozen (! frozen);
}

} // namespace pnam

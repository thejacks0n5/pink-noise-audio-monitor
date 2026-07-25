/*
  ==============================================================================
    LevelMeterComponent.cpp
    Pink Noise Audio Monitor (pnam)
  ==============================================================================
*/

#include "LevelMeterComponent.h"

namespace pnam
{

namespace
{
    constexpr float dbTicks[] = { 0.0f, -3.0f, -6.0f, -12.0f, -20.0f, -30.0f, -40.0f, -50.0f, -60.0f };
}

//==============================================================================
LevelMeterComponent::LevelMeterComponent (Role roleIn, const juce::String& titleIn)
    : role (roleIn), title (titleIn)
{
    numChannels = (role == Role::reference) ? 1 : 2;
    setInterceptsMouseClicks (true, false);
}

LevelMeterComponent::~LevelMeterComponent() = default;

//==============================================================================
void LevelMeterComponent::setSource (AnalysisProcessor* source) noexcept
{
    analysis = source;
}

void LevelMeterComponent::setNumChannels (int newNumChannels)
{
    if (role == Role::reference)
        newNumChannels = 1;

    newNumChannels = juce::jlimit (1, 2, newNumChannels);

    if (numChannels != newNumChannels)
    {
        numChannels = newNumChannels;
        resized();
        repaint();
    }
}

//==============================================================================
void LevelMeterComponent::refresh()
{
    if (analysis == nullptr)
        return;

    const auto now = juce::Time::getMillisecondCounter();
    float dtSeconds = 1.0f / 30.0f;
    if (lastRefreshMs != 0)
        dtSeconds = juce::jlimit (0.0f, 0.5f, (float) (now - lastRefreshMs) / 1000.0f);
    lastRefreshMs = now;

    const float releaseStep = defaultReleaseDbPerSecond * dtSeconds;

    float peakDb[2]     { silenceFloorDb, silenceFloorDb };
    float rmsDb[2]      { silenceFloorDb, silenceFloorDb };
    float holdDb[2]     { silenceFloorDb, silenceFloorDb };

    if (role == Role::reference)
    {
        peakDb[0] = peakDb[1] = analysis->getRefPeakDb();
        rmsDb[0]  = rmsDb[1]  = analysis->getRefRmsDb();
        holdDb[0] = holdDb[1] = analysis->getRefPeakHoldDb();
    }
    else
    {
        peakDb[0] = analysis->getInPeakDb (0);
        peakDb[1] = analysis->getInPeakDb (1);
        rmsDb[0]  = analysis->getInRmsDb (0);
        rmsDb[1]  = analysis->getInRmsDb (1);
        holdDb[0] = analysis->getInPeakHoldDb (0);
        holdDb[1] = analysis->getInPeakHoldDb (1);
    }

    bool changed = false;

    for (int ch = 0; ch < 2; ++ch)
    {
        const float prevPeak = shownPeakDb[ch];
        const float prevRms  = shownRmsDb[ch];

        // Instant attack, ballistic release for the displayed bar.
        if (peakDb[ch] > shownPeakDb[ch])
            shownPeakDb[ch] = peakDb[ch];
        else
            shownPeakDb[ch] = juce::jmax (peakDb[ch], shownPeakDb[ch] - releaseStep);

        if (rmsDb[ch] > shownRmsDb[ch])
            shownRmsDb[ch] = rmsDb[ch];
        else
            shownRmsDb[ch] = juce::jmax (rmsDb[ch], shownRmsDb[ch] - releaseStep);

        if (! juce::approximatelyEqual (shownHoldDb[ch], holdDb[ch]))
        {
            shownHoldDb[ch] = holdDb[ch];
            changed = true;
        }

        if (! juce::approximatelyEqual (prevPeak, shownPeakDb[ch])
            || ! juce::approximatelyEqual (prevRms, shownRmsDb[ch]))
            changed = true;
    }

    if (changed)
        repaint();
}

void LevelMeterComponent::updateValues (float peakDbL, float rmsDbL,
                                        float peakDbR, float rmsDbR,
                                        float peakHoldDbL, float peakHoldDbR)
{
    shownPeakDb[0] = peakDbL;
    shownRmsDb[0]  = rmsDbL;
    shownHoldDb[0] = peakHoldDbL;

    shownPeakDb[1] = peakDbR;
    shownRmsDb[1]  = rmsDbR;
    shownHoldDb[1] = peakHoldDbR;

    repaint();
}

//==============================================================================
void LevelMeterComponent::notifyClipped (int channel)
{
    channel = juce::jlimit (0, 1, channel);
    if (! clipLit[channel])
    {
        clipLit[channel] = true;
        repaint (clipArea);
    }
}

void LevelMeterComponent::clearClipIndicator()
{
    if (clipLit[0] || clipLit[1])
    {
        clipLit[0] = clipLit[1] = false;
        repaint (clipArea);
    }
}

bool LevelMeterComponent::isClipIndicatorLit() const noexcept
{
    return clipLit[0] || clipLit[1];
}

//==============================================================================
void LevelMeterComponent::setRangeDb (float newMinDb, float newMaxDb)
{
    if (newMaxDb <= newMinDb)
        return;

    minDb = newMinDb;
    maxDb = newMaxDb;
    repaint();
}

void LevelMeterComponent::setReadoutVisible (bool shouldShow)
{
    if (readoutVisible != shouldShow)
    {
        readoutVisible = shouldShow;
        resized();
        repaint();
    }
}

float LevelMeterComponent::proportionForDb (float db) const noexcept
{
    return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
}

//==============================================================================
void LevelMeterComponent::paint (juce::Graphics& g)
{
    g.fillAll (Palette::get (Palette::panelBackground));

    // Title
    g.setColour (Palette::get (Palette::textSecondary));
    g.setFont (juce::Font (juce::FontOptions (13.0f)));
    g.drawFittedText (title, titleArea, juce::Justification::centred, 1);

    // Clip lamps
    const int lampSize = 8;
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const int lampX = clipArea.getX() + ch * (lampSize + 6);
        juce::Rectangle<int> lamp (lampX, clipArea.getY(), lampSize, lampSize);
        g.setColour (clipLit[ch] ? Palette::get (Palette::meterRed) : Palette::get (Palette::meterTrough));
        g.fillEllipse (lamp.toFloat());
        g.setColour (Palette::get (Palette::outline));
        g.drawEllipse (lamp.toFloat(), 1.0f);
    }

    // Bars + ruler
    if (! barArea.isEmpty())
    {
        const int gap = 4;
        const int barWidth = (barArea.getWidth() - gap * (numChannels - 1) - 26) / juce::jmax (1, numChannels);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            juce::Rectangle<int> bar (barArea.getX() + ch * (barWidth + gap), barArea.getY(),
                                      barWidth, barArea.getHeight());

            g.setColour (Palette::get (Palette::meterTrough));
            g.fillRect (bar);

            const float peakProp = proportionForDb (shownPeakDb[ch]);
            const int litHeight = juce::roundToInt (peakProp * (float) bar.getHeight());

            if (litHeight > 0)
            {
                juce::Rectangle<int> litRect (bar.getX(), bar.getBottom() - litHeight, bar.getWidth(), litHeight);
                g.setColour (Palette::meterColourForDb (shownPeakDb[ch]));
                g.fillRect (litRect);
            }

            // RMS translucent overlay
            const float rmsProp = proportionForDb (shownRmsDb[ch]);
            const int rmsHeight = juce::roundToInt (rmsProp * (float) bar.getHeight());
            if (rmsHeight > 0)
            {
                juce::Rectangle<int> rmsRect (bar.getX(), bar.getBottom() - rmsHeight, bar.getWidth(), rmsHeight);
                g.setColour (Palette::get (Palette::meterRmsOverlay));
                g.fillRect (rmsRect);
            }

            // Peak-hold tick
            if (shownHoldDb[ch] > silenceFloorDb)
            {
                const float holdProp = proportionForDb (shownHoldDb[ch]);
                const int holdY = bar.getBottom() - juce::roundToInt (holdProp * (float) bar.getHeight());
                g.setColour (Palette::get (Palette::meterPeakTick));
                g.fillRect (bar.getX(), juce::jlimit (bar.getY(), bar.getBottom() - 2, holdY - 1), bar.getWidth(), 2);
            }

            g.setColour (Palette::get (Palette::outline));
            g.drawRect (bar, 1);
        }

        // dB ruler
        juce::Rectangle<int> rulerArea (barArea.getRight() - 24, barArea.getY(), 24, barArea.getHeight());
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        for (float tick : dbTicks)
        {
            if (tick < minDb || tick > maxDb)
                continue;

            const float prop = proportionForDb (tick);
            const int y = rulerArea.getBottom() - juce::roundToInt (prop * (float) rulerArea.getHeight());

            g.setColour (Palette::get (Palette::textSecondary));
            g.drawHorizontalLine (y, (float) rulerArea.getX(), (float) rulerArea.getX() + 4.0f);
            g.drawFittedText (juce::String ((int) tick), rulerArea.getX() + 6, y - 6, rulerArea.getWidth() - 6, 12,
                              juce::Justification::centredLeft, 1);
        }
    }

    // Numeric readout
    if (readoutVisible && ! readoutArea.isEmpty())
    {
        g.setFont (DarkLookAndFeel::getMonoFont (12.0f));
        g.setColour (Palette::get (Palette::textPrimary));

        juce::String text;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (ch > 0)
                text << "  ";

            if (shownPeakDb[ch] <= silenceFloorDb)
                text << "-inf";
            else
                text << juce::String (shownPeakDb[ch], 1);
        }

        g.drawFittedText (text, readoutArea, juce::Justification::centred, 1);
    }
}

void LevelMeterComponent::resized()
{
    auto bounds = getLocalBounds();

    titleArea = bounds.removeFromTop (16);
    clipArea  = bounds.removeFromTop (10);

    if (readoutVisible)
        readoutArea = bounds.removeFromBottom (14);
    else
        readoutArea = {};

    barArea = bounds;
}

void LevelMeterComponent::mouseDown (const juce::MouseEvent&)
{
    clearClipIndicator();

    if (onResetRequest != nullptr)
        onResetRequest();
}

} // namespace pnam

/*
  ==============================================================================
    EventLogComponent.cpp
    Pink Noise Audio Monitor (pnam)
  ==============================================================================
*/

#include "EventLogComponent.h"

namespace pnam
{

namespace
{
    juce::String formatElapsedHms (double elapsedSeconds)
    {
        return juce::String (elapsedSeconds, 3);
    }

    juce::String formatTimestamp (juce::Time t)
    {
        if (t == juce::Time())
            return {};

        return t.formatted ("%H:%M:%S") + juce::String::formatted (".%03d",
                   (int) (t.toMilliseconds() % 1000));
    }

    juce::String formatNumber (double value)
    {
        return juce::String (value, 2);
    }
}

//==============================================================================
EventLogComponent::EventLogComponent()
{
    addAndMakeVisible (severityFilterBox);
    severityFilterBox.addItem ("All severities", 1);
    severityFilterBox.addItem ("Warning+", 2);
    severityFilterBox.addItem ("Critical only", 3);
    severityFilterBox.setSelectedId (1, juce::dontSendNotification);
    severityFilterBox.onChange = [this]
    {
        switch (severityFilterBox.getSelectedId())
        {
            case 2:  setMinimumSeverity (Severity::Warning);  break;
            case 3:  setMinimumSeverity (Severity::Critical); break;
            default: setMinimumSeverity (Severity::Info);     break;
        }
    };

    addAndMakeVisible (typeFilterBox);
    typeFilterBox.addItem ("All types", 1);
    typeFilterBox.addItem ("Complete Dropout", 2);
    typeFilterBox.addItem ("Clipping", 3);
    typeFilterBox.addItem ("Level Change", 4);
    typeFilterBox.addItem ("Manual Marker", 5);
    typeFilterBox.addItem ("Audio Device Lost", 6);
    typeFilterBox.addItem ("Logging Error", 7);
    typeFilterBox.addItem ("Other", 8);
    typeFilterBox.setSelectedId (1, juce::dontSendNotification);
    typeFilterBox.onChange = [this]
    {
        const int id = typeFilterBox.getSelectedId();
        if (id == 1)
        {
            showAllTypes();
        }
        else
        {
            for (int i = 0; i < 7; ++i)
                typeVisible[i] = false;

            typeVisible[id - 2] = true;
            rebuildFilteredIndex();
            repaint();
        }
    };

    addAndMakeVisible (autoScrollButton);
    autoScrollButton.setToggleState (autoScroll, juce::dontSendNotification);
    autoScrollButton.onClick = [this] { setAutoScrollEnabled (autoScrollButton.getToggleState()); };

    addAndMakeVisible (clearButton);
    clearButton.onClick = [this] { clear(); if (onCleared != nullptr) onCleared(); };

    addAndMakeVisible (countLabel);
    countLabel.setJustificationType (juce::Justification::centredRight);
    countLabel.setFont (juce::Font (juce::FontOptions (12.0f)));

    addAndMakeVisible (table);
    table.getHeader().addColumn ("Time",     colTimestamp,   90);
    table.getHeader().addColumn ("Elapsed",  colElapsed,     70);
    table.getHeader().addColumn ("Type",     colType,        130);
    table.getHeader().addColumn ("Severity", colSeverity,    80);
    table.getHeader().addColumn ("Ch",       colChannel,     40);
    table.getHeader().addColumn ("Measured", colMeasured,    80);
    table.getHeader().addColumn ("Expected", colExpected,    80);
    table.getHeader().addColumn ("Diff",     colDifference,  70);
    table.getHeader().addColumn ("Description", colDescription, 280);
    table.setHeaderHeight (22);
    table.setRowHeight (20);
    table.getHeader().setStretchToFitActive (true);
}

EventLogComponent::~EventLogComponent() = default;

//==============================================================================
void EventLogComponent::addEvent (const Event& event, const juce::String& description, juce::Time wallClockUtc)
{
    Row row;
    row.event = event;
    row.description = description;
    row.timestampText = formatTimestamp (wallClockUtc);

    rows.push_back (row);
    ++totalReceived;

    dropOldestIfNeeded();
    rebuildFilteredIndex();

    table.updateContent();

    if (autoScroll && ! visibleRows.empty())
        table.scrollToEnsureRowIsOnscreen ((int) visibleRows.size() - 1);

    repaint();
}

void EventLogComponent::clear()
{
    rows.clear();
    rebuildFilteredIndex();
    visibleRows.clear();
    table.updateContent();
    repaint();
}

juce::int64 EventLogComponent::getTotalEventsReceived() const noexcept
{
    return totalReceived;
}

int EventLogComponent::getNumRowsStored() const noexcept
{
    return (int) rows.size();
}

//==============================================================================
void EventLogComponent::setMinimumSeverity (Severity minimum)
{
    if (minimumSeverity != minimum)
    {
        minimumSeverity = minimum;
        rebuildFilteredIndex();
        table.updateContent();
        repaint();
    }
}

void EventLogComponent::setTypeVisible (EventType type, bool shouldShow)
{
    const int idx = (int) type;
    if (idx < 0 || idx >= 7)
        return;

    if (typeVisible[idx] != shouldShow)
    {
        typeVisible[idx] = shouldShow;
        rebuildFilteredIndex();
        table.updateContent();
        repaint();
    }
}

bool EventLogComponent::isTypeVisible (EventType type) const noexcept
{
    const int idx = (int) type;
    if (idx < 0 || idx >= 7)
        return true;
    return typeVisible[idx];
}

void EventLogComponent::showAllTypes()
{
    for (auto& v : typeVisible)
        v = true;

    rebuildFilteredIndex();
    table.updateContent();
    repaint();
}

//==============================================================================
void EventLogComponent::setAutoScrollEnabled (bool shouldAutoScroll)
{
    autoScroll = shouldAutoScroll;
    autoScrollButton.setToggleState (autoScroll, juce::dontSendNotification);

    if (autoScroll && ! visibleRows.empty())
        table.scrollToEnsureRowIsOnscreen ((int) visibleRows.size() - 1);
}

void EventLogComponent::setMaxStoredEvents (int newMax)
{
    maxStoredEvents = juce::jmax (1, newMax);
    dropOldestIfNeeded();
    rebuildFilteredIndex();
    table.updateContent();
}

//==============================================================================
void EventLogComponent::rebuildFilteredIndex()
{
    visibleRows.clear();
    visibleRows.reserve (rows.size());

    for (int i = 0; i < (int) rows.size(); ++i)
        if (passesFilter (rows[(size_t) i]))
            visibleRows.push_back (i);

    countLabel.setText (juce::String (visibleRows.size()) + " / " + juce::String (rows.size()) + " events",
                        juce::dontSendNotification);
}

bool EventLogComponent::passesFilter (const Row& row) const noexcept
{
    if ((int) row.event.severity < (int) minimumSeverity)
        return false;

    const int idx = (int) row.event.type;
    if (idx >= 0 && idx < numEventTypes && ! typeVisible[idx])
        return false;

    return true;
}

void EventLogComponent::dropOldestIfNeeded()
{
    if ((int) rows.size() > maxStoredEvents)
    {
        const size_t numToDrop = rows.size() - (size_t) maxStoredEvents;
        rows.erase (rows.begin(), rows.begin() + (long) numToDrop);
    }
}

//==============================================================================
int EventLogComponent::getNumRows()
{
    return (int) visibleRows.size();
}

void EventLogComponent::paintRowBackground (juce::Graphics& g, int rowNumber, int width, int height,
                                            bool rowIsSelected)
{
    if (rowIsSelected)
    {
        g.fillAll (Palette::get (Palette::controlHover));
        return;
    }

    if (rowNumber < 0 || rowNumber >= (int) visibleRows.size())
    {
        g.fillAll (Palette::get (Palette::panelBackground));
        return;
    }

    const auto& row = rows[(size_t) visibleRows[(size_t) rowNumber]];

    juce::Colour bg = (rowNumber % 2 == 0) ? Palette::get (Palette::panelBackground)
                                            : Palette::get (Palette::panelBackgroundAlt);

    if (row.event.severity == Severity::Critical)
        bg = bg.overlaidWith (Palette::get (Palette::severityCritical).withAlpha (0.10f));

    g.fillAll (bg);
    juce::ignoreUnused (width, height);
}

void EventLogComponent::paintCell (juce::Graphics& g, int rowNumber, int columnId, int width, int height,
                                   bool /*rowIsSelected*/)
{
    if (rowNumber < 0 || rowNumber >= (int) visibleRows.size())
        return;

    const auto& row = rows[(size_t) visibleRows[(size_t) rowNumber]];
    const auto& event = row.event;

    // Severity stripe on the left edge
    if (columnId == colTimestamp)
    {
        g.setColour (Palette::severityColour ((int) event.severity));
        g.fillRect (0, 0, 3, height);
    }

    juce::String text;
    juce::Justification justification = juce::Justification::centredLeft;
    bool useMono = false;

    switch (columnId)
    {
        case colTimestamp:  text = row.timestampText; useMono = true; break;
        case colElapsed:    text = formatElapsedHms (event.elapsedSeconds); useMono = true;
                            justification = juce::Justification::centredRight; break;
        case colType:       text = toString (event.type); break;
        case colSeverity:   text = toString (event.severity); break;
        case colChannel:    // channelIndex is authoritative once there are more than
                            // two channels; `channel` reads "Both" for every per-channel
                            // fault and cannot say which input dropped out.
                            text = event.channelIndex >= 0
                                     ? (event.channelIsInput ? "In " : "Out ") + juce::String (event.channelIndex + 1)
                                     : juce::String (toString (event.channel));
                            justification = juce::Justification::centred; break;
        case colMeasured:   text = formatNumber (event.measuredValue); useMono = true;
                            justification = juce::Justification::centredRight; break;
        case colExpected:   text = formatNumber (event.expectedValue); useMono = true;
                            justification = juce::Justification::centredRight; break;
        case colDifference: text = formatNumber (event.difference); useMono = true;
                            justification = juce::Justification::centredRight; break;
        case colDescription: text = row.description; break;
        default: break;
    }

    g.setColour (columnId == colSeverity ? Palette::severityColour ((int) event.severity)
                                          : Palette::get (Palette::textPrimary));
    g.setFont (useMono ? DarkLookAndFeel::getMonoFont (12.0f) : juce::Font (juce::FontOptions (12.0f)));

    const int leftPad = (columnId == colTimestamp) ? 6 : 4;
    g.drawFittedText (text, leftPad, 0, width - leftPad - 4, height, justification, 1);
}

juce::String EventLogComponent::getCellTooltip (int rowNumber, int columnId)
{
    if (rowNumber < 0 || rowNumber >= (int) visibleRows.size())
        return {};

    if (columnId == colDescription)
        return rows[(size_t) visibleRows[(size_t) rowNumber]].description;

    return {};
}

//==============================================================================
void EventLogComponent::resized()
{
    auto bounds = getLocalBounds();
    auto toolbar = bounds.removeFromTop (24);

    toolbar.reduce (4, 2);

    severityFilterBox.setBounds (toolbar.removeFromLeft (120));
    toolbar.removeFromLeft (4);
    typeFilterBox.setBounds (toolbar.removeFromLeft (140));
    toolbar.removeFromLeft (4);
    autoScrollButton.setBounds (toolbar.removeFromLeft (70));
    toolbar.removeFromLeft (4);
    clearButton.setBounds (toolbar.removeFromLeft (60));

    countLabel.setBounds (toolbar);

    table.setBounds (bounds);
}

void EventLogComponent::paint (juce::Graphics& g)
{
    g.fillAll (Palette::get (Palette::panelBackground));
}

} // namespace pnam

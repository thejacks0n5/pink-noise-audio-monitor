/*
  ==============================================================================
    EventLogComponent.cpp
    Pink Noise Audio Monitor (pnam)
  ==============================================================================
*/

#include "EventLogComponent.h"

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace pnam
{

namespace
{
    juce::String formatElapsedHms (double elapsedSeconds)
    {
        return juce::String (elapsedSeconds, 3);
    }

    /** "CompleteDropout" -> "Complete Dropout", for the type filter. */
    juce::String spacedTypeName (EventType type)
    {
        const juce::String name (toString (type));
        juce::String spaced;

        for (int i = 0; i < name.length(); ++i)
        {
            if (i > 0 && juce::CharacterFunctions::isUpperCase (name[i]))
                spaced << ' ';

            spaced << name[i];
        }

        return spaced;
    }

    /** Tiny CallOutBox content: one TextEditor for naming a marker. */
    class MarkerNameEditor : public juce::Component
    {
    public:
        MarkerNameEditor (const juce::String& initialText,
                          std::function<void (const juce::String&)> commitFn)
            : initial (initialText), onCommit (std::move (commitFn))
        {
            editor.setText (initialText, juce::dontSendNotification);
            editor.setSelectAllWhenFocused (true);

            editor.onReturnKey = [this]
            {
                commit();
                dismiss();
            };

            // Escape is the only way to throw the text away.
            editor.onEscapeKey = [this]
            {
                finished = true;
                dismiss();
            };

            // Clicking away keeps what was typed: discarding it lost notes with
            // no sign that anything had gone.
            editor.onFocusLost = [this]
            {
                commit();
                dismiss();
            };

            addAndMakeVisible (editor);
            setSize (260, 30);
        }

        void resized() override            { editor.setBounds (getLocalBounds().reduced (3)); }
        void parentHierarchyChanged() override
        {
            if (isShowing())
                editor.grabKeyboardFocus();
        }

    private:
        /** At most once: dismissing after Return takes the focus away, which
            would otherwise commit the same text a second time. */
        void commit()
        {
            if (finished)
                return;

            finished = true;

            const auto text = editor.getText().trim();

            if (onCommit != nullptr && text.isNotEmpty() && text != initial)
                onCommit (text);
        }

        void dismiss()
        {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                box->dismiss();
        }

        juce::TextEditor editor;
        juce::String initial;
        std::function<void (const juce::String&)> onCommit;
        bool finished = false;
    };

    juce::String formatTimestamp (juce::Time t)
    {
        if (t == juce::Time())
            return {};

       #if JUCE_MAC
        // Format with the user's own clock conventions. The ICU "j" skeleton is
        // the documented hook for the System Settings 12/24-hour preference:
        // it resolves to "h:mm:ss.SSS a" or "HH:mm:ss.SSS" per that setting and
        // the locale — strftime("%H") would pin everyone to 24-hour.
        // Message-thread only, so a lazily built shared formatter is safe.
        static CFDateFormatterRef formatter = []() -> CFDateFormatterRef
        {
            CFLocaleRef locale = CFLocaleCopyCurrent();
            auto* f = CFDateFormatterCreate (kCFAllocatorDefault, locale,
                                             kCFDateFormatterNoStyle, kCFDateFormatterNoStyle);

            if (f != nullptr)
                if (CFStringRef pattern = CFDateFormatterCreateDateFormatFromTemplate (
                        kCFAllocatorDefault, CFSTR ("jmmssSSS"), 0, locale))
                {
                    CFDateFormatterSetFormat (f, pattern);
                    CFRelease (pattern);
                }

            CFRelease (locale);
            return f;
        }();

        if (formatter != nullptr)
        {
            const auto absTime = (CFAbsoluteTime) ((double) t.toMilliseconds() / 1000.0)
                                   - kCFAbsoluteTimeIntervalSince1970;

            if (CFStringRef str = CFDateFormatterCreateStringWithAbsoluteTime (
                    kCFAllocatorDefault, formatter, absTime))
            {
                char buffer[64] {};
                const bool ok = CFStringGetCString (str, buffer, sizeof (buffer),
                                                    kCFStringEncodingUTF8);
                CFRelease (str);

                if (ok)
                    return juce::String::fromUTF8 (buffer);
            }
        }
       #endif

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
    std::fill (std::begin (typeVisible), std::end (typeVisible), true);

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

    // One entry per EventType, taken from the enum (item id = type + 2), so a
    // type appended to the model is filterable without this list being touched.
    // A hand-written list had already fallen one short of the enum.
    for (int i = 0; i < numEventTypes; ++i)
        typeFilterBox.addItem (spacedTypeName ((EventType) i), i + 2);

    typeFilterBox.setSelectedId (1, juce::dontSendNotification);
    typeFilterBox.onChange = [this]
    {
        const int id = typeFilterBox.getSelectedId();
        if (id <= 1)
        {
            showAllTypes();
        }
        else
        {
            std::fill (std::begin (typeVisible), std::end (typeVisible), false);

            if (juce::isPositiveAndBelow (id - 2, numEventTypes))
                typeVisible[id - 2] = true;

            rebuildFilteredIndex();
            table.updateContent();
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
    table.getHeader().addColumn ("Time",     colTimestamp,   110);
    table.getHeader().addColumn ("Elapsed",  colElapsed,     70);
    table.getHeader().addColumn ("Type",     colType,        130);
    table.getHeader().addColumn ("Severity", colSeverity,    80);
    table.getHeader().addColumn ("Ch",       colChannel,     40);
    table.getHeader().addColumn ("Measured", colMeasured,    80);
    table.getHeader().addColumn ("Expected", colExpected,    80);
    table.getHeader().addColumn ("Diff",     colDifference,  70);
    table.getHeader().addColumn ("Description", colDescription, 280);
    table.getHeader().addColumn ("Note",     colNote,        160);
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

void EventLogComponent::cellDoubleClicked (int rowNumber, int columnId, const juce::MouseEvent&)
{
    if (! juce::isPositiveAndBelow (rowNumber, (int) visibleRows.size()))
        return;

    const auto storageIndex = (size_t) visibleRows[(size_t) rowNumber];
    if (storageIndex >= rows.size())
        return;

    const auto& row = rows[storageIndex];

    // Identify the event by value: rows can shift (oldest dropped) while the
    // editor is open, so an index captured now could point at the wrong row by
    // the time the user hits return. Matching needs channel + code too — three
    // inputs can drop out in the same block, sharing elapsed time and type.
    const Event eventCopy = row.event;

    const auto matches = [eventCopy] (const Event& e)
    {
        return e.type == eventCopy.type
            && e.channelIndex == eventCopy.channelIndex
            && e.code == eventCopy.code
            && juce::approximatelyEqual (e.elapsedSeconds, eventCopy.elapsedSeconds);
    };

    // Double-clicking a marker's Description renames the marker itself; any
    // other double-click, on any event, edits its user note ("I was swapping
    // the cable at this point").
    const bool renamingMarker = row.event.type == EventType::ManualMarker
                              && columnId == colDescription;

    // SafePointer, not `this`: the edit is now also committed when the editor
    // loses focus, which can happen as late as the callout being torn down.
    juce::Component::SafePointer<EventLogComponent> safeThis (this);

    auto editor = std::make_unique<MarkerNameEditor> (
        renamingMarker ? row.description : row.note,
        [safeThis, matches, eventCopy, renamingMarker] (const juce::String& text)
        {
            if (safeThis == nullptr)
                return;

            const auto& forward = renamingMarker ? safeThis->onMarkerRenamed
                                                 : safeThis->onNoteEdited;

            const bool reachedCsv = forward != nullptr && forward (eventCopy, text);

            for (auto& r : safeThis->rows)
                if (matches (r.event))
                {
                    (renamingMarker ? r.description : r.note) = text;
                    (renamingMarker ? r.nameNotInCsv : r.noteNotInCsv) = ! reachedCsv;
                    break;
                }

            safeThis->table.repaint();
        });

    const auto rowBounds = table.getRowPosition (rowNumber, true);
    juce::CallOutBox::launchAsynchronously (std::move (editor),
                                            table.localAreaToGlobal (rowBounds),
                                            nullptr);
}

void EventLogComponent::visitEventsSince (juce::int64 firstSequence,
                                          const std::function<bool (const Event&,
                                                                    const juce::String&,
                                                                    const juce::String&)>& visitor)
{
    if (visitor == nullptr)
        return;

    // rows only ever loses entries from the front, so row i is event number
    // (totalReceived - rows.size() + i) of the run.
    const auto firstStored = totalReceived - (juce::int64) rows.size();

    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (firstStored + (juce::int64) i < firstSequence)
            continue;

        auto& row = rows[i];

        if (visitor (row.event, row.description, row.note))
            row.noteNotInCsv = row.nameNotInCsv = false;
    }

    table.repaint();
}

void EventLogComponent::setChannelNames (const juce::StringArray& outputNames,
                                         const juce::StringArray& inputNames)
{
    if (outputChannelNames != outputNames || inputChannelNames != inputNames)
    {
        outputChannelNames = outputNames;
        inputChannelNames  = inputNames;
        table.repaint();
    }
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
    if (idx < 0 || idx >= numEventTypes)
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
    if (idx < 0 || idx >= numEventTypes)
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
                            if (event.channelIndex >= 0)
                            {
                                const auto& names = event.channelIsInput ? inputChannelNames
                                                                         : outputChannelNames;
                                text = juce::isPositiveAndBelow (event.channelIndex, names.size())
                                         && names[event.channelIndex].isNotEmpty()
                                       ? names[event.channelIndex]
                                       : (event.channelIsInput ? "In " : "Out ")
                                           + juce::String (event.channelIndex + 1);
                            }
                            else
                                text = toString (event.channel);
                            justification = juce::Justification::centred; break;
        case colMeasured:   text = formatNumber (event.measuredValue); useMono = true;
                            justification = juce::Justification::centredRight; break;
        case colExpected:   text = formatNumber (event.expectedValue); useMono = true;
                            justification = juce::Justification::centredRight; break;
        case colDifference: text = formatNumber (event.difference); useMono = true;
                            justification = juce::Justification::centredRight; break;
        // An edit that could not be written to events.csv says so, in front of
        // the text so a narrow column cannot truncate the warning away.
        case colDescription: text = row.nameNotInCsv ? "[name not in CSV] " + row.description
                                                     : row.description; break;
        case colNote:        text = row.noteNotInCsv ? "[not in CSV] " + row.note
                                                     : row.note; break;
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

#pragma once
/*
  ==============================================================================
    EventLogComponent.h
    Pink Noise Audio Monitor (pnam) — live scrolling event log table.

    A juce::TableListBox-based view of every pnam::Event drained from the
    EventQueue, with a small filter toolbar (severity minimum + event-type
    dropdown + auto-scroll + clear). MESSAGE THREAD ONLY.

    Data flow: MainComponent::timerCallback drains the EventQueue, composes
    the human-readable description via SessionLogger::composeDescription(),
    logs to CSV if a session is active, and ALWAYS calls addEvent() here so
    the live window works whether or not logging is on.

    Memory bound: the log keeps at most maxStoredEvents rows (default 5000);
    older rows are dropped from the display (the CSV keeps everything).

    Row colouring: severity tints the row text / a left edge stripe using
    Palette::severityColour; Critical rows also get a faint red row wash.
  ==============================================================================
*/

#include <functional>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../Model/DataModel.h"
#include "DarkLookAndFeel.h"

namespace pnam
{

//==============================================================================
class EventLogComponent : public juce::Component,
                          private juce::TableListBoxModel
{
public:
    //==========================================================================
    /** Table columns (juce column ids must be >= 1). */
    enum ColumnId
    {
        colTimestamp = 1,   ///< wall-clock UTC, HH:MM:SS.mmm
        colElapsed,         ///< elapsed_s, mono font, 3 decimals
        colType,            ///< toString (EventType)
        colSeverity,        ///< toString (Severity), tinted
        colChannel,         ///< toString (ChannelId)
        colMeasured,        ///< measuredValue (duration s for end events)
        colExpected,        ///< expectedValue
        colDifference,      ///< difference
        colDescription      ///< composed description text (stretches)
    };

    static constexpr int defaultMaxStoredEvents = 5000;

    //==========================================================================
    EventLogComponent();
    ~EventLogComponent() override;

    //==========================================================================
    // Feeding (message thread)
    /** Appends one event row.
        @param event        the drained POD event.
        @param description  text from SessionLogger::composeDescription().
        @param wallClockUtc absolute time (sessionStartUtc + elapsedSeconds);
                            pass juce::Time() to display elapsed only. */
    void addEvent (const Event& event,
                   const juce::String& description,
                   juce::Time wallClockUtc);

    /** Removes all rows (Reset Session / user pressed Clear). */
    void clear();

    /** Fired when the user renames a manual marker (double-click its row). The
        Event identifies the marker (elapsedSeconds + type); MainComponent
        forwards the new name into the session CSV. MESSAGE THREAD. */
    std::function<void (const Event&, const juce::String& newName)> onMarkerRenamed;

    /** User channel names, used to render the Ch column so it matches the CSV.
        Index = channel index; missing entries fall back to "In n"/"Out n". */
    void setChannelNames (const juce::StringArray& outputNames,
                          const juce::StringArray& inputNames);

    /** Total events ever added this run (not reduced by ring-drop/clear-…);
        shown in the toolbar as "N events". */
    juce::int64 getTotalEventsReceived() const noexcept;
    int getNumRowsStored() const noexcept;

    //==========================================================================
    // Filtering (also settable by the user via the built-in toolbar)
    /** Rows below this severity are hidden (Info shows everything). */
    void setMinimumSeverity (Severity minimum);
    Severity getMinimumSeverity() const noexcept        { return minimumSeverity; }

    /** Show/hide one event type. All types start visible. */
    void setTypeVisible (EventType type, bool shouldShow);
    bool isTypeVisible (EventType type) const noexcept;
    void showAllTypes();

    //==========================================================================
    // Behaviour
    /** When on (default), the table keeps the newest row in view unless the
        user has manually scrolled up; scrolling back to the bottom re-arms
        it. */
    void setAutoScrollEnabled (bool shouldAutoScroll);
    bool isAutoScrollEnabled() const noexcept           { return autoScroll; }

    /** Cap on stored rows before oldest are dropped (display only). */
    void setMaxStoredEvents (int newMax);

    //==========================================================================
    /** Fired when the user clicks Clear in the toolbar, after the rows are
        removed — lets MainComponent also reset its "last critical" banner
        state if it keeps any. Optional. */
    std::function<void()> onCleared;

    //==========================================================================
    // juce::Component
    void resized() override;   // toolbar strip (24 px) on top, table below
    void paint (juce::Graphics&) override;

private:
    //==========================================================================
    // TableListBoxModel
    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int rowNumber, int width,
                             int height, bool rowIsSelected) override;
    void paintCell (juce::Graphics&, int rowNumber, int columnId, int width,
                    int height, bool rowIsSelected) override;
    juce::String getCellTooltip (int rowNumber, int columnId) override;

    //==========================================================================
    /** Stored row: POD event + strings composed once at insertion. */
    struct Row
    {
        Event event;
        juce::String description;
        juce::String timestampText;   // pre-formatted HH:MM:SS.mmm ("" if none)
    };

    void rebuildFilteredIndex();      // recompute visibleRows after filter edits
    void cellDoubleClicked (int rowNumber, int columnId, const juce::MouseEvent&) override;
    bool passesFilter (const Row&) const noexcept;
    void dropOldestIfNeeded();

    std::vector<Row> rows;            // ring-capped at maxStoredEvents
    std::vector<int> visibleRows;     // indices into rows passing the filter
    juce::int64 totalReceived = 0;
    int maxStoredEvents = defaultMaxStoredEvents;

    Severity minimumSeverity = Severity::Info;
    bool typeVisible[numEventTypes] { true, true, true, true, true, true, true }; // per EventType
    bool autoScroll = true;

    // Toolbar widgets (built in the ctor, laid out in resized()):
    juce::ComboBox severityFilterBox;     // All / Warning+ / Critical only
    juce::ComboBox typeFilterBox;         // All types / individual EventType
    juce::ToggleButton autoScrollButton { "Follow" };
    juce::TextButton clearButton { "Clear" };
    juce::Label countLabel;               // "123 events (2 hidden)"

    juce::TableListBox table { "eventLog", this };

    juce::StringArray outputChannelNames, inputChannelNames;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EventLogComponent)
};

} // namespace pnam

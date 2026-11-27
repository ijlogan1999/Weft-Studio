#pragma once

// List editors (D-UI-48): the Event, Marker, Tempo and Signature lists, in the side panel's Lists tab
// or a window of their own. One row per item, edited in place (double-click a cell and type).

#include "Studio.h"
#include "Widgets.h"

namespace weft::ui
{
class ListsPanel final : public juce::Component, public juce::TableListBoxModel, private juce::ChangeListener
{
public:
    enum class List
    {
        events,
        markers,
        tempo,
        signatures
    };
    explicit ListsPanel (Studio& studio, List first = List::events);
    ~ListsPanel() override;
    void resized() override;
    void paint (juce::Graphics&) override;

    void show (List list);
    List current() const noexcept { return which; }
    void refresh();

    struct Row
    {
        enum class Kind
        {
            region,
            note,
            controller,
            marker,
            tempo,
            signature,
            key
        };
        Kind kind = Kind::note;
        Id id, regionId;
        Ticks pos = 0;
    };
    const std::vector<Row>& rows() const noexcept { return items; }
    juce::StringArray columnNames() const;
    juce::String cellText (int row, int column) const; // columns from 0
    /** Applies what was typed into a cell, as one undo step; false (with the status line saying why) if it can't. */
    bool setCell (int row, int column, const juce::String& text);
    /** Selects the row: a marker moves the playhead to it; an event is selected in the piano roll and, with MIDI Out
        on, sounds; a region is selected. */
    void clickRow (int row);
    /** Double-click a region's row: its events. */
    void openRow (int row);
    /** Display Level: up from the events to the list of regions. */
    void levelUp();
    bool showingRegions() const;
    /** The + button at the playhead. Events: "note", "cc", "pitchBend", "channelPressure", "polyPressure",
        "programChange". Signatures: "time" or "key". Markers and tempo take no kind. */
    bool create (const std::string& kind = {});
    void deleteSelected();
    /** Event Type buttons: which kinds of event are listed ("note", "cc", "pitchBend", "pressure", "programChange"). */
    bool kindShown (const std::string& filter) const { return ! hiddenKinds.contains (filter); }
    void setKindShown (const std::string& filter, bool shown);
    /** Marker Text Area: shows the selected marker's text under the list. */
    void setTextArea (bool shown);
    bool textAreaShown() const noexcept { return textShown; }
    const Id& textMarker() const noexcept { return shownMarker; }

    juce::TextButton eventTab { "Event" }, markerTab { "Marker" }, tempoTab { "Tempo" }, signatureTab { "Signature" };
    juce::TextButton add { "+" }, level { "Regions" }, midiOut { "MIDI Out" }, textArea { "Text" };
    juce::TextButton filterNotes { "Notes" }, filterControl { "Control" }, filterBend { "Bend" }, filterPressure { "Pressure" },
        filterProgram { "Program" };
    juce::TableListBox table;
    juce::TextEditor markerText;
    /** Typing into a cell: double-click opens it over the cell; Return or clicking away applies, Escape cancels. */
    juce::TextEditor cellEditor;
    bool editable (int row, int column) const;
    void beginEdit (int row, int column);
    bool commitEdit();
    bool soundEvents = true; // MIDI Out

    // TableListBoxModel
    int getNumRows() override { return static_cast<int> (items.size()); }
    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    void cellClicked (int row, int column, const juce::MouseEvent&) override;
    void cellDoubleClicked (int row, int column, const juce::MouseEvent&) override;
    void deleteKeyPressed (int lastRowSelected) override;
    void selectedRowsChanged (int lastRowSelected) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void layoutColumns();
    bool applyCell (int row, int column, const juce::String& text);
    std::vector<Id> eventRegions() const;
    std::string filterFor (const Row& row) const;
    void saveMarkerText();
    void showAddMenu();
    juce::String positionText (Ticks t) const;
    juce::String lengthText (Ticks pos, Ticks length) const;
    std::optional<Ticks> parseLength (Ticks pos, const juce::String& text) const;

    Studio& studio;
    List which = List::events;
    std::vector<Row> items;
    std::set<std::string> hiddenKinds;
    bool regionLevel = false;
    bool textShown = false;
    bool regionColumns = false;
    int columnsFor = -1;
    Id shownMarker;
    std::set<Id> lastSelection;
    int editRow = -1, editColumn = -1;
};
} // namespace weft::ui

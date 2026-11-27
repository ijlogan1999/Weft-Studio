#include "ListsPanel.h"

#include "weft/commands/Notes.h"
#include "weft/time/Position.h"

#include <algorithm>

namespace weft::ui
{
namespace
{
using Kind = ListsPanel::Row::Kind;
using commands::EditResult;

std::optional<int> parseInt (const juce::String& text)
{
    const auto t = text.trim();
    if (t.isEmpty() || ! t.retainCharacters ("-0123456789").equalsIgnoreCase (t))
        return std::nullopt;
    return t.getIntValue();
}

/** "C3", "F#2", "Bb4" (as noteName shows them, with the Middle C setting) or a MIDI number. */
std::optional<int> parsePitch (const juce::String& text)
{
    const auto t = text.trim();
    if (auto n = parseInt (t))
        return *n >= 0 && *n <= 127 ? n : std::nullopt;
    if (t.isEmpty())
        return std::nullopt;
    static const int classes[] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
    const auto letter = juce::CharacterFunctions::toUpperCase (t[0]);
    if (letter < 'A' || letter > 'G')
        return std::nullopt;
    int pc = classes[letter - 'A'];
    auto rest = t.substring (1);
    if (rest.startsWith ("#"))
        ++pc, rest = rest.substring (1);
    else if (rest.startsWith ("b"))
        --pc, rest = rest.substring (1);
    const auto octave = parseInt (rest);
    if (! octave)
        return std::nullopt;
    const auto pitch = 60 + (*octave - commands::middleCOctave()) * 12 + pc;
    return pitch >= 0 && pitch <= 127 ? std::optional<int> (pitch) : std::nullopt;
}

juce::String statusFor (const model::CcEvent& e)
{
    if (e.kind == "pitchBend")
        return "Pitch Bend";
    if (e.kind == "channelPressure")
        return "Ch Pressure";
    if (e.kind == "polyPressure")
        return "Poly Pressure";
    if (e.kind == "programChange")
        return "Program";
    return "Control";
}

juce::String utf8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }
} // namespace

ListsPanel::ListsPanel (Studio& s, List first) : studio (s), which (first)
{
    const auto tab = [this] (juce::TextButton& b, List list, const juce::String& tip) {
        describe (b, b.getButtonText() + " List", tip);
        b.setClickingTogglesState (false);
        b.onClick = [this, list] { show (list); };
        addAndMakeVisible (b);
    };
    tab (eventTab, List::events, "The events of the selected MIDI regions");
    tab (markerTab, List::markers, "Every marker");
    tab (tempoTab, List::tempo, "Every tempo change");
    tab (signatureTab, List::signatures, "Every time signature and key change");

    describe (add, "Create", "Add one at the playhead");
    add.onClick = [this] {
        if (which == List::events || which == List::signatures)
            showAddMenu();
        else
            create();
    };
    describe (level, "Display Level", "Up from the events to the list of regions");
    level.onClick = [this] { levelUp(); };
    describe (midiOut, "MIDI Out", "Events sound when you select them");
    midiOut.onClick = [this] {
        soundEvents = ! soundEvents;
        refresh();
    };
    describe (textArea, "Marker Text Area", "Show the selected marker's text");
    textArea.onClick = [this] { setTextArea (! textShown); };
    const auto filter = [this] (juce::TextButton& b, std::string kind) {
        describe (b, "Show " + b.getButtonText(), "Event Type: list or hide these events");
        b.onClick = [this, kind] { setKindShown (kind, ! kindShown (kind)); };
        addChildComponent (b);
    };
    filter (filterNotes, "note");
    filter (filterControl, "cc");
    filter (filterBend, "pitchBend");
    filter (filterPressure, "pressure");
    filter (filterProgram, "programChange");
    for (auto* b : { &add, &level, &midiOut, &textArea })
        addChildComponent (*b);
    add.setVisible (true);

    table.setModel (this);
    table.setRowHeight (size::control);
    table.setMultipleSelectionEnabled (true);
    table.setColour (juce::ListBox::backgroundColourId, colour (Token::surface));
    describe (table, "List", "Double-click a cell to type a new value; Delete removes the selected rows");
    addAndMakeVisible (table);

    cellEditor.setFont (font (Type::body));
    cellEditor.onReturnKey = [this] { commitEdit(); };
    cellEditor.onEscapeKey = [this] {
        editRow = editColumn = -1;
        cellEditor.setVisible (false);
    };
    cellEditor.onFocusLost = [this] { commitEdit(); };
    table.addChildComponent (cellEditor);

    markerText.setMultiLine (true);
    markerText.setReturnKeyStartsNewLine (true);
    markerText.setFont (font (Type::body));
    describe (markerText, "Marker Text", "Notes for the selected marker");
    markerText.onFocusLost = [this] { saveMarkerText(); };
    addChildComponent (markerText);

    studio.addChangeListener (this);
    setSize (420, 420);
    show (first);
}

ListsPanel::~ListsPanel()
{
    studio.removeChangeListener (this);
    table.setModel (nullptr);
}

void ListsPanel::show (List list)
{
    if (which == List::markers && list != List::markers)
        saveMarkerText();
    which = list;
    table.deselectAllRows();
    refresh();
    resized();
}

void ListsPanel::setKindShown (const std::string& filter, bool shown)
{
    if (shown)
        hiddenKinds.erase (filter);
    else
        hiddenKinds.insert (filter);
    refresh();
}

void ListsPanel::setTextArea (bool shown)
{
    if (textShown && ! shown)
        saveMarkerText();
    textShown = shown;
    refresh();
    resized();
}

void ListsPanel::levelUp()
{
    regionLevel = true;
    table.deselectAllRows();
    refresh();
}

std::vector<Id> ListsPanel::eventRegions() const
{
    if (regionLevel)
        return {};
    const auto& m = studio.model();
    std::vector<Id> out;
    for (const auto& id : studio.selection.regions())
        if (const auto* r = m.region (id); r != nullptr && r->isMidi)
            out.push_back (id);
    if (out.empty())
        out = studio.pianoRollRegions();
    std::sort (out.begin(), out.end(), [&] (const Id& a, const Id& b) {
        const auto *ra = m.region (a), *rb = m.region (b);
        return ra->start != rb->start ? ra->start < rb->start : a < b;
    });
    return out;
}

bool ListsPanel::showingRegions() const { return which == List::events && eventRegions().empty(); }

std::string ListsPanel::filterFor (const Row& row) const
{
    if (row.kind == Kind::note)
        return "note";
    if (row.kind != Kind::controller)
        return {};
    const auto* r = studio.model().region (row.regionId);
    const auto* seq = r != nullptr ? studio.model().sequence (r->midi.sequenceId) : nullptr;
    if (seq == nullptr || ! seq->controllers.contains (row.id))
        return {};
    const auto& kind = seq->controllers.at (row.id).kind;
    return kind == "channelPressure" || kind == "polyPressure" ? "pressure" : kind;
}

void ListsPanel::refresh()
{
    const auto& m = studio.model();
    // A selection made elsewhere (the tracks area) shows that region's events again.
    if (studio.selection.regions() != lastSelection)
    {
        lastSelection = studio.selection.regions();
        regionLevel = false;
    }
    items.clear();
    if (which == List::events)
    {
        const auto regions = eventRegions();
        if (regions.empty())
        {
            for (const auto& [id, r] : m.regions)
                items.push_back ({ Kind::region, id, id, r->start });
        }
        else
            for (const auto& rid : regions)
            {
                const auto* r = m.region (rid);
                const auto* seq = m.sequence (r->midi.sequenceId);
                if (seq == nullptr)
                    continue;
                for (const auto* n : seq->sortedNotes())
                    items.push_back ({ Kind::note, n->id, rid, commands::noteTimelineTick (*r, n->pos) });
                for (const auto& [cid, c] : seq->controllers)
                    items.push_back ({ Kind::controller, cid, rid, commands::noteTimelineTick (*r, c.pos) });
            }
        std::erase_if (items, [this] (const Row& row) {
            const auto f = filterFor (row);
            return ! f.empty() && ! kindShown (f);
        });
    }
    else if (which == List::markers)
    {
        for (const auto& [id, mk] : m.markers)
            items.push_back ({ Kind::marker, id, {}, mk->pos });
    }
    else if (which == List::tempo)
    {
        for (const auto& p : m.tempo.points())
            items.push_back ({ Kind::tempo, p.id, {}, p.pos });
    }
    else
    {
        // The project's key is the first key row (it holds from the start); then the changes.
        items.push_back ({ Kind::key, {}, {}, 0 });
        for (const auto& p : m.signatures.points())
            items.push_back ({ Kind::signature, p.id, {}, m.signatures.barStart (p.bar) });
        for (const auto* k : m.keyChangesInOrder())
            items.push_back ({ Kind::key, k->id, {}, k->pos });
    }
    std::stable_sort (items.begin(), items.end(), [&] (const Row& a, const Row& b) {
        if (a.pos != b.pos)
            return a.pos < b.pos;
        if (a.kind == Kind::region && b.kind == Kind::region)
        {
            const auto order = [&] (const Id& rid) {
                const auto tid = m.effectiveTrackId (*m.region (rid));
                const auto it = std::find (m.trackOrder.begin(), m.trackOrder.end(), tid);
                return it - m.trackOrder.begin();
            };
            return order (a.id) < order (b.id);
        }
        if (a.kind == Kind::signature && b.kind == Kind::key)
            return true;
        if (a.kind == Kind::key && b.kind == Kind::signature)
            return false;
        return a.kind == Kind::key && b.kind == Kind::key ? a.id < b.id : false;
    });

    layoutColumns();
    table.updateContent();
    table.repaint();

    // The buttons for this list.
    const bool events = which == List::events;
    for (const auto& [b, list] : { std::pair { &eventTab, List::events }, std::pair { &markerTab, List::markers }, std::pair { &tempoTab, List::tempo },
                             std::pair { &signatureTab, List::signatures } })
        b->setToggleState (which == list, juce::dontSendNotification);
    level.setVisible (events);
    level.setEnabled (events && ! showingRegions());
    midiOut.setVisible (events);
    midiOut.setToggleState (soundEvents, juce::dontSendNotification);
    textArea.setVisible (which == List::markers);
    textArea.setToggleState (textShown, juce::dontSendNotification);
    for (const auto& [b, kind] : { std::pair { &filterNotes, "note" }, std::pair { &filterControl, "cc" }, std::pair { &filterBend, "pitchBend" },
                             std::pair { &filterPressure, "pressure" }, std::pair { &filterProgram, "programChange" } })
    {
        b->setVisible (events && ! showingRegions());
        b->setToggleState (kindShown (kind), juce::dontSendNotification);
    }
    add.setEnabled (! showingRegions());

    // The marker text area follows the selected marker.
    markerText.setVisible (which == List::markers && textShown);
    if (which == List::markers && textShown)
    {
        const auto row = table.getSelectedRow();
        const Id id = row >= 0 && row < getNumRows() ? items[static_cast<std::size_t> (row)].id : Id();
        if (id != shownMarker)
        {
            saveMarkerText();
            shownMarker = id;
        }
        const auto it = m.markers.find (shownMarker);
        if (it == m.markers.end())
            shownMarker.clear();
        if (! markerText.hasKeyboardFocus (true))
            markerText.setText (it != m.markers.end() ? utf8 (it->second->text) : juce::String(), false);
        markerText.setReadOnly (shownMarker.empty());
    }
}

void ListsPanel::layoutColumns()
{
    const bool regions = showingRegions();
    const int key = static_cast<int> (which) * 2 + (regions ? 1 : 0);
    if (key == columnsFor)
        return;
    columnsFor = key;
    auto& header = table.getHeader();
    header.removeAllColumns();
    const auto names = columnNames();
    for (int i = 0; i < names.size(); ++i)
    {
        const int width = i == 0 ? 100 : names[i] == "Ch" ? 36 : names[i] == "Name" || names[i] == "Value" || names[i] == "Status" ? 110 : 64;
        header.addColumn (names[i], i + 1, width, 30, -1, juce::TableHeaderComponent::notSortable);
    }
}

juce::StringArray ListsPanel::columnNames() const
{
    switch (which)
    {
        case List::events:
            return showingRegions() ? juce::StringArray { "Position", "Name", "Track", "Length" }
                                    : juce::StringArray { "Position", "Status", "Ch", "Num", "Val", "Length" };
        case List::markers: return { "Position", "Name" };
        case List::tempo: return { "Position", "Tempo" };
        case List::signatures: return { "Position", "Type", "Value" };
    }
    return {};
}

juce::String ListsPanel::positionText (Ticks t) const { return formatBarsBeats (t, studio.model().signatures); }

juce::String ListsPanel::lengthText (Ticks pos, Ticks length) const
{
    // Bars, beats, divisions and ticks, from 0: one beat is "0.1.0.0".
    const auto& sig = studio.model().signatures;
    const auto& point = sig.signatureAtBar (sig.barAt (pos).bar);
    const auto bar = point.barLength();
    const Ticks beat = 4 * ticksPerQuarter / point.denominator;
    const auto bars = length / bar;
    auto rest = length % bar;
    const auto beats = rest / beat;
    rest %= beat;
    const auto divisions = rest / divisionTicks;
    rest %= divisionTicks;
    return juce::String (bars) + "." + juce::String (beats) + "." + juce::String (divisions) + "." + juce::String (rest);
}

std::optional<Ticks> ListsPanel::parseLength (Ticks pos, const juce::String& text) const
{
    juce::StringArray parts;
    parts.addTokens (text.trim(), ". ", "");
    parts.removeEmptyStrings();
    if (parts.isEmpty() || parts.size() > 4)
        return std::nullopt;
    const auto& sig = studio.model().signatures;
    const auto& point = sig.signatureAtBar (sig.barAt (pos).bar);
    const Ticks units[] = { point.barLength(), 4 * ticksPerQuarter / point.denominator, divisionTicks, 1 };
    Ticks total = 0;
    for (int i = 0; i < parts.size(); ++i)
    {
        const auto n = parseInt (parts[i]);
        if (! n || *n < 0)
            return std::nullopt;
        total += *n * units[i];
    }
    return total > 0 ? std::optional<Ticks> (total) : std::nullopt;
}

juce::String ListsPanel::cellText (int row, int column) const
{
    if (row < 0 || row >= static_cast<int> (items.size()))
        return {};
    const auto& item = items[static_cast<std::size_t> (row)];
    const auto& m = studio.model();
    if (column == 0)
        return positionText (item.pos);
    switch (item.kind)
    {
        case Kind::region:
        {
            const auto* r = m.region (item.id);
            if (r == nullptr)
                return {};
            if (column == 1)
                return utf8 (r->name);
            if (column == 2)
            {
                const auto* t = m.track (m.effectiveTrackId (*r));
                return t != nullptr ? utf8 (t->name) : juce::String();
            }
            return lengthText (r->start, static_cast<Ticks> (std::llround (m.regionEndTick (*r))) - r->start);
        }
        case Kind::note:
        case Kind::controller:
        {
            const auto* r = m.region (item.regionId);
            const auto* seq = r != nullptr ? m.sequence (r->midi.sequenceId) : nullptr;
            if (seq == nullptr)
                return {};
            if (item.kind == Kind::note)
            {
                const auto it = seq->notes.find (item.id);
                if (it == seq->notes.end())
                    return {};
                const auto& n = it->second;
                const juce::String cells[] = { {}, "Note", juce::String (n.channel), utf8 (commands::noteName (n.pitch)), juce::String (n.velocity),
                                               lengthText (item.pos, n.length) };
                return column < 6 ? cells[column] : juce::String();
            }
            const auto it = seq->controllers.find (item.id);
            if (it == seq->controllers.end())
                return {};
            const auto& e = it->second;
            const auto num = e.kind == "cc" ? juce::String (e.number) : e.kind == "polyPressure" ? utf8 (commands::noteName (e.number)) : juce::String();
            const juce::String cells[] = { {}, statusFor (e), juce::String (e.channel), num, juce::String (e.value), {} };
            return column < 6 ? cells[column] : juce::String();
        }
        case Kind::marker:
        {
            const auto it = m.markers.find (item.id);
            return it != m.markers.end() && column == 1 ? utf8 (it->second->name) : juce::String();
        }
        case Kind::tempo:
            for (const auto& p : m.tempo.points())
                if (p.id == item.id)
                    return juce::String (p.bpm, 2);
            return {};
        case Kind::signature:
            if (column == 1)
                return "Time Signature";
            for (const auto& p : m.signatures.points())
                if (p.id == item.id)
                    return juce::String (p.numerator) + "/" + juce::String (p.denominator);
            return {};
        case Kind::key:
        {
            if (column == 1)
                return item.id.empty() ? "Key (project)" : "Key";
            if (item.id.empty())
                return keyText (m.info.key);
            const auto it = m.keyChanges.find (item.id);
            return it != m.keyChanges.end() ? keyText (it->second->key) : juce::String();
        }
    }
    return {};
}

bool ListsPanel::editable (int row, int column) const
{
    if (row < 0 || row >= static_cast<int> (items.size()))
        return false;
    const auto& item = items[static_cast<std::size_t> (row)];
    switch (item.kind)
    {
        case Kind::region: return column == 1;
        case Kind::note: return column != 1;
        case Kind::controller:
        {
            if (column == 1 || column == 5)
                return false;
            if (column != 3)
                return true;
            const auto text = cellText (row, 3);
            return text.isNotEmpty();
        }
        case Kind::marker:
        case Kind::tempo: return column <= 1;
        case Kind::signature: return column == 0 || column == 2;
        case Kind::key: return column == 2 || (column == 0 && ! item.id.empty());
    }
    return false;
}

bool ListsPanel::setCell (int row, int column, const juce::String& text)
{
    const bool done = applyCell (row, column, text);
    refresh();
    return done;
}

bool ListsPanel::applyCell (int row, int column, const juce::String& text)
{
    if (! editable (row, column))
        return false;
    const auto item = items[static_cast<std::size_t> (row)];
    const auto& m = studio.model();
    auto& s = studio.session();
    const auto fail = [this] (const char* why) { return studio.check (EditResult::fail (why)); };
    std::optional<Ticks> at;
    if (column == 0)
    {
        at = parseBarsBeats (text.toStdString(), m.signatures);
        if (! at)
            return fail ("Type a position as bar beat division tick, like 5 1 1 0.");
    }
    switch (item.kind)
    {
        case Kind::region:
            return studio.check (s.setRegionName (item.id, text.trim().toStdString()));
        case Kind::note:
        {
            const auto* r = m.region (item.regionId);
            const auto* seq = r != nullptr ? m.sequence (r->midi.sequenceId) : nullptr;
            if (seq == nullptr || ! seq->notes.contains (item.id))
                return fail ("That note no longer exists.");
            auto note = seq->notes.at (item.id);
            if (column == 0)
            {
                if (*at < r->start)
                    return fail ("A note can't start before its region.");
                note.pos = commands::noteSequenceTick (*r, *at);
            }
            else if (column == 2)
            {
                const auto ch = parseInt (text);
                if (! ch || *ch < 1 || *ch > 16)
                    return fail ("The channel is 1 to 16.");
                note.channel = *ch;
            }
            else if (column == 3)
            {
                const auto pitch = parsePitch (text);
                if (! pitch)
                    return fail ("Type a note like C3 or a number 0 to 127.");
                note.pitch = *pitch;
            }
            else if (column == 4)
            {
                const auto v = parseInt (text);
                if (! v || *v < 1 || *v > 127)
                    return fail ("The velocity is 1 to 127.");
                note.velocity = *v;
            }
            else
            {
                const auto length = parseLength (item.pos, text);
                if (! length)
                    return fail ("Type a length as bars beats divisions ticks, like 0 1 0 0.");
                note.length = *length;
            }
            if (note == seq->notes.at (item.id))
                return false;
            return studio.check (s.updateNote (item.regionId, note));
        }
        case Kind::controller:
        {
            const auto* r = m.region (item.regionId);
            const auto* seq = r != nullptr ? m.sequence (r->midi.sequenceId) : nullptr;
            if (seq == nullptr || ! seq->controllers.contains (item.id))
                return fail ("That event no longer exists.");
            auto e = seq->controllers.at (item.id);
            e.pos = item.pos;
            if (column == 0)
                e.pos = *at;
            else if (column == 2)
            {
                const auto ch = parseInt (text);
                if (! ch || *ch < 1 || *ch > 16)
                    return fail ("The channel is 1 to 16.");
                e.channel = *ch;
            }
            else if (column == 3)
            {
                const auto n = e.kind == "polyPressure" ? parsePitch (text) : parseInt (text);
                if (! n || *n < 0 || *n > 127)
                    return fail ("The number is 0 to 127.");
                e.number = *n;
            }
            else
            {
                const auto v = parseInt (text);
                if (! v)
                    return fail ("Type a number.");
                e.value = *v;
            }
            return studio.check (s.setControllerEvent (item.regionId, e));
        }
        case Kind::marker:
            return studio.check (column == 0 ? s.moveMarker (item.id, *at) : s.renameMarker (item.id, text.trim().toStdString()));
        case Kind::tempo:
            for (const auto& p : m.tempo.points())
                if (p.id == item.id)
                {
                    if (column == 0)
                        return studio.check (s.setTempoPoint (p.id, *at, p.bpm));
                    const auto bpm = text.trim().getDoubleValue();
                    if (bpm < 20.0 || bpm > 999.0 || text.trim().isEmpty())
                        return fail ("The tempo is 20 to 999 BPM.");
                    return studio.check (s.setTempoPoint (p.id, p.pos, bpm));
                }
            return fail ("That tempo change no longer exists.");
        case Kind::signature:
            for (const auto& p : m.signatures.points())
                if (p.id == item.id)
                {
                    if (column == 0)
                        return studio.check (s.setSignaturePoint (p.id, m.signatures.barAt (*at).bar, p.numerator, p.denominator));
                    juce::StringArray parts;
                    parts.addTokens (text.trim(), "/", "");
                    const auto num = parts.size() == 2 ? parseInt (parts[0]) : std::nullopt;
                    const auto den = parts.size() == 2 ? parseInt (parts[1]) : std::nullopt;
                    if (! num || ! den || ! SignatureMap::isValid (*num, *den))
                        return fail ("Type a time signature like 3/4.");
                    return studio.check (s.setSignaturePoint (p.id, p.bar, *num, *den));
                }
            return fail ("That signature change no longer exists.");
        case Kind::key:
        {
            if (column == 0)
            {
                const auto it = m.keyChanges.find (item.id);
                if (it == m.keyChanges.end())
                    return fail ("That key change no longer exists.");
                return studio.check (s.setKeyChange (item.id, *at, it->second->key));
            }
            const auto key = model::KeySignature::parse (text.trim().toStdString());
            if (! key)
                return fail ("Type a key like F# minor or Bb.");
            if (item.id.empty())
                return studio.check (s.setKey (*key));
            const auto it = m.keyChanges.find (item.id);
            if (it == m.keyChanges.end())
                return fail ("That key change no longer exists.");
            return studio.check (s.setKeyChange (item.id, it->second->pos, *key));
        }
    }
    return false;
}

void ListsPanel::clickRow (int row)
{
    if (row < 0 || row >= getNumRows())
        return;
    if (! table.isRowSelected (row))
        table.selectRow (row);
    const auto item = items[static_cast<std::size_t> (row)];
    const auto& m = studio.model();
    if (item.kind == Kind::marker)
        studio.locate (item.pos);
    else if (item.kind == Kind::region)
    {
        // Stay on the regions; double-click opens one.
        studio.selection.set ({ item.id });
        lastSelection = studio.selection.regions();
        regionLevel = true;
        studio.sendChangeMessage();
    }
    else if (item.kind == Kind::note)
    {
        const auto* r = m.region (item.regionId);
        const auto* seq = r != nullptr ? m.sequence (r->midi.sequenceId) : nullptr;
        if (seq == nullptr || ! seq->notes.contains (item.id))
            return;
        studio.piano.selection.clear();
        const auto rowsSelected = table.getSelectedRows();
        for (const auto selected : rowsSelected.getRanges())
            for (int i = selected.getStart(); i < selected.getEnd(); ++i)
                if (const auto& it = items[static_cast<std::size_t> (i)]; it.kind == Kind::note)
                    studio.piano.selection.insert ({ it.regionId, it.id });
        const auto& n = seq->notes.at (item.id);
        if (soundEvents)
            studio.auditionNote (item.regionId, n.pitch + r->midi.transpose, n.velocity, n.channel, n.length);
        studio.sendChangeMessage();
    }
}

void ListsPanel::openRow (int row)
{
    if (row < 0 || row >= getNumRows() || items[static_cast<std::size_t> (row)].kind != Kind::region)
        return;
    const auto id = items[static_cast<std::size_t> (row)].id;
    studio.selection.set ({ id });
    lastSelection = studio.selection.regions();
    const auto* r = studio.model().region (id);
    regionLevel = r == nullptr || ! r->isMidi; // an audio region has no events to list
    table.deselectAllRows();
    studio.sendChangeMessage();
    refresh();
}

void ListsPanel::showAddMenu()
{
    juce::PopupMenu menu;
    if (which == List::events)
    {
        int i = 1;
        for (const auto* name : { "Note", "Controller", "Pitch Bend", "Channel Pressure", "Poly Pressure", "Program Change" })
            menu.addItem (i++, name);
    }
    else
    {
        menu.addItem (1, "Time Signature");
        menu.addItem (2, "Key Signature");
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (add), [safe = juce::Component::SafePointer<ListsPanel> (this)] (int item) {
        if (safe == nullptr || item <= 0)
            return;
        static const char* const events[] = { "note", "cc", "pitchBend", "channelPressure", "polyPressure", "programChange" };
        if (safe->which == List::events)
            safe->create (events[item - 1]);
        else
            safe->create (item == 1 ? "time" : "key");
    });
}

bool ListsPanel::create (const std::string& kind)
{
    const auto& m = studio.model();
    auto& s = studio.session();
    const auto at = studio.playhead();
    std::optional<Id> made;
    if (which == List::events)
    {
        const auto regions = eventRegions();
        if (regions.empty())
            return studio.check (EditResult::fail ("Select a MIDI region first."));
        // The region under the playhead, else the first; the event goes inside it.
        const auto* r = m.region (regions.front());
        for (const auto& id : regions)
            if (const auto* c = m.region (id); c->start <= at && at < c->start + c->length)
                r = c;
        const auto pos = std::clamp (at, r->start, std::max (r->start, r->start + r->length - 1));
        if (kind == "note" || kind.empty())
        {
            model::Note n;
            n.pos = commands::noteSequenceTick (*r, pos);
            n.length = ticksPerQuarter;
            made = s.addNote (r->id, n);
        }
        else
        {
            model::CcEvent e;
            e.pos = pos;
            e.kind = kind;
            e.number = kind == "cc" ? 1 : kind == "polyPressure" ? 60 : 0;
            e.value = kind == "pitchBend" || kind == "programChange" ? 0 : 64;
            made = s.addController (r->id, e);
        }
        if (made)
            setKindShown (kind == "channelPressure" || kind == "polyPressure" ? "pressure" : kind.empty() ? "note" : kind, true);
    }
    else if (which == List::markers)
        made = s.addMarker (at, "");
    else if (which == List::tempo)
        made = s.addTempoPoint (at, m.tempo.bpmAt (at));
    else if (kind == "key")
        made = s.addKeyChange (at, m.keyAt (at));
    else
    {
        const auto bar = m.signatures.barAt (at).bar;
        const auto& sig = m.signatures.signatureAtBar (bar);
        made = s.addSignaturePoint (bar, sig.numerator, sig.denominator);
    }
    if (! made)
        return studio.check (EditResult::fail (s.lastError()));
    studio.edited();
    refresh();
    for (int i = 0; i < getNumRows(); ++i)
        if (items[static_cast<std::size_t> (i)].id == *made)
        {
            table.selectRow (i);
            break;
        }
    return true;
}

void ListsPanel::deleteSelected()
{
    const auto selected = table.getSelectedRows();
    if (selected.isEmpty())
        return;
    std::vector<commands::NoteRef> notes;
    std::map<Id, std::vector<Id>> controllers;
    std::vector<Row> others;
    for (int k = 0; k < selected.size(); ++k)
    {
        const auto i = selected[k];
        if (i < 0 || i >= getNumRows())
            continue;
        const auto& item = items[static_cast<std::size_t> (i)];
        if (item.kind == Kind::note)
            notes.push_back ({ item.regionId, item.id });
        else if (item.kind == Kind::controller)
            controllers[item.regionId].push_back (item.id);
        else
            others.push_back (item);
    }
    auto& s = studio.session();
    s.beginGesture ("Delete");
    bool any = false;
    if (! notes.empty())
        any = studio.check (s.deleteNotes (notes)) || any;
    for (const auto& [rid, ids] : controllers)
        any = studio.check (s.deleteControllers (rid, ids)) || any;
    for (const auto& item : others)
    {
        if (item.kind == Kind::marker)
            any = studio.check (s.deleteMarker (item.id)) || any;
        else if (item.kind == Kind::tempo)
            any = studio.check (s.deleteTempoPoint (item.id)) || any;
        else if (item.kind == Kind::signature)
            any = studio.check (s.deleteSignaturePoint (item.id)) || any;
        else if (item.kind == Kind::key && ! item.id.empty())
            any = studio.check (s.deleteKeyChange (item.id)) || any;
        else if (item.kind == Kind::region)
            any = studio.check (s.deleteRegions ({ item.id })) || any;
    }
    s.endGesture();
    table.deselectAllRows();
    if (any)
        studio.piano.selection.clear();
    studio.edited();
    refresh();
}

void ListsPanel::saveMarkerText()
{
    const auto& m = studio.model();
    const auto it = m.markers.find (shownMarker);
    if (it == m.markers.end())
        return;
    const auto text = markerText.getText().toStdString();
    if (text != it->second->text)
        studio.check (studio.session().setMarkerText (shownMarker, text));
}

void ListsPanel::beginEdit (int row, int column)
{
    if (! editable (row, column))
        return;
    editRow = row;
    editColumn = column;
    cellEditor.setBounds (table.getCellPosition (column + 1, row, true).translated (0, table.getHeaderHeight()));
    cellEditor.setText (cellText (row, column), false);
    cellEditor.setVisible (true);
    cellEditor.grabKeyboardFocus();
    cellEditor.selectAll();
}

bool ListsPanel::commitEdit()
{
    if (editRow < 0)
        return false;
    const auto row = editRow, column = editColumn;
    editRow = editColumn = -1;
    cellEditor.setVisible (false);
    const auto text = cellEditor.getText();
    if (text == cellText (row, column))
        return false;
    return setCell (row, column, text);
}

void ListsPanel::paintRowBackground (juce::Graphics& g, int row, int width, int height, bool selected)
{
    if (selected)
        g.fillAll (colour (Token::accent).withAlpha (0.35f));
    else if (row % 2 == 1)
        g.fillAll (colour (Token::surfaceRaised).withAlpha (0.5f));
    g.setColour (colour (Token::lineSubtle));
    g.drawHorizontalLine (height - 1, 0.0f, static_cast<float> (width));
}

void ListsPanel::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool)
{
    g.setColour (colour (Token::text));
    g.setFont (font (column == 1 ? Type::body : Type::monoValue));
    g.drawText (cellText (row, column - 1), space::half, 0, width - space::s1, height, juce::Justification::centredLeft, true);
}

void ListsPanel::cellClicked (int row, int, const juce::MouseEvent& e)
{
    if (e.mods.isShiftDown() || e.mods.isCommandDown())
        return; // the table extends the selection
    clickRow (row);
}

void ListsPanel::cellDoubleClicked (int row, int column, const juce::MouseEvent&)
{
    if (editable (row, column - 1))
        beginEdit (row, column - 1);
    else
        openRow (row);
}

void ListsPanel::deleteKeyPressed (int) { deleteSelected(); }

void ListsPanel::selectedRowsChanged (int)
{
    if (which == List::markers && textShown)
        refresh();
}

void ListsPanel::paint (juce::Graphics& g) { g.fillAll (colour (Token::surface)); }

void ListsPanel::resized()
{
    auto r = getLocalBounds().reduced (space::half);
    auto tabs = r.removeFromTop (size::control);
    const auto w = tabs.getWidth() / 4;
    for (auto* b : { &eventTab, &markerTab, &tempoTab, &signatureTab })
        b->setBounds (tabs.removeFromLeft (w).reduced (1, 0));
    r.removeFromTop (space::half);
    auto bar = r.removeFromTop (size::control);
    add.setBounds (bar.removeFromLeft (size::control));
    bar.removeFromLeft (space::half);
    if (which == List::events)
    {
        level.setBounds (bar.removeFromLeft (64));
        bar.removeFromLeft (space::half);
        midiOut.setBounds (bar.removeFromRight (64));
        bar.removeFromRight (space::half);
        auto filters = r.removeFromTop (size::control);
        r.removeFromTop (space::half);
        const auto fw = filters.getWidth() / 5;
        for (auto* b : { &filterNotes, &filterControl, &filterBend, &filterPressure, &filterProgram })
            b->setBounds (filters.removeFromLeft (fw).reduced (1, 0));
    }
    else if (which == List::markers)
        textArea.setBounds (bar.removeFromLeft (56));
    r.removeFromTop (space::half);
    if (which == List::markers && textShown)
    {
        markerText.setBounds (r.removeFromBottom (std::max (80, r.getHeight() / 3)));
        r.removeFromBottom (space::half);
    }
    table.setBounds (r);
}
} // namespace weft::ui

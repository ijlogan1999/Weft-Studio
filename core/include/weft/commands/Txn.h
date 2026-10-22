#pragma once

#include "weft/doc/Document.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace weft::commands
{
using doc::Path;
using doc::Value;

/**
    What one local change wrote, for per-user undo (spec 05 §6).

    A set record restores `before` (absent = remove) only while the field still
    holds `after` (D-COL-6). For containers "still holds" means "is still the
    object this change created", compared by op id, so a collaborator's edit
    inside an object we created doesn't block undoing the creation.

    List records add or remove one element identified by its id (list elements
    in the schema are ids or maps with an "id" field).
*/
struct TxnRecord
{
    enum class Kind
    {
        set,
        listInsert,
        listRemove
    };

    Kind kind = Kind::set;
    Path path;                   // set: the field; list: the list
    std::optional<Value> before; // set only
    std::optional<Value> after;  // set: written value; list: the element
    std::string afterOpId;       // set with container after: op id of the object written
    std::string beforeOpId;      // set with container before: op id of the object replaced or removed
    std::string restoresOpId;    // undo/redo: this write re-creates the object that had this op id
    /** Set with container before: op ids of the objects nested in it, by path relative to it.
        Re-creating the container gives them new op ids too, which undo/redo must alias. */
    std::vector<std::pair<Path, std::string>> beforeNestedOpIds;
    std::vector<std::pair<Path, std::string>> restoresNestedOpIds; // undo/redo: as restoresOpId, for nested objects
    std::string elementId;       // list
    std::size_t index = 0;       // list: index at the time of the write
};

struct Txn
{
    std::string name;
    std::vector<TxnRecord> records;

    bool empty() const noexcept { return records.empty(); }
};

/**
    Writes to the document and records each write in a Txn. Repeated writes to the
    same path merge into one record (first before, last after), so a 200-event
    drag is one record per field.
*/
class TxnWriter
{
public:
    TxnWriter (doc::Document& d, Txn& t) : document (d), txn (t) {}

    bool put (const Path& p, const Value& v);
    bool remove (const Path& p);
    /** Inserts an element into a list at index (clamped to the list size). */
    bool listInsert (const Path& list, std::size_t index, const Value& element, const std::string& elementId);
    /** Removes the element with the given id from a list. */
    bool listRemove (const Path& list, const std::string& elementId);

    doc::Document& doc() noexcept { return document; }

private:
    void recordSet (const Path& p, std::optional<Value> before, std::optional<Value> after, const std::string& beforeOpId);
    /** Op ids of the containers nested in the container at p, by relative path. */
    std::vector<std::pair<Path, std::string>> nestedOpIds (const Path& p, const Value& container) const;

    doc::Document& document;
    Txn& txn;
    std::map<std::string, std::size_t> setIndex; // toString(path) -> record index
};

/** Element id of a list element: the string itself, or a map's "id" field. */
std::string listElementId (const Value& element);
} // namespace weft::commands

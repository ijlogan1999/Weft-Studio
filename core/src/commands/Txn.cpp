#include "weft/commands/Txn.h"

#include <algorithm>

namespace weft::commands
{
using doc::PathSegment;
using doc::Status;

std::string listElementId (const Value& element) { return element.isString() ? element.asString() : element["id"].asString(); }

void TxnWriter::recordSet (const Path& p, std::optional<Value> before, std::optional<Value> after, const std::string& beforeOpId)
{
    const auto key = doc::toString (p);
    if (auto it = setIndex.find (key); it != setIndex.end())
    {
        txn.records[it->second].after = std::move (after); // first before, last after
        return;
    }
    setIndex.emplace (key, txn.records.size());
    TxnRecord r;
    r.kind = TxnRecord::Kind::set;
    r.path = p;
    r.beforeOpId = beforeOpId;
    r.before = std::move (before);
    r.after = std::move (after);
    txn.records.push_back (std::move (r));
}

bool TxnWriter::put (const Path& p, const Value& v)
{
    auto before = document.get (p);
    const auto beforeOp = before && before->isContainer() ? document.opId (p).value_or ("") : std::string();
    if (document.put (p, v) != Status::ok)
        return false;
    recordSet (p, std::move (before), v, beforeOp);
    return true;
}

bool TxnWriter::remove (const Path& p)
{
    auto before = document.get (p);
    if (! before)
        return true; // already gone
    const auto beforeOp = before->isContainer() ? document.opId (p).value_or ("") : std::string();
    if (document.remove (p) != Status::ok)
        return false;
    recordSet (p, std::move (before), std::nullopt, beforeOp);
    return true;
}

bool TxnWriter::listInsert (const Path& list, std::size_t index, const Value& element, const std::string& elementId)
{
    const auto current = document.get (list);
    if (! current || ! current->isList())
        return false;
    index = std::min (index, current->asList().size());
    if (document.insert (list / PathSegment::at (index), element) != Status::ok)
        return false;
    TxnRecord r;
    r.kind = TxnRecord::Kind::listInsert;
    r.path = list;
    r.after = element;
    r.elementId = elementId;
    r.index = index;
    txn.records.push_back (std::move (r));
    // A later set inside the list must not merge with a record made before this insert.
    setIndex.clear();
    return true;
}

bool TxnWriter::listRemove (const Path& list, const std::string& elementId)
{
    const auto current = document.get (list);
    if (! current || ! current->isList())
        return false;
    const auto& items = current->asList();
    const auto it = std::find_if (items.begin(), items.end(), [&] (const Value& e) { return listElementId (e) == elementId; });
    if (it == items.end())
        return true;
    const auto index = static_cast<std::size_t> (it - items.begin());
    if (document.remove (list / PathSegment::at (index)) != Status::ok)
        return false;
    TxnRecord r;
    r.kind = TxnRecord::Kind::listRemove;
    r.path = list;
    r.after = *it;
    r.elementId = elementId;
    r.index = index;
    txn.records.push_back (std::move (r));
    setIndex.clear();
    return true;
}
} // namespace weft::commands

#include "weft/engine/graph/Instrument.h"

#include <cmath>
#include <cstdio>

namespace weft::engine
{
std::string Processor::parameterText (int, double normalized) const
{
    char text[32];
    std::snprintf (text, sizeof text, "%.0f%%", normalized * 100.0);
    return text;
}

int Processor::parameterIndex (const std::string& id) const
{
    const auto params = parameters();
    for (std::size_t i = 0; i < params.size(); ++i)
        if (params[i].id == id)
            return static_cast<int> (i);
    return -1;
}

std::shared_ptr<InstrumentSlot> InstrumentRack::set (const Id& trackId, std::shared_ptr<Instrument> instrument, std::string key)
{
    if (! instrument)
    {
        slots.erase (trackId);
        return nullptr;
    }
    if (config)
        instrument->prepare (config->first, config->second);
    auto slot = std::make_shared<InstrumentSlot> (std::move (instrument));
    slot->key = std::move (key);
    slots[trackId] = slot;
    return slot;
}

std::shared_ptr<InstrumentSlot> InstrumentRack::find (const Id& trackId) const
{
    const auto it = slots.find (trackId);
    return it == slots.end() ? nullptr : it->second;
}

void InstrumentRack::prepare (double sampleRate, int maxBlockSize)
{
    config = { sampleRate, maxBlockSize };
    for (auto& [id, slot] : slots)
        slot->instrument().prepare (sampleRate, maxBlockSize);
}

void InstrumentRack::prune (const std::set<Id>& keep)
{
    std::erase_if (slots, [&] (const auto& kv) { return ! kv.second->key.empty() && ! keep.contains (kv.first); });
}

std::shared_ptr<EffectSlot> EffectRack::find (const Id& pluginId) const
{
    const auto it = slots.find (pluginId);
    return it == slots.end() ? nullptr : it->second;
}

std::shared_ptr<EffectSlot> EffectRack::set (const Id& pluginId, std::shared_ptr<Processor> processor, std::string key)
{
    if (config)
        processor->prepare (config->first, config->second);
    auto slot = std::make_shared<EffectSlot>();
    slot->processor = std::move (processor);
    slot->key = std::move (key);
    slots[pluginId] = slot;
    return slot;
}

void EffectRack::prepare (double sampleRate, int maxBlockSize)
{
    config = { sampleRate, maxBlockSize };
    for (auto& [id, slot] : slots)
        slot->processor->prepare (sampleRate, maxBlockSize);
}

void EffectRack::prune (const std::set<Id>& keep)
{
    std::erase_if (slots, [&] (const auto& kv) { return ! keep.contains (kv.first); });
}
} // namespace weft::engine

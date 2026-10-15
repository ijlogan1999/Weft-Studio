#include "weft/engine/graph/Instrument.h"

namespace weft::engine
{
void InstrumentRack::set (const Id& trackId, std::shared_ptr<Instrument> instrument)
{
    if (! instrument)
    {
        slots.erase (trackId);
        return;
    }
    if (config)
        instrument->prepare (config->first, config->second);
    slots[trackId] = std::make_shared<InstrumentSlot> (std::move (instrument));
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
} // namespace weft::engine

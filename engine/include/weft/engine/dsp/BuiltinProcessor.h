#pragma once

#include "weft/engine/graph/Instrument.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace weft::engine
{
/** How a built-in parameter maps normalized 0..1 to its plain value. */
enum class ParamScale : std::uint8_t
{
    linear,
    logarithmic, // frequencies and times
    choice       // `steps` values: 0, 1, ...
};

struct BuiltinParam
{
    std::string id, name, unit;
    double min = 0, max = 1, defaultValue = 0;
    ParamScale scale = ParamScale::linear;
    std::vector<std::string> choices; // ParamScale::choice

    double toPlain (double normalized) const noexcept;
    double toNormalized (double plain) const noexcept;
};

/**
    Base of the built-in instrument and effects (F-PLG-9, F-PLG-10): parameters held as
    atomics so the message thread can set them while the audio thread reads, and a state
    chunk that is the canonical encoding of {paramId: normalized}.
*/
class BuiltinProcessor : public Processor
{
public:
    explicit BuiltinProcessor (std::vector<BuiltinParam> params);

    std::vector<ParameterInfo> parameters() const override;
    void setParameter (int index, double normalized) noexcept override;
    double parameter (int index) const noexcept override;
    std::string parameterText (int index, double normalized) const override;
    std::vector<std::uint8_t> state() const override;
    bool setState (std::span<const std::uint8_t> bytes) override;

    /** Sets a parameter by id in its own unit (dB, Hz, ms, ...). Unknown ids are ignored. */
    void setValue (const std::string& id, double plain) noexcept;
    double value (int index) const noexcept { return defs[static_cast<std::size_t> (index)].toPlain (parameter (index)); }

protected:
    /** True once after any parameter changed: processors recompute coefficients then. */
    bool takeChanges() noexcept { return changes.exchange (false, std::memory_order_acq_rel); }

private:
    std::vector<BuiltinParam> defs;
    std::unique_ptr<std::atomic<double>[]> values;
    std::atomic<bool> changes { true };
};

/** Flushes denormals to zero in feedback paths (AT-PLG-9). */
inline float undenormal (float v) noexcept { return (v > -1.0e-15f && v < 1.0e-15f) ? 0.0f : v; }
} // namespace weft::engine

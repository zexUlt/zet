#include "zet/core/log.hpp"

namespace zet::log {
namespace {

/// One event loop on one thread, so this is a plain struct with no locking.
/// Kept behind an accessor rather than as three loose globals so that the
/// state has a name and a single point of definition.
struct State {
    // Not named Sink or MinLevel: a member would shadow the type and the free
    // function of those names inside this scope.
    Sink ActiveSink{nullptr};
    void* ActiveContext{nullptr};
    ELevel Threshold{ELevel::Info};
};

[[nodiscard]] State& Current() noexcept {
    // Constant-initialised, so no guard variable and no lazy-init machinery.
    static State state{};
    return state;
}

}  // namespace

void SetSink(Sink sink, void* context) noexcept {
    Current().ActiveSink = sink;
    Current().ActiveContext = context;
}

void SetMinLevel(ELevel level) noexcept { Current().Threshold = level; }

ELevel MinLevel() noexcept { return Current().Threshold; }

bool IsEnabled(ELevel level) noexcept {
    const State& state = Current();
    return state.ActiveSink != nullptr && level >= state.Threshold;
}

void Emit(ELevel level, std::string_view message, std::span<const Field> fields,
          Timestamp at) noexcept {
    if (!IsEnabled(level)) {
        return;
    }
    const Record record{
        .Level = level, .Message = message, .Fields = fields, .At = at};
    const State& state = Current();
    state.ActiveSink(record, state.ActiveContext);
}

}  // namespace zet::log

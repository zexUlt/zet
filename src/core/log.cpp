#include "zet/core/log.hpp"

namespace zet::log {
namespace {

/// One event loop on one thread, so this is a plain struct with no locking.
/// Kept behind an accessor rather than as three loose globals so that the
/// state has a name and a single point of definition.
struct State {
    Sink sink{nullptr};
    void* context{nullptr};
    ELevel minLevel{ELevel::Info};
};

[[nodiscard]] State& Current() noexcept {
    // Constant-initialised, so no guard variable and no lazy-init machinery.
    static State state{};
    return state;
}

}  // namespace

void SetSink(Sink sink, void* context) noexcept {
    Current().sink = sink;
    Current().context = context;
}

void SetMinLevel(ELevel level) noexcept { Current().minLevel = level; }

ELevel MinLevel() noexcept { return Current().minLevel; }

bool IsEnabled(ELevel level) noexcept {
    const State& state = Current();
    return state.sink != nullptr && level >= state.minLevel;
}

void Emit(ELevel level, std::string_view message, std::span<const Field> fields,
          Timestamp at) noexcept {
    if (!IsEnabled(level)) {
        return;
    }
    const Record record{
        .level = level, .message = message, .fields = fields, .at = at};
    const State& state = Current();
    state.sink(record, state.context);
}

}  // namespace zet::log

#include "zet/core/log.hpp"

namespace zet::log {
namespace {

Sink activeSink = nullptr;
void* activeContext = nullptr;
ELevel minLevel = ELevel::Info;

}  // namespace

void SetSink(Sink sink, void* context) noexcept {
    activeSink = sink;
    activeContext = context;
}

void SetMinLevel(ELevel level) noexcept { minLevel = level; }

ELevel MinLevel() noexcept { return minLevel; }

bool IsEnabled(ELevel level) noexcept {
    return activeSink != nullptr && level >= minLevel;
}

void Emit(ELevel level, std::string_view message, std::span<const Field> fields,
          Timestamp at) noexcept {
    if (!IsEnabled(level)) {
        return;
    }
    const Record record{
        .level = level, .message = message, .fields = fields, .at = at};
    activeSink(record, activeContext);
}

}  // namespace zet::log

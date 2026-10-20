#include "zet/session/mux_router.hpp"

#include <algorithm>
#include <iterator>

#include "zet/session/limits.hpp"

namespace zet::session {

MuxRouter::MuxRouter(MutableByteSpan pendingStorage) noexcept
    : Storage_(pendingStorage),
      // Storage short of MUX_PENDING_STORAGE means fewer slots, not a
      // buffer overrun: what does not fit is never handed out.
      Slots_(std::min(MAX_PENDING_CONNECTIONS,
                      pendingStorage.size() / MUX_PENDING_BUFFER)) {}

std::expected<void, wire::EMuxRefusal> MuxRouter::Register(
    MuxAgentId agent, Uid uid, const crypto::Key& registration) noexcept {
    // The claim is checked by what it hashes to; R itself is not kept.
    const wire::SessionId id = wire::SessionIdOf(registration);

    const auto held = std::ranges::find_if(
        Registrations_,
        [&](const Registration& r) { return r.Live && r.Id == id; });
    if (held != Registrations_.end()) {
        if (held->Owner != uid) {
            return std::unexpected(wire::EMuxRefusal::Taken);
        }
        held->Agent = agent;
        return {};
    }

    const auto owned = std::ranges::count_if(
        Registrations_,
        [&](const Registration& r) { return r.Live && r.Owner == uid; });
    if (static_cast<std::size_t>(owned) >= MAX_REGISTRATIONS_PER_UID) {
        return std::unexpected(wire::EMuxRefusal::Limit);
    }
    const auto free = std::ranges::find_if(
        Registrations_, [](const Registration& r) { return !r.Live; });
    if (free == Registrations_.end()) {
        return std::unexpected(wire::EMuxRefusal::Limit);
    }
    *free = Registration{.Id = id, .Agent = agent, .Owner = uid, .Live = true};
    ++RegistrationCount_;
    return {};
}

void MuxRouter::Unregister(MuxAgentId agent) noexcept {
    for (Registration& r : Registrations_) {
        if (r.Live && r.Agent == agent) {
            r = Registration{};
            --RegistrationCount_;
        }
    }
}

std::optional<MuxAgentId> MuxRouter::Find(
    const wire::SessionId& id) const noexcept {
    const auto held = std::ranges::find_if(
        Registrations_,
        [&](const Registration& r) { return r.Live && r.Id == id; });
    if (held == Registrations_.end()) {
        return std::nullopt;
    }
    return held->Agent;
}

std::expected<void, EMuxClose> MuxRouter::Accept(MuxConnectionId connection,
                                                 const PeerAddress& from,
                                                 Timestamp now) noexcept {
    if (FindWaiting(connection) != nullptr) {
        // A number the I/O layer already gave out and has not closed.
        return std::unexpected(EMuxClose::Unknown);
    }
    const auto fromThere = std::ranges::count_if(
        Waiting_, [&](const Waiting& w) { return w.Live && w.From == from; });
    if (static_cast<std::size_t>(fromThere) >= MAX_PENDING_PER_ADDRESS) {
        return std::unexpected(EMuxClose::Limit);
    }
    const auto usable = std::span{Waiting_}.first(Slots_);
    const auto free =
        std::ranges::find_if(usable, [](const Waiting& w) { return !w.Live; });
    if (free == usable.end()) {
        return std::unexpected(EMuxClose::Limit);
    }
    *free = Waiting{.Connection = connection,
                    .From = from,
                    .Until = now + HANDSHAKE_TIMEOUT,
                    .Length = 0,
                    .Live = true};
    ++PendingCount_;
    return {};
}

std::size_t MuxRouter::Room(MuxConnectionId connection) const noexcept {
    const Waiting* waiting = FindWaiting(connection);
    return waiting == nullptr ? 0 : MUX_PENDING_BUFFER - waiting->Length;
}

MuxDecision MuxRouter::Feed(MuxConnectionId connection,
                            ByteSpan bytes) noexcept {
    Waiting* waiting = FindWaiting(connection);
    if (waiting == nullptr) {
        return MuxClose{.Connection = connection, .Reason = EMuxClose::Unknown};
    }
    const MutableByteSpan buffer = BufferOf(*waiting);
    const std::size_t room = buffer.size() - waiting->Length;
    // More than the room cannot hold a Hello the room could not: a Hello is
    // one handshake frame, and the buffer is exactly that wide.
    const std::size_t taken = std::min(room, bytes.size());
    std::ranges::copy(bytes.first(taken),
                      buffer.subspan(waiting->Length).begin());
    waiting->Length += taken;

    const ByteSpan held = ByteSpan{buffer}.first(waiting->Length);
    const auto peek = wire::PeekHello(held);
    if (!peek) {
        if (peek.error() == EProtoError::Truncated && taken == bytes.size()) {
            return MuxNeedMore{};
        }
        Release(*waiting);
        return MuxClose{.Connection = connection,
                        .Reason = EMuxClose::Malformed};
    }

    // Released before it is reported: the prefix stays where it is until the
    // next Accept reuses the slot.
    Release(*waiting);
    if (const auto agent = Find(peek->Id)) {
        return MuxHandoff{
            .Connection = connection, .Agent = *agent, .Prefix = held};
    }
    return MuxDecoy{.Connection = connection, .Prefix = held};
}

void MuxRouter::Closed(MuxConnectionId connection) noexcept {
    if (Waiting* waiting = FindWaiting(connection)) {
        Release(*waiting);
    }
}

Deadline MuxRouter::NextDeadline() const noexcept {
    Deadline earliest;
    for (const Waiting& w : Waiting_) {
        if (w.Live) {
            earliest = Earliest(earliest, Deadline{w.Until});
        }
    }
    return earliest;
}

std::optional<MuxClose> MuxRouter::Expire(Timestamp now) noexcept {
    const auto due = std::ranges::find_if(
        Waiting_, [&](const Waiting& w) { return w.Live && w.Until <= now; });
    if (due == Waiting_.end()) {
        return std::nullopt;
    }
    const MuxConnectionId connection = due->Connection;
    Release(*due);
    return MuxClose{.Connection = connection, .Reason = EMuxClose::Expired};
}

MuxRouter::Waiting* MuxRouter::FindWaiting(
    MuxConnectionId connection) noexcept {
    const auto found = std::ranges::find_if(Waiting_, [&](const Waiting& w) {
        return w.Live && w.Connection == connection;
    });
    return found == Waiting_.end() ? nullptr : &*found;
}

const MuxRouter::Waiting* MuxRouter::FindWaiting(
    MuxConnectionId connection) const noexcept {
    const auto found = std::ranges::find_if(Waiting_, [&](const Waiting& w) {
        return w.Live && w.Connection == connection;
    });
    return found == Waiting_.end() ? nullptr : &*found;
}

MutableByteSpan MuxRouter::BufferOf(const Waiting& waiting) noexcept {
    const auto slot = static_cast<std::size_t>(
        std::distance(static_cast<const Waiting*>(Waiting_.data()), &waiting));
    return Storage_.subspan(slot * MUX_PENDING_BUFFER, MUX_PENDING_BUFFER);
}

void MuxRouter::Release(Waiting& waiting) noexcept {
    waiting.Live = false;
    --PendingCount_;
}

}  // namespace zet::session

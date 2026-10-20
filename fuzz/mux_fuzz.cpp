/// MuxRouter against a model of what it promises (design.md §6), on any
/// sequence of registrations, disconnects, connections, bytes and time.
///
/// Input: a stream of operations, one opcode byte (low four bits) and its
/// arguments each:
///
///     0  agent uid seed    Register(agent % AGENTS, uid % UIDS, R(seed))
///     1  agent             Unregister(agent % AGENTS)
///     2  conn addr         Accept(conn % CONNECTIONS, addr % ADDRESSES)
///     3  conn seed cut     Feed the Hello for R(seed)'s sid, `cut` bytes
///     4  conn len bytes…   Feed `len` raw bytes
///     5  conn              Closed(conn)
///     6  step              now += step × TIME_STEP, then Expire until done
///
/// Properties, each a trap when broken:
///  P1  a handoff goes to the agent the model holds for the Hello's sid, and
///      only if the model holds one — never by a sid whose R was not shown
///  P2  a registration of a live sid from another uid is refused
///  P3  a decoy only for a sid nobody holds
///  P4  Pending() and Registrations() agree with the model, and stay within
///      their limits
///  P5  once Expire is drained, nothing pending is past its deadline

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include "zet/core/byte_reader.hpp"
#include "zet/core/byte_writer.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/session/limits.hpp"
#include "zet/session/mux_router.hpp"
#include "zet/wire/frame.hpp"
#include "zet/wire/message.hpp"

namespace {

using namespace zet;
using namespace zet::session;

constexpr std::size_t AGENTS = 4;
constexpr std::size_t UIDS = 3;
/// Registration secrets: few enough that sids collide across agents and uids.
constexpr std::uint8_t SEEDS = 8;
/// Hello seeds go twice as far, so half of them name sids nobody can hold.
constexpr std::uint8_t HELLO_SEEDS = 2 * SEEDS;
constexpr std::size_t CONNECTIONS = 8;
constexpr std::size_t ADDRESSES = 3;
constexpr Duration TIME_STEP = std::chrono::milliseconds{250};
constexpr std::size_t MAX_OPS = 512;

crypto::Key RegistrationFor(std::uint8_t seed) noexcept {
    crypto::Key key;
    for (auto& byte : key.Expose()) {
        byte = static_cast<std::byte>(seed);
    }
    return key;
}

/// What the model holds for one registration seed.
struct Claim {
    bool Live{false};
    MuxAgentId Agent{0};
    Uid Owner{0};
};

struct Model {
    std::array<Claim, SEEDS> Claims{};
    std::array<bool, CONNECTIONS> Pending{};
    std::array<Timestamp, CONNECTIONS> Until{};

    [[nodiscard]] std::size_t Registrations() const noexcept {
        std::size_t n = 0;
        for (const Claim& c : Claims) {
            n += c.Live ? 1 : 0;
        }
        return n;
    }
    [[nodiscard]] std::size_t PendingCount() const noexcept {
        std::size_t n = 0;
        for (const bool p : Pending) {
            n += p ? 1 : 0;
        }
        return n;
    }
    /// The seed whose sid is `id`, among those that can be registered.
    [[nodiscard]] std::optional<std::uint8_t> SeedOf(
        const wire::SessionId& id) const noexcept {
        for (std::uint8_t seed = 0; seed < SEEDS; ++seed) {
            if (wire::SessionIdOf(RegistrationFor(seed)) == id) {
                return seed;
            }
        }
        return std::nullopt;
    }
};

std::array<std::byte, MUX_PENDING_STORAGE> storage{};
std::optional<MuxRouter> router;

void Check(bool holds) noexcept {
    if (!holds) {
        __builtin_trap();
    }
}

/// The frame of a Hello naming R(seed)'s sid; returns its length.
std::size_t HelloFrame(
    std::uint8_t seed,
    std::array<std::byte, wire::MAX_HANDOFF_PREFIX>& out) noexcept {
    wire::HelloMessage hello{};
    hello.Id = wire::SessionIdOf(RegistrationFor(seed));
    std::array<std::byte, wire::MAX_HANDSHAKE_FRAME> body{};
    ByteWriter bodyWriter{MutableByteSpan{body}};
    Check(wire::WriteHello(bodyWriter, hello).has_value());
    ByteWriter writer{MutableByteSpan{out}};
    Check(wire::WriteFrameHeader(
              writer, wire::FrameHeader{.Length = static_cast<std::uint32_t>(
                                            bodyWriter.Size()),
                                        .Epoch = 0})
              .has_value());
    Check(writer.WriteBytes(bodyWriter.Written()).has_value());
    return writer.Size();
}

void Judge(const MuxDecision& decision, std::size_t connection,
           Model& model) noexcept {
    if (std::holds_alternative<MuxNeedMore>(decision)) {
        Check(model.Pending[connection]);
        return;
    }
    model.Pending[connection] = false;
    if (const auto* handoff = std::get_if<MuxHandoff>(&decision)) {
        const auto peek = wire::PeekHello(handoff->Prefix);
        Check(peek.has_value());
        const auto seed = model.SeedOf(peek->Id);
        // P1: only by a sid somebody showed R for, and to its holder.
        Check(seed.has_value());
        const Claim& claim = model.Claims[*seed];
        Check(claim.Live && claim.Agent == handoff->Agent);
        return;
    }
    if (const auto* decoy = std::get_if<MuxDecoy>(&decision)) {
        const auto peek = wire::PeekHello(decoy->Prefix);
        Check(peek.has_value());
        const auto seed = model.SeedOf(peek->Id);
        // P3.
        Check(!seed.has_value() || !model.Claims[*seed].Live);
    }
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
    if (!crypto::Init()) {
        __builtin_trap();
    }
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    router.emplace(MutableByteSpan{storage});
    Model model;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    ByteReader reader{ByteSpan{reinterpret_cast<const std::byte*>(data), size}};
    Timestamp now = Timestamp::FromNanos(1);

    for (std::size_t op = 0; op < MAX_OPS && !reader.Exhausted(); ++op) {
        const auto code = reader.ReadU8();
        if (!code) {
            break;
        }
        switch (*code & 0x0FU) {
            case 0: {
                const auto agent = reader.ReadU8();
                const auto uid = reader.ReadU8();
                const auto seed = reader.ReadU8();
                if (!agent || !uid || !seed) {
                    break;
                }
                const auto a = static_cast<MuxAgentId>(*agent % AGENTS);
                const auto u = static_cast<Uid>(*uid % UIDS);
                const auto s = static_cast<std::uint8_t>(*seed % SEEDS);
                const auto result = router->Register(a, u, RegistrationFor(s));
                Claim& claim = model.Claims[s];
                if (claim.Live && claim.Owner != u) {
                    // P2.
                    Check(!result.has_value() &&
                          result.error() == wire::EMuxRefusal::Taken);
                } else {
                    Check(result.has_value());
                    claim = Claim{.Live = true, .Agent = a, .Owner = u};
                }
                break;
            }
            case 1: {
                const auto agent = reader.ReadU8();
                if (!agent) {
                    break;
                }
                const auto a = static_cast<MuxAgentId>(*agent % AGENTS);
                router->Unregister(a);
                for (Claim& claim : model.Claims) {
                    if (claim.Live && claim.Agent == a) {
                        claim = Claim{};
                    }
                }
                break;
            }
            case 2: {
                const auto conn = reader.ReadU8();
                const auto addr = reader.ReadU8();
                if (!conn || !addr) {
                    break;
                }
                const std::size_t c = *conn % CONNECTIONS;
                PeerAddress from{};
                from.back() = static_cast<std::byte>(*addr % ADDRESSES);
                const auto accepted = router->Accept(c, from, now);
                // Below every limit by construction, so only a number in use
                // is refused.
                Check(accepted.has_value() == !model.Pending[c]);
                if (accepted) {
                    model.Pending[c] = true;
                    model.Until[c] = now + HANDSHAKE_TIMEOUT;
                }
                break;
            }
            case 3: {
                const auto conn = reader.ReadU8();
                const auto seed = reader.ReadU8();
                const auto cut = reader.ReadU8();
                if (!conn || !seed || !cut) {
                    break;
                }
                const std::size_t c = *conn % CONNECTIONS;
                std::array<std::byte, wire::MAX_HANDOFF_PREFIX> frame{};
                const std::size_t length = HelloFrame(
                    static_cast<std::uint8_t>(*seed % HELLO_SEEDS), frame);
                const std::size_t take = std::min<std::size_t>(*cut, length);
                if (!model.Pending[c]) {
                    const MuxDecision decision =
                        router->Feed(c, ByteSpan{frame}.first(take));
                    Check(std::holds_alternative<MuxClose>(decision));
                    break;
                }
                Judge(router->Feed(c, ByteSpan{frame}.first(take)), c, model);
                break;
            }
            case 4: {
                const auto conn = reader.ReadU8();
                const auto length = reader.ReadU8();
                if (!conn || !length) {
                    break;
                }
                const std::size_t c = *conn % CONNECTIONS;
                const auto bytes = reader.ReadBytes(
                    std::min<std::size_t>(*length, reader.Remaining()));
                if (!bytes) {
                    break;
                }
                if (!model.Pending[c]) {
                    Check(std::holds_alternative<MuxClose>(
                        router->Feed(c, *bytes)));
                    break;
                }
                Judge(router->Feed(c, *bytes), c, model);
                break;
            }
            case 5: {
                const auto conn = reader.ReadU8();
                if (!conn) {
                    break;
                }
                const std::size_t c = *conn % CONNECTIONS;
                router->Closed(c);
                model.Pending[c] = false;
                break;
            }
            case 6: {
                const auto step = reader.ReadU8();
                if (!step) {
                    break;
                }
                now += TIME_STEP * *step;
                while (const auto closed = router->Expire(now)) {
                    const auto c = static_cast<std::size_t>(closed->Connection);
                    Check(c < CONNECTIONS && model.Pending[c] &&
                          model.Until[c] <= now &&
                          closed->Reason == EMuxClose::Expired);
                    model.Pending[c] = false;
                }
                // P5.
                for (std::size_t c = 0; c < CONNECTIONS; ++c) {
                    Check(!model.Pending[c] || model.Until[c] > now);
                }
                const Deadline next = router->NextDeadline();
                Check(next.IsSet() == (model.PendingCount() > 0));
                Check(!next.IsSet() || next.At() > now);
                break;
            }
            default:
                break;
        }
        // P4.
        Check(router->Pending() == model.PendingCount());
        Check(router->Registrations() == model.Registrations());
        Check(router->Pending() <= MAX_PENDING_CONNECTIONS);
        Check(router->Registrations() <= MAX_MUX_REGISTRATIONS);
    }
    return 0;
}

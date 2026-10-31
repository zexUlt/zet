/// Shared test helpers for grid path (A/B) and snapshot probe tests.
///
/// Included by both grid_path_property_test.cpp and
/// connection_snapshot_probe_test.cpp.  Everything lives in an anonymous
/// namespace so each translation unit gets its own copy (no ODR issue).
#pragma once

#include "zet/session/connection.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/session/agent_session.hpp"
#include "zet/session/channel.hpp"
#include "zet/session/limits.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"

namespace {

using namespace std::chrono_literals;
using namespace zet;
using namespace zet::session;
using zet::wire::DeriveSessionSecrets;
using zet::wire::HandshakeNonce;

struct SodiumFixture {
    SodiumFixture() { REQUIRE(crypto::Init()); }
};

// ── Basic helpers ─────────────────────────────────────────────────────────

crypto::Key MakeKey(std::uint8_t seed) {
    crypto::KeyBytes bytes{};
    std::uint8_t v = seed;
    for (auto& b : bytes) {
        b = std::byte{v};
        v = static_cast<std::uint8_t>(v + 7);
    }
    return crypto::Key{bytes};
}

HandshakeNonce MakeNonce(std::uint64_t counter) {
    HandshakeNonce n{};
    std::uint64_t v = counter * 0x9E3779B97F4A7C15ULL + 1;
    for (auto& b : n) {
        b = static_cast<std::byte>(v & 0xFF);
        v = v * 6364136223846793005ULL + 1442695040888963407ULL;
    }
    return n;
}

const Timestamp START = Timestamp::FromNanos(1'000'000'000);

/// Tiny limits for tests: ring overflows quickly.
SessionLimits TinyLimits() {
    return SessionLimits{
        .ResendMax = 512,
        .DisconnectMax = 512,
        .OrphanTimeout = std::chrono::minutes{30},
        .Liveness = LivenessLimits{.KeepaliveInterval = Duration{0},
                                   .PeerSilenceTimeout = Duration{0}},
    };
}

// ── One-connection harness ─────────────────────────────────────────────────

struct Space {
    std::array<std::byte, CONNECTION_FRAME_STORAGE> AgentFrames{};
    std::array<std::byte, CONNECTION_FRAME_STORAGE> ClientFrames{};
    std::array<std::byte, CONNECTION_OUT_SIZE> Out{};
    std::array<std::byte, CONNECTION_SCRATCH_SIZE> Scratch{};
    std::vector<std::byte> SnapshotBuf{wire::SNAPSHOT_MAX_BYTES, std::byte{}};
};

template <typename TConn>
[[nodiscard]] bool Feed(TConn& conn, Space& sp, std::size_t written) {
    auto r = conn.OnBytes(ByteSpan{sp.Out}.first(written));
    return r.has_value();
}

ConnectionResult PumpAgent(AgentConnection& a, Space& sp, Timestamp now) {
    return a.Pump(now, MutableByteSpan{sp.Out}, MutableByteSpan{sp.Scratch});
}
ConnectionResult PumpClient(ClientConnection& c, Space& sp, Timestamp now) {
    return c.Pump(now, MutableByteSpan{sp.Out}, MutableByteSpan{sp.Scratch});
}

bool DriveToLive(AgentConnection& agent, ClientConnection& client,
                 AgentSession& session, Space& sp, Timestamp now,
                 std::uint64_t& nonces) {
    for (int i = 0; i < 64; ++i) {
        auto ea = PumpAgent(agent, sp, now);
        if (!ea) {
            return false;
        }
        if (const auto* ns = std::get_if<NeedsSession>(&*ea)) {
            REQUIRE(ns->Id == session.Ref().Id());
            agent.Route(session.Ref(), MakeNonce(++nonces));
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            if (!Feed(client, sp, sf->Written)) {
                return false;
            }
        }

        auto ec = PumpClient(client, sp, now);
        if (!ec) {
            return false;
        }
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            if (!Feed(agent, sp, sf->Written)) {
                return false;
            }
        }

        if (agent.IsLive() && client.IsLive()) {
            return true;
        }
    }
    return false;
}

// ── Probe harness (Rig + Reconnect) ──────────────────────────────────────

struct Rig {
    SessionLimits Limits = TinyLimits();
    std::vector<std::byte> As =
        std::vector<std::byte>(Limits.DisconnectMax + 256);
    std::vector<std::byte> Cs = std::vector<std::byte>(Limits.ResendMax + 256);
    AgentSession Agent{
        DeriveSessionSecrets(MakeKey(33)),
        SessionStreams{
            .Storage = MutableByteSpan{As}, .Now = START, .Limits = Limits}};
    ClientSession Client{
        DeriveSessionSecrets(MakeKey(33)),
        SessionStreams{
            .Storage = MutableByteSpan{Cs}, .Now = START, .Limits = Limits}};
    Space Sp;
    std::uint64_t Nonces = 0;
};

[[maybe_unused]] std::string Reconnect(Rig& r, bool capable,
                                       std::uint16_t ceiling, bool close = true,
                                       bool supply = false) {
    AgentConnection ac{MutableByteSpan{r.Sp.AgentFrames}, START, ceiling};
    ClientConnection cc{r.Client,
                        MakeNonce(++r.Nonces),
                        MutableByteSpan{r.Sp.ClientFrames},
                        START,
                        capable,
                        ceiling,
                        MutableByteSpan{r.Sp.SnapshotBuf}};
    if (!DriveToLive(ac, cc, r.Agent, r.Sp, START, r.Nonces)) {
        return "drive-failed";
    }
    std::string res = "no-snapshot";
    const std::vector<std::byte> snap(16, std::byte{0x22});
    bool applied = false;
    for (int i = 0; i < 60; ++i) {
        auto ea = PumpAgent(ac, r.Sp, START);
        if (!ea) {
            res += " agent-err";
            break;
        }
        if (std::holds_alternative<NeedsSnapshot>(*ea)) {
            res = "NeedsSnapshot";
            if (!supply) {
                break;
            }
            ac.SupplySnapshot(START, ByteSpan{snap}, 0, 0);
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            if (!Feed(cc, r.Sp, sf->Written)) {
                break;
            }
        }
        auto ec = PumpClient(cc, r.Sp, START);
        if (!ec) {
            res += " client-err";
            break;
        }
        if (const auto* snap_ev = std::get_if<ApplySnapshot>(&*ec)) {
            applied = true;
            cc.ConfirmApply(snap_ev->StreamGen);
        }
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            if (!Feed(ac, r.Sp, sf->Written)) {
                break;
            }
        }
    }
    if (applied) {
        res += "+applied";
    }
    if (close) {
        ac.Close(START);
    }
    return res;
}

}  // namespace

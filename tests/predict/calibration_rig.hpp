// The rig for cursor position calibration (design.md §9, round C). It is the
// overlay rig with the client's wiring of Calibration around it, in the
// client's order: the stdout queue (overlay, agent bytes and the query, in
// queue order) is written to a grid that plays the terminal ("Real"); the
// terminal answers `CSI 6 n CSI c` from Real's cursor when the query is
// written, and the answer arrives on stdin after a delay of virtual time, mixed
// with the keystrokes the test types. "Ref" is the terminal without the overlay
// and the query, as in the overlay rig.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "overlay_rig.hpp"
#include "zet/predict/calibration.hpp"

namespace zet::predict::test {

/// How the terminal answers the query.
enum class EReply : std::uint8_t {
    Normal,   ///< `CSI r ; c R CSI ? 6 2 c`, after Delay
    Never,    ///< silence
    CprOnly,  ///< the position without the device attributes
    Da1Only,  ///< the device attributes without the position
};

struct TermModel {
    EReply Reply = EReply::Normal;
    std::chrono::milliseconds Delay{0};
};

/// How a terminal draws the three glyph classes of round C4: East Asian
/// ambiguous (A) in one or two cells, private use (P) in one or two, and
/// whether VS16 widens a text glyph that has an emoji variation sequence (E).
/// The terminal is a grid with this width policy; the mirror starts with the
/// default one and has to learn it.
inline grid::WidthPolicy Terminal(int ambiguous, int privateUse,
                                  bool vs16Widens) {
    grid::WidthPolicy p;
    p.Ambiguous = static_cast<std::uint8_t>(ambiguous);
    p.PrivateUse = static_cast<std::uint8_t>(privateUse);
    p.Vs16Widens = vs16Widens;
    return p;
}

inline std::string Cpr(int row, int col) {
    return "\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "R";
}
inline const std::string DA1 = "\x1b[?62;c";

struct CalClient {
    Client C;
    std::unique_ptr<Calibration> Cal;
    TermModel Model;
    /// How much output the wait holds (the constant outside this rig).
    std::size_t LagMax = CPR_LAG_BYTES_MAX;

    /// Every byte queued for the terminal, in queue order.
    std::string Wire;
    /// The agent's bytes alone.
    std::string AgentStream;
    /// What stdin handed on to the agent.
    std::string ToAgent;
    /// Ranges of Wire that are not the agent's: overlay and queries.
    std::vector<std::pair<std::size_t, std::size_t>> Inserts;
    /// The same for the queries alone.
    std::vector<std::pair<std::size_t, std::size_t>> QueryInserts;
    /// Virtual time each query was written.
    std::vector<Timestamp> QueryTimes;

    bool QueueRoom = true;
    /// Bytes queue but are not written (a full terminal).
    bool Blocked = false;

    CalClient(int cols, int rows, EDisplayMode mode = EDisplayMode::Always)
        : C(cols, rows, mode) {}

    Rig& R() { return C.R; }

    /// The terminal draws as `policy` says, from now on: both screens that
    /// stand for it (with and without the overlay) and every grid made for
    /// them later. Call before the first bytes.
    void SetTerminal(const grid::WidthPolicy& policy) {
        TermPolicy = policy;
        C.Real.G->SetWidthPolicy(policy);
        C.Ref.G->SetWidthPolicy(policy);
    }
    grid::WidthPolicy TermPolicy{};
    CalibrationTarget T() {
        return CalibrationTarget{C.R.G.get(), &C.R.P, &C.R.E};
    }
    [[nodiscard]] std::size_t Queries() const { return QueryInserts.size(); }
    [[nodiscard]] bool Frozen() const { return Cal && Cal->Frozen(); }
    void Before(std::string_view s) { C.Before(s); }

    /// The client starts calibration (the first EchoAck of the session).
    void Begin(bool seeded = false, bool settle = true) {
        Cal = Calibration::Create(seeded, T(), LagMax);
        REQUIRE(Cal != nullptr);
        if (settle) Settle();
    }

    // --- The stdout queue --------------------------------------------------

    enum class EKind : std::uint8_t { Overlay, Agent, Query };
    struct Segment {
        EKind Kind;
        std::string Bytes;
    };
    std::vector<Segment> Queue;

    void Emit(EKind kind, std::string_view bytes) {
        if (kind != EKind::Agent) {
            Inserts.emplace_back(Wire.size(), bytes.size());
            if (kind == EKind::Query) {
                QueryInserts.emplace_back(Wire.size(), bytes.size());
            }
        }
        Wire.append(bytes);
        Queue.push_back({kind, std::string(bytes)});
        if (!Blocked) Flush();
    }

    struct Reply {
        Timestamp At;
        std::string Bytes;
    };
    std::vector<Reply> Replies;

    void Flush() {
        auto queue = std::move(Queue);
        Queue.clear();
        for (const Segment& s : queue) {
            C.Real.Feed(s.Bytes);
            switch (s.Kind) {
                case EKind::Overlay:
                    if (Cal) Cal->OnWritten(Bytes(s.Bytes), R().Now);
                    break;
                case EKind::Agent:
                    C.Ref.Feed(s.Bytes);
                    AgentStream += s.Bytes;
                    if (Cal) Cal->OnWritten(Bytes(s.Bytes), R().Now);
                    break;
                case EKind::Query:
                    // The overlay is off the terminal when the query goes.
                    CHECK(SameScreen(*C.Real.G, *C.Ref.G));
                    QueryTimes.push_back(R().Now);
                    if (Cal) {
                        Cal->OnQueryWritten(R().Now, T());
                        Forward();
                    }
                    Answer();
                    break;
            }
        }
    }

    void Answer() {
        const std::string pos =
            Cpr(C.Real.G->CursorRow() + 1, C.Real.G->CursorCol() + 1);
        std::string reply;
        switch (Model.Reply) {
            case EReply::Normal:
                reply = pos + DA1;
                break;
            case EReply::Never:
                return;
            case EReply::CprOnly:
                reply = pos;
                break;
            case EReply::Da1Only:
                reply = DA1;
                break;
        }
        Replies.push_back({R().Now + Model.Delay, reply});
    }

    /// The terminal answers the pending query with this position (0-based)
    /// instead of its own: for the answers a model terminal cannot give. Use
    /// with `Model.Reply = EReply::Never`, after the query is written.
    void Scripted(int row, int col) {
        Replies.push_back({R().Now, Cpr(row + 1, col + 1) + DA1});
        Settle();
    }

    void Unblock() {
        Blocked = false;
        Flush();
        Settle();
    }

    // --- The client's side ---------------------------------------------------

    /// What the demultiplexer let through goes to the agent.
    void Forward() {
        if (!Cal) return;
        const ByteSpan fwd = Cal->Forwarded();
        if (!fwd.empty()) {
            const std::string s(reinterpret_cast<const char*>(fwd.data()),
                                fwd.size());
            Cal->ClearForwarded();
            ToAgent += s;
            R().Type(s);
        }
    }

    void UndoOverlay() {
        const auto b = C.O->BuildUndo(R().View());
        if (b.empty()) return;
        Emit(EKind::Overlay,
             std::string_view(reinterpret_cast<const char*>(b.data()),
                              b.size()));
        C.O->Commit(R().E);
    }

    void Refresh() {
        if (Frozen()) return;
        const auto b = C.O->BuildUpdate(R().E, R().View());
        if (b.empty()) return;
        ++C.Draws;
        Emit(EKind::Overlay,
             std::string_view(reinterpret_cast<const char*>(b.data()),
                              b.size()));
        C.O->Commit(R().E);
    }

    void MaybeQuery() {
        if (!Cal || !QueueRoom) return;
        const bool due = Cal->QueryDue(R().Now, T());
        Forward();
        if (!due) return;
        if (!Cal->BeginQuery(R().Now, T())) return;
        UndoOverlay();
        Emit(EKind::Query, std::string_view(reinterpret_cast<const char*>(
                                                Calibration::Query().data()),
                                            Calibration::QUERY_SIZE));
    }

    /// Delivers the answers that are due, asks what is due.
    void Settle() {
        for (int guard = 0; guard < 16; ++guard) {
            bool did = false;
            for (std::size_t i = 0; i < Replies.size();) {
                if (Replies[i].At <= R().Now) {
                    const std::string bytes = Replies[i].Bytes;
                    Replies.erase(Replies.begin() +
                                  static_cast<std::ptrdiff_t>(i));
                    StdinNoSettle(bytes);
                    did = true;
                } else {
                    ++i;
                }
            }
            const std::size_t before = Queries();
            MaybeQuery();
            Refresh();
            if (!did && Queries() == before) break;
        }
    }

    void StdinNoSettle(std::string_view bytes) {
        if (Cal) {
            Cal->OnInput(Bytes(bytes), R().Now, T());
            Forward();
        } else {
            ToAgent += bytes;
            R().Type(bytes);
        }
    }

    /// Bytes read from stdin: keys, answers, anything.
    void Stdin(std::string_view bytes) {
        StdinNoSettle(bytes);
        Refresh();
        Settle();
    }
    void Key(std::string_view k) { Stdin(k); }

    /// Output of the agent reaches the terminal and the mirror.
    void Agent(std::string_view bytes) {
        UndoOverlay();
        Emit(EKind::Agent, bytes);
        R().Out += bytes.size();
        if (Cal) {
            Cal->OnTerminalBytes(Bytes(bytes), R().Out, R().Now, T());
        } else {
            R().P.Feed(Bytes(bytes).data(), bytes.size(), *R().G);
            R().E.OnOutput(R().Out, R().Now, R().View());
        }
        Refresh();
        Settle();
    }

    void Ack() {
        const EchoAck ack{R().Gen, R().Out, R().In};
        if (Cal) {
            Cal->OnEchoAck(ack, R().Now, T());
        } else {
            R().E.OnEchoAck(ack, R().Now, R().View());
        }
        Refresh();
        Settle();
    }

    /// Virtual time passes.
    void Tick(Duration d) {
        R().Now += d;
        if (Cal) {
            Cal->OnTick(R().Now, T());
            Forward();
        }
        R().E.OnTick(R().Now);
        Refresh();
        Settle();
    }

    void Resize(std::uint16_t cols, std::uint16_t rows) {
        if (C.O->UndoSurvivesResize(R().View(), cols)) {
            UndoOverlay();
        } else {
            C.O->Forget();
            R().E.ClearDrawn();
        }
        if (Cal) {
            Cal->OnResize(R().Now, T());
            Forward();
        }
        REQUIRE(C.Real.G->Resize(cols, rows).has_value());
        REQUIRE(C.Ref.G->Resize(cols, rows).has_value());
        REQUIRE(R().G->Resize(cols, rows).has_value());
        R().E.OnResize(R().Now, R().View());
        Settle();
    }

    /// A path-B snapshot: the terminal and the mirror are what `bytes` makes
    /// of a fresh grid.
    void Snapshot(std::string_view bytes) {
        UndoOverlay();
        const int cols = R().G->Cols();
        const int rows = R().G->Rows();
        C.Real.G = MakeGrid(cols, rows);
        C.Real.G->SetWidthPolicy(TermPolicy);
        C.Real.P.Reset();
        C.Ref.G = MakeGrid(cols, rows);
        C.Ref.G->SetWidthPolicy(TermPolicy);
        C.Ref.P.Reset();
        Emit(EKind::Agent, bytes);
        R().G = MakeGrid(cols, rows);
        R().P.Reset();
        R().P.Feed(Bytes(bytes).data(), bytes.size(), *R().G);
        ++R().Gen;
        R().Out = 0;
        if (Cal) Cal->OnSnapshot(R().Now, T());
        R().E.OnSnapshot(R().Gen, R().Now, R().View());
        Settle();
    }

    [[nodiscard]] std::string WireWithoutInserts() const {
        std::string out;
        std::size_t at = 0;
        for (const auto& [from, len] : Inserts) {
            out.append(Wire, at, from - at);
            at = from + len;
        }
        out.append(Wire, at, std::string::npos);
        return out;
    }
};

}  // namespace zet::predict::test

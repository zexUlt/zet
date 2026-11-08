// Fuzz target for the prediction engine (design.md §16).
//
// Feeds the engine, interleaved: chunks of agent output (through a real mirror
// grid and parser), keystrokes, EchoAcks (current, early, stale, of another
// generation), resizes, snapshots that replace the mirror object, SRTT
// samples, drawn marks and the passage of time.
//
// The terminal-grid assertions of §16 (the overlay leaves a second grid equal
// to the mirror, inserts start in Ground, no DECSC) need the overlay builder,
// which arrives with the overlay in step 5 and extends this target then. What
// the engine alone owes is asserted after every step:
//   1. at most PREDICTIONS_MAX predictions, memory bounded by construction
//   2. a visible prediction belongs to an epoch not above confirmed_epoch
//   3. epochs: 1 <= prediction's <= current; confirmed <= current; neither the
//      current nor the confirmed epoch ever goes back
//   4. every prediction and the predicted cursor lie in the mirror's cursor
//      row, on the main screen, inside the grid
//   5. one prediction per cell; input ends do not decrease along the list
//   6. glitch count bounded; `never` predicts nothing
//   7. the engine does not touch the mirror (cells and cursor unchanged by
//      engine calls: checked by hashing around a call that only reads)
//
// Input: 3 header bytes (columns, rows, mode), then operations: one byte
// selecting the operation, one byte giving the operand length, the operand.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "zet/core/time.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"
#include "zet/predict/engine.hpp"

using namespace zet;
using namespace zet::predict;

namespace {

struct Mirror {
    std::unique_ptr<grid::Grid> G;
    grid::VtParser P;

    Mirror(std::uint16_t cols, std::uint16_t rows) {
        auto r = grid::Grid::Create(cols, rows, 0);
        if (r.has_value()) G.reset(r.value());
    }

    void Feed(const std::uint8_t* d, std::size_t n) {
        if (n == 0) return;
        P.Feed(reinterpret_cast<const std::byte*>(d), n, *G);
    }
    MirrorView View() { return MirrorView{*G, P}; }
};

[[noreturn]] void Fail() { __builtin_trap(); }

void Check(bool ok) {
    if (!ok) Fail();
}

struct Seen {
    std::uint32_t Epoch{0};
    std::uint32_t Confirmed{0};
};

void CheckInvariants(const PredictionEngine& e, const grid::Grid& g,
                     Seen& seen) {
    const auto preds = e.Predictions();
    Check(preds.size() <= PREDICTIONS_MAX);
    if (e.Mode() == EDisplayMode::Never) Check(preds.empty());

    Check(e.PredictionEpoch() >= seen.Epoch);
    Check(e.ConfirmedEpoch() >= seen.Confirmed);
    Check(e.ConfirmedEpoch() <= e.PredictionEpoch());
    seen.Epoch = e.PredictionEpoch();
    seen.Confirmed = e.ConfirmedEpoch();
    Check(e.GlitchTrigger() <= 2 * GLITCH_REPAIR_COUNT);

    std::uint64_t lastEnd = 0;
    for (std::size_t i = 0; i < preds.size(); ++i) {
        const Prediction& p = preds[i];
        if (e.IsVisible(p)) Check(p.Epoch <= e.ConfirmedEpoch());
        Check(p.Epoch >= 1 && p.Epoch <= e.PredictionEpoch());
        Check(!g.IsAltScreen());
        Check(p.Row == g.CursorRow());
        Check(p.Row < g.Rows() && p.Col < g.Cols());
        Check(p.InputEnd >= lastEnd);
        lastEnd = p.InputEnd;
        for (std::size_t j = i + 1; j < preds.size(); ++j) {
            Check(preds[j].Row != p.Row || preds[j].Col != p.Col);
        }
    }
    if (const auto& c = e.Cursor()) {
        Check(c->Epoch >= 1 && c->Epoch <= e.PredictionEpoch());
        Check(c->Row == g.CursorRow());
        Check(c->Row < g.Rows() && c->Col < g.Cols());
    }
}

std::uint64_t Hash(const grid::Grid& g) {
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
    mix(g.CursorRow());
    mix(g.CursorCol());
    mix(g.PendingWrap());
    for (std::uint16_t r = 0; r < g.Rows(); ++r) {
        for (std::uint16_t c = 0; c < g.Cols(); ++c) {
            const grid::Cell& cell = g.CellAt(r, c);
            mix(cell.Codepoint);
            mix(cell.Width);
            mix(cell.Attrs);
        }
    }
    return h;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    if (size < 3) return 0;
    const auto cols = static_cast<std::uint16_t>(data[0] % 60 + 2);
    const auto rows = static_cast<std::uint16_t>(data[1] % 10 + 1);
    const auto mode = static_cast<EDisplayMode>(data[2] % 3);

    auto mirror = std::make_unique<Mirror>(cols, rows);
    if (!mirror->G) return 0;

    Timestamp now = Timestamp::FromNanos(1'000'000'000);
    PredictionEngine engine(mirror->View(), mode);
    std::uint64_t in = 0;
    std::uint64_t out = 0;
    std::uint32_t gen = 0;
    Seen seen;

    std::size_t i = 3;
    while (i + 1 < size) {
        const std::uint8_t op = data[i] % 9;
        const std::size_t want = data[i + 1] % 24;
        i += 2;
        const std::size_t n = want < size - i ? want : size - i;
        const std::uint8_t* p = data + i;
        i += n;
        auto arg = [&](std::size_t k) -> std::uint8_t {
            return k < n ? p[k] : 0;
        };

        switch (op) {
            case 0:  // agent output
                mirror->Feed(p, n);
                out += n;
                engine.OnOutput(out, now, mirror->View());
                break;
            case 1:  // keystrokes
                in += n;
                engine.OnInput({reinterpret_cast<const std::byte*>(p), n}, in,
                               now, mirror->View());
                break;
            case 2: {  // EchoAck: current, early, short on input, other gen
                const std::uint64_t dO = arg(0) % 8;
                const std::uint64_t dI = arg(1) % 8;
                EchoAck ack;
                ack.StreamGen = engine.StreamGen();
                if (arg(2) % 16 == 0) ack.StreamGen += 1;
                if (arg(2) % 16 == 1 && ack.StreamGen > 0) ack.StreamGen -= 1;
                ack.OutputOffset = (arg(0) & 0x80U) != 0
                                       ? out + dO
                                       : (out >= dO ? out - dO : 0);
                ack.InputOffset = in >= dI ? in - dI : 0;
                engine.OnEchoAck(ack, now, mirror->View());
                break;
            }
            case 3: {  // the local terminal is resized
                const auto c = static_cast<std::uint16_t>(arg(0) % 60 + 1);
                const auto r = static_cast<std::uint16_t>(arg(1) % 10 + 1);
                if (mirror->G->Resize(c, r).has_value()) {
                    engine.OnResize(now, mirror->View());
                }
                break;
            }
            case 4: {  // a snapshot: a new mirror object, filled, applied
                auto fresh = std::make_unique<Mirror>(
                    static_cast<std::uint16_t>(arg(0) % 60 + 2),
                    static_cast<std::uint16_t>(arg(1) % 10 + 1));
                if (!fresh->G) break;
                fresh->Feed(p + (n > 2 ? 2 : n), n > 2 ? n - 2 : 0);
                mirror = std::move(fresh);
                ++gen;
                out = 0;
                engine.OnSnapshot(gen, now, mirror->View());
                break;
            }
            case 5:  // time passes
                now += std::chrono::milliseconds(arg(0) * 8 + arg(1) * 100);
                engine.OnTick(now);
                break;
            case 6:  // an RTT sample
                engine.OnSrtt(std::chrono::milliseconds(arg(0) * 2), now);
                break;
            case 7: {  // the overlay builder drew or erased something
                const auto count = engine.Predictions().size();
                if (count == 0) {
                    engine.ClearDrawn();
                } else if (arg(1) % 4 == 0) {
                    engine.ClearDrawn();
                } else {
                    engine.SetDrawn(arg(0) % count, arg(1) % 2 == 0);
                }
                break;
            }
            default:  // another look at the clock
                engine.OnTick(now);
                break;
        }
        CheckInvariants(engine, *mirror->G, seen);
    }

    // The engine reads the mirror and never writes it: a call that only reads
    // leaves every cell and the cursor as it was.
    const std::uint64_t before = Hash(*mirror->G);
    engine.OnTick(now);
    engine.OnInput({}, in, now, mirror->View());
    Check(Hash(*mirror->G) == before);
    return 0;
}

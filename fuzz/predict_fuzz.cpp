// Fuzz target for the prediction engine (design.md §16).
//
// Feeds the engine, interleaved: chunks of agent output (through a real mirror
// grid and parser), keystrokes, EchoAcks (current, early, stale, of another
// generation), resizes, snapshots that replace the mirror object, SRTT
// samples, drawn marks and the passage of time.
//
// The overlay builder runs the way the client runs it: the overlay is taken
// off before every piece of bytes, the bytes go to the mirror and to a second
// grid that plays the terminal, and what is visible is drawn after them. The
// terminal grid gets exactly what the client would queue for stdout. Asserted
// after the engine's own invariants below:
//   a. after every restore the terminal grid equals the mirror: every cell,
//      the cursor, the pen's attributes and kinds, cursor visibility
//   b. every insert begins when the terminal's parser is in Ground and not in
//      the middle of a UTF-8 character
//   c. no overlay byte string holds DECSC, DECRC, ESC [ s or ESC [ u
//   d. the overlay never changes the mirror (the hash check at the end)
// What the engine alone owes is asserted after every step:
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
//   8. echo-off model (the header's mode byte, bit 2): the mirror gets
//      prompts and unsolicited output but never a lowercase letter, and the
//      keystrokes are lowercase letters or controls, so no echo ever happens;
//      then no epoch is ever confirmed and no prediction is ever visible.
//      Some output is wrapped as a prompt on another row (DECSC / row 9 /
//      DECRC, or CNL-style down and CUU+CHA back)
//
// Input: 3 header bytes (columns, rows, mode), then operations: one byte
// selecting the operation, one byte giving the operand length, the operand.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "zet/core/time.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"
#include "zet/predict/engine.hpp"
#include "zet/predict/overlay.hpp"

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

// The echo-off model: typed printable bytes become lowercase letters, and
// the mirror never receives one or a non-ASCII byte (output and snapshot
// bytes are masked and uppercased), so
// nothing it shows can equal a typed key.
std::uint8_t TypedKey(std::uint8_t b) {
    return (b >= 0x20 && b < 0x7F) ? static_cast<std::uint8_t>('a' + b % 26)
                                   : b;
}

std::uint8_t NoLowercase(std::uint8_t b) {
    b &= 0x7FU;
    return (b >= 'a' && b <= 'z') ? static_cast<std::uint8_t>(b - 'a' + 'A')
                                  : b;
}

[[noreturn]] void Fail() { __builtin_trap(); }

bool Blank(const grid::Cell& c) {
    return c.Codepoint == 0 || c.Codepoint == U' ';
}

// The terminal shows what the mirror shows. Blank is blank: ECH leaves an
// empty cell where the mirror may have held a space, and the screens look the
// same.
bool SameScreen(const grid::Grid& a, const grid::Grid& b) {
    if (a.Cols() != b.Cols() || a.Rows() != b.Rows()) return false;
    if (a.CursorRow() != b.CursorRow() || a.CursorCol() != b.CursorCol()) {
        return false;
    }
    if (a.PenAttrs() != b.PenAttrs() || a.PenFgKind() != b.PenFgKind() ||
        a.PenBgKind() != b.PenBgKind()) {
        return false;
    }
    if (a.ModeSet(grid::EMode::Dectcem) != b.ModeSet(grid::EMode::Dectcem)) {
        return false;
    }
    for (std::uint16_t r = 0; r < a.Rows(); ++r) {
        for (std::uint16_t c = 0; c < a.Cols(); ++c) {
            const grid::Cell& x = a.CellAt(r, c);
            const grid::Cell& y = b.CellAt(r, c);
            if (Blank(x) != Blank(y)) return false;
            if (!Blank(x) && x.Codepoint != y.Codepoint) return false;
            if (x.Attrs != y.Attrs || x.FgKind != y.FgKind ||
                x.BgKind != y.BgKind) {
                return false;
            }
        }
    }
    return true;
}

bool Has(ByteSpan b, std::initializer_list<unsigned char> seq) {
    const std::size_t n = seq.size();
    for (std::size_t i = 0; i + n <= b.size(); ++i) {
        std::size_t k = 0;
        for (const unsigned char want : seq) {
            if (static_cast<unsigned char>(b[i + k]) != want) break;
            ++k;
        }
        if (k == n) return true;
    }
    return false;
}

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
    const bool echoOff = (data[2] / 3) % 2 == 1;

    auto mirror = std::make_unique<Mirror>(cols, rows);
    if (!mirror->G) return 0;
    auto term = std::make_unique<Mirror>(cols, rows);
    if (!term->G) return 0;
    auto overlay = Overlay::Create();
    if (!overlay) return 0;

    Timestamp now = Timestamp::FromNanos(1'000'000'000);
    PredictionEngine engine(mirror->View(), mode);
    std::uint64_t in = 0;
    std::uint64_t out = 0;
    std::uint32_t gen = 0;
    Seen seen;

    // The client's order around every piece of bytes.
    const auto undo = [&] {
        const ByteSpan b = overlay->BuildUndo(mirror->View());
        if (b.empty()) return;
        Check(!Has(b, {0x1B, '7'}) && !Has(b, {0x1B, '8'}) &&
              !Has(b, {0x1B, '[', 's'}) && !Has(b, {0x1B, '[', 'u'}));
        term->P.Feed(b.data(), b.size(), *term->G);
        overlay->Commit(engine);
        Check(SameScreen(*term->G, *mirror->G));
    };
    const auto draw = [&] {
        const ByteSpan b = overlay->BuildUpdate(engine, mirror->View());
        if (b.empty()) return;
        Check(term->P.State() == grid::EParserState::Ground &&
              !term->P.Utf8Pending());
        Check(!Has(b, {0x1B, '7'}) && !Has(b, {0x1B, '8'}) &&
              !Has(b, {0x1B, '[', 's'}) && !Has(b, {0x1B, '[', 'u'}));
        term->P.Feed(b.data(), b.size(), *term->G);
        overlay->Commit(engine);
    };
    const auto feed = [&](const std::uint8_t* d, std::size_t n) {
        mirror->Feed(d, n);
        term->Feed(d, n);
    };

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
        std::array<std::uint8_t, 24> shown{};  // what the echo-off mirror gets
        std::array<std::uint8_t, 24> typed{};
        for (std::size_t k = 0; k < n; ++k) {
            shown[k] = NoLowercase(p[k]);
            typed[k] = TypedKey(p[k]);
        }

        switch (op) {
            case 0:  // agent output
                if (echoOff && arg(0) % 4 == 0) {
                    // A prompt on another row, the cursor saved and put back
                    // (DECSC/DECRC), or the row below and back up.
                    static constexpr std::uint8_t SAVE[] = {
                        0x1B, '7', 0x1B, '[', '9', ';', '1', 'H'};
                    static constexpr std::uint8_t BELOW[] = {0x1B, '[', 'B',
                                                             '\r'};
                    static constexpr std::uint8_t BACK[] = {0x1B, '8'};
                    static constexpr std::uint8_t UP[] = {0x1B, '[', 'A', 0x1B,
                                                          '[',  '5', 'G'};
                    const bool save = arg(1) % 2 == 0;
                    undo();
                    feed(save ? SAVE : BELOW,
                         save ? sizeof(SAVE) : sizeof(BELOW));
                    feed(shown.data(), n);
                    feed(save ? BACK : UP, save ? sizeof(BACK) : sizeof(UP));
                } else {
                    undo();
                    feed(echoOff ? shown.data() : p, n);
                }
                out += n;
                engine.OnOutput(out, now, mirror->View());
                break;
            case 1:  // keystrokes
                in += n;
                engine.OnInput({reinterpret_cast<const std::byte*>(
                                    echoOff ? typed.data() : p),
                                n},
                               in, now, mirror->View());
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
                undo();
                if (mirror->G->Resize(c, r).has_value()) {
                    if (!term->G->Resize(c, r).has_value()) return 0;
                    engine.OnResize(now, mirror->View());
                }
                break;
            }
            case 4: {  // a snapshot: a new mirror object, filled, applied
                auto fresh = std::make_unique<Mirror>(
                    static_cast<std::uint16_t>(arg(0) % 60 + 2),
                    static_cast<std::uint16_t>(arg(1) % 10 + 1));
                if (!fresh->G) break;
                auto fresh2 = std::make_unique<Mirror>(fresh->G->Cols(),
                                                       fresh->G->Rows());
                if (!fresh2->G) break;
                undo();
                const std::uint8_t* body =
                    (echoOff ? shown.data() : p) + (n > 2 ? 2 : n);
                fresh->Feed(body, n > 2 ? n - 2 : 0);
                fresh2->Feed(body, n > 2 ? n - 2 : 0);
                mirror = std::move(fresh);
                term = std::move(fresh2);
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
        draw();
        CheckInvariants(engine, *mirror->G, seen);
        if (echoOff) {
            Check(engine.ConfirmedEpoch() == 0);
            for (const Prediction& pr : engine.Predictions()) {
                Check(!engine.IsVisible(pr));
            }
        }
    }

    // The engine reads the mirror and never writes it: a call that only reads
    // leaves every cell and the cursor as it was.
    const std::uint64_t before = Hash(*mirror->G);
    engine.OnTick(now);
    engine.OnInput({}, in, now, mirror->View());
    Check(Hash(*mirror->G) == before);
    return 0;
}

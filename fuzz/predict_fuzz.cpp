// Fuzz target for the prediction engine (design.md §16).
//
// Feeds the engine, interleaved: chunks of agent output (through a real mirror
// grid and parser), keystrokes, EchoAcks (current, early, stale, of another
// generation), resizes, snapshots that replace the mirror object, SRTT
// samples, drawn marks and the passage of time.
//
// The overlay builder runs the way the client runs it: the overlay is taken
// off before every piece of bytes, the bytes go to the mirror and to a grid
// that plays the terminal ("Real"), and what is visible is drawn after them.
// Real gets exactly what the client would queue for stdout. A third grid
// ("Ref") gets the agent's bytes only: the terminal as it would be without
// the overlay. Real and Ref start with the same content, which the mirror does
// not have: the local shell's lines above row N, the cursor on row N (the
// header's bytes say what and where), and the mirror starts blank at row 0 as
// the client creates it. A snapshot clears all three first, as its preamble
// does. Asserted after the engine's own invariants below:
//   a. after every restore Real equals Ref: every cell, the cursor, the pen's
//      attributes and kinds, cursor visibility, the character REP repeats
//   b. every insert begins when Real's parser is in Ground and not in the
//      middle of a UTF-8 character
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
// Calibration (round C3). When data[0] / 60 is 1..4 and the mode is not
// `never`, the client's Calibration runs as in the client, and the terminal
// ("Real") answers its query `CSI 6 n CSI c`: data[0] / 60 says how (1 both
// replies, 2 silence, 3 position only, 4 device attributes only), data[1] / 100
// after how long (0, 300 ms or 1500 ms of time the target steps through in
// pieces, as the client's timer does). Replies come in with the keystrokes, in
// one piece or two. In this mode keystrokes hold no ESC, so what the demux
// let through to the agent can be compared with what the user typed:
//   9. when the terminal answers both replies, the bytes forwarded to the
//      agent are exactly the bytes typed, always (no reply leaks)
//
// Input: 3 header bytes (columns, rows, mode; what is left of them says the
// pre-zet row N and the text above it), then operations: one byte selecting the
// operation, one byte giving the operand length, the operand.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "zet/core/time.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"
#include "zet/predict/calibration.hpp"
#include "zet/predict/engine.hpp"
#include "zet/predict/limits.hpp"
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

[[noreturn]] void Fail(int line) {
    std::fprintf(stderr, "predict_fuzz: invariant failed at line %d\n", line);
    std::abort();
}

bool Blank(const grid::Cell& c) {
    return c.Codepoint == 0 || c.Codepoint == U' ';
}

// The terminal shows what it would without the overlay. Blank is blank: ECH
// leaves an empty cell where the mirror may have held a space, and the screens
// look the same.
bool SameScreen(const grid::Grid& a, const grid::Grid& b) {
    if (a.Cols() != b.Cols() || a.Rows() != b.Rows()) return false;
    if (a.CursorRow() != b.CursorRow() || a.CursorCol() != b.CursorCol()) {
        return false;
    }
    if (a.PenAttrs() != b.PenAttrs() || a.PenFgKind() != b.PenFgKind() ||
        a.PenBgKind() != b.PenBgKind() ||
        a.LastPrinted().Codepoint != b.LastPrinted().Codepoint) {
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

// What the terminal showed before zet started: the shell's lines on the rows
// above `row`, the cursor on `row` at column 0 (the cursor row and below are
// blank). The text comes from `seed`; a few lines hold a wide or an ambiguous
// glyph.
void PreZet(Mirror& t, std::uint16_t row, std::uint32_t seed) {
    static constexpr const char* PIECES[] = {
        "ls", "$ ", "file", " ", "x", "\xe4\xb8\xad", "\xc2\xb1", "Z"};
    for (std::uint16_t r = 0; r < row; ++r) {
        std::uint16_t used = 0;
        for (int k = 0; k < 24; ++k) {
            seed = seed * 1664525U + 1013904223U;
            const char* piece = PIECES[(seed >> 16) % 8];
            std::size_t len = 0;
            while (piece[len] != 0) ++len;
            if (used + len + 3 >= t.G->Cols()) break;
            t.Feed(reinterpret_cast<const std::uint8_t*>(piece), len);
            used = static_cast<std::uint16_t>(used + len);
        }
        static constexpr std::uint8_t CRLF[] = {'\r', '\n'};
        t.Feed(CRLF, 2);
    }
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

void Check(bool ok, int line = __builtin_LINE()) {
    if (!ok) Fail(line);
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
    const int replyMode = mode == EDisplayMode::Never ? 0 : data[0] / 60 % 5;
    const bool calibrated = replyMode != 0;
    static constexpr std::chrono::milliseconds DELAYS[] = {
        std::chrono::milliseconds(0), std::chrono::milliseconds(300),
        std::chrono::milliseconds(1500)};
    const auto replyDelay = DELAYS[data[1] / 100 % 3];

    auto mirror = std::make_unique<Mirror>(cols, rows);
    if (!mirror->G) return 0;
    auto term = std::make_unique<Mirror>(cols, rows);  // Real
    if (!term->G) return 0;
    auto ref = std::make_unique<Mirror>(cols, rows);
    if (!ref->G) return 0;
    {
        const auto preRow = static_cast<std::uint16_t>((data[1] / 10) % rows);
        PreZet(*term, preRow, data[2] / 6U);
        PreZet(*ref, preRow, data[2] / 6U);
    }
    auto overlay = Overlay::Create();
    if (!overlay) return 0;

    Timestamp now = Timestamp::FromNanos(1'000'000'000);
    PredictionEngine engine(mirror->View(), mode);
    std::uint64_t in = 0;
    std::uint64_t out = 0;
    std::uint32_t gen = 0;
    Seen seen;

    std::unique_ptr<Calibration> cal;
    std::string typedAll;      // what the user typed
    std::string forwardedAll;  // what reached the agent
    struct Reply {
        Timestamp At;
        std::string Bytes;
    };
    std::vector<Reply> replies;
    const auto target = [&] {
        return CalibrationTarget{mirror->G.get(), &mirror->P, &engine};
    };
    if (calibrated) {
        cal = Calibration::Create(false, target());
        if (!cal) return 0;
    }
    // Stdin bytes the demux let through go to the engine, as the client's
    // FeedStdin does after the session write.
    const auto forward = [&] {
        if (!cal) return;
        const ByteSpan f = cal->Forwarded();
        if (f.empty()) return;
        forwardedAll.append(reinterpret_cast<const char*>(f.data()), f.size());
        in += f.size();
        engine.OnInput(f, in, now, mirror->View());
        cal->ClearForwarded();
    };

    // The client's order around every piece of bytes.
    const auto undo = [&] {
        const ByteSpan b = overlay->BuildUndo(mirror->View());
        if (b.empty()) return;
        Check(!Has(b, {0x1B, '7'}) && !Has(b, {0x1B, '8'}) &&
              !Has(b, {0x1B, '[', 's'}) && !Has(b, {0x1B, '[', 'u'}));
        term->P.Feed(b.data(), b.size(), *term->G);
        if (cal) cal->OnWritten(b, now);
        overlay->Commit(engine);
        Check(SameScreen(*term->G, *ref->G));
    };
    const auto draw = [&] {
        if (cal && cal->Frozen()) return;
        const ByteSpan b = overlay->BuildUpdate(engine, mirror->View());
        if (b.empty()) return;
        Check(term->P.State() == grid::EParserState::Ground &&
              !term->P.Utf8Pending());
        Check(!Has(b, {0x1B, '7'}) && !Has(b, {0x1B, '8'}) &&
              !Has(b, {0x1B, '[', 's'}) && !Has(b, {0x1B, '[', 'u'}));
        term->P.Feed(b.data(), b.size(), *term->G);
        if (cal) cal->OnWritten(b, now);
        overlay->Commit(engine);
    };
    // Bytes of the agent: the terminals get them at once, the mirror gets them
    // together at the end of the piece, through the calibration (which may hold
    // them back) or straight in.
    std::array<std::uint8_t, 64> piece{};
    std::size_t pieceLen = 0;
    const auto feed = [&](const std::uint8_t* d, std::size_t n) {
        term->Feed(d, n);
        ref->Feed(d, n);
        for (std::size_t k = 0; k < n && pieceLen < piece.size(); ++k) {
            piece[pieceLen++] = d[k];
        }
    };
    // `agentBytes`: how much of the agent's stream the piece counts for.
    const auto deliver = [&](std::size_t agentBytes) {
        const std::byte* d = reinterpret_cast<const std::byte*>(piece.data());
        if (cal) {
            cal->OnTerminalBytes({d, pieceLen}, out + agentBytes, now,
                                 target());
            cal->OnWritten({d, pieceLen}, now);
        } else {
            mirror->Feed(piece.data(), pieceLen);
        }
        pieceLen = 0;
        out += agentBytes;
        if (!cal) engine.OnOutput(out, now, mirror->View());
    };

    // The query is written (the overlay off first, as the client does) and the
    // terminal answers from its cursor. The replies that are due go to stdin.
    const auto maybeQuery = [&] {
        if (!cal) return;
        const bool due = cal->QueryDue(now, target());
        forward();
        if (!due || !cal->BeginQuery(now, target())) return;
        undo();
        term->P.Feed(Calibration::Query().data(), Calibration::QUERY_SIZE,
                     *term->G);
        const std::string pos = "\x1b[" +
                                std::to_string(term->G->CursorRow() + 1) + ";" +
                                std::to_string(term->G->CursorCol() + 1) + "R";
        const std::string da1 = "\x1b[?62;c";
        std::string reply;
        switch (replyMode) {
            case 1:
                reply = pos + da1;
                break;
            case 3:
                reply = pos;
                break;
            case 4:
                reply = da1;
                break;
            default:
                break;
        }
        if (!reply.empty()) replies.push_back({now + replyDelay, reply});
        cal->OnQueryWritten(now, target());
    };
    const auto stdinBytes = [&](std::string_view bytes) {
        if (bytes.empty()) return;
        cal->OnInput(
            {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()},
            now, target());
        forward();
    };
    // Answers that are due arrive, whole or in two reads; queries that are due
    // are asked. Repeats until nothing moves.
    const auto settle = [&] {
        if (!cal) return;
        for (int guard = 0; guard < 16; ++guard) {
            bool did = false;
            for (std::size_t k = 0; k < replies.size();) {
                if (replies[k].At <= now) {
                    const std::string r = replies[k].Bytes;
                    replies.erase(replies.begin() +
                                  static_cast<std::ptrdiff_t>(k));
                    const std::size_t cut = r.size() > 3 ? r.size() / 2 : 0;
                    stdinBytes(std::string_view(r).substr(0, cut));
                    stdinBytes(std::string_view(r).substr(cut));
                    did = true;
                } else {
                    ++k;
                }
            }
            const std::size_t before = replies.size();
            maybeQuery();
            if (!did && replies.size() == before) break;
        }
    };
    // Time passes in steps no longer than the client's timer would let it go
    // without a tick.
    const auto advance = [&](std::chrono::milliseconds span) {
        while (span.count() > 0) {
            const auto step = span < std::chrono::milliseconds(250)
                                  ? span
                                  : std::chrono::milliseconds(250);
            span -= step;
            now += step;
            if (cal) {
                cal->OnTick(now, target());
                forward();
            }
            engine.OnTick(now);
            settle();
        }
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
        std::array<std::uint8_t, 24> plain{};  // what the echo-on mirror gets
        for (std::size_t k = 0; k < n; ++k) {
            shown[k] = NoLowercase(p[k]);
            typed[k] = TypedKey(p[k]);
            plain[k] = p[k];
            if (calibrated) {
                // The program asks no queries of its own (CSI c, CSI 6 n,
                // ESC Z): a reply to one is rightly the agent's, and the
                // no-leak invariant would not hold.
                if (plain[k] == 'c' || plain[k] == 'n' || plain[k] == 'Z') {
                    plain[k] = '.';
                }
                if (shown[k] == 'Z') shown[k] = '.';
            }
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
                    feed(echoOff ? shown.data() : plain.data(), n);
                }
                deliver(n);
                break;
            case 1: {  // keystrokes
                std::array<std::uint8_t, 24> keys = echoOff ? typed : plain;
                if (cal) {
                    // No ESC among them: what the demux let through must be
                    // exactly what was typed.
                    for (std::size_t k = 0; k < n; ++k) {
                        if (keys[k] == 0x1B) keys[k] = 'e';
                    }
                    typedAll.append(reinterpret_cast<const char*>(keys.data()),
                                    n);
                    stdinBytes(std::string_view(
                        reinterpret_cast<const char*>(keys.data()), n));
                } else {
                    in += n;
                    engine.OnInput(
                        {reinterpret_cast<const std::byte*>(keys.data()), n},
                        in, now, mirror->View());
                }
                break;
            }
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
                if (cal) {
                    cal->OnEchoAck(ack, now, target());
                } else {
                    engine.OnEchoAck(ack, now, mirror->View());
                }
                break;
            }
            case 3: {  // the local terminal is resized
                const auto c = static_cast<std::uint16_t>(arg(0) % 60 + 1);
                const auto r = static_cast<std::uint16_t>(arg(1) % 10 + 1);
                undo();
                if (cal) {
                    cal->OnResize(now, target());
                    forward();
                }
                if (mirror->G->Resize(c, r).has_value()) {
                    if (!term->G->Resize(c, r).has_value()) return 0;
                    if (!ref->G->Resize(c, r).has_value()) return 0;
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
                auto fresh3 = std::make_unique<Mirror>(fresh->G->Cols(),
                                                       fresh->G->Rows());
                if (!fresh3->G) break;
                undo();
                // The snapshot's preamble: leave the alternate screen, clear,
                // home. The terminal gets it with the rest.
                static constexpr std::uint8_t PREAMBLE[] = {
                    0x1B, '[', '?', '1', '0',  '4', '9', 'l',
                    0x1B, '[', '2', 'J', 0x1B, '[', 'H'};
                const std::uint8_t* body =
                    (echoOff ? shown.data() : plain.data()) + (n > 2 ? 2 : n);
                for (Mirror* m : {fresh.get(), fresh2.get(), fresh3.get()}) {
                    m->Feed(PREAMBLE, sizeof(PREAMBLE));
                    m->Feed(body, n > 2 ? n - 2 : 0);
                }
                mirror = std::move(fresh);
                term = std::move(fresh2);
                ref = std::move(fresh3);
                ++gen;
                out = 0;
                if (cal) cal->OnSnapshot(now, target());
                engine.OnSnapshot(gen, now, mirror->View());
                break;
            }
            case 5:  // time passes
                if (cal) {
                    advance(
                        std::chrono::milliseconds(arg(0) * 8 + arg(1) * 100));
                } else {
                    now += std::chrono::milliseconds(arg(0) * 8 + arg(1) * 100);
                    engine.OnTick(now);
                }
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
        settle();
        draw();
        CheckInvariants(engine, *mirror->G, seen);
        if (cal && replyMode == 1) Check(forwardedAll == typedAll);
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

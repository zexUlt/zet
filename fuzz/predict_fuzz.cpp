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
// pieces, as the client's timer does). The terminal answers the program's own
// queries (CSI c, CSI 0 c, ESC Z, CSI 6 n) the same way: a position for
// CSI 6 n in modes 1 and 3, device attributes for the others in modes 1 and 4.
// Replies come in with the keystrokes, in one piece or two. Keystrokes hold no
// ESC, so what the demux let through to the agent can be compared with what the
// user typed and the program was answered:
//   9. the typed bytes are an in-order subsequence of what reached the agent
//      (and the bytes the demux still holds), in every reply mode; after all
//      deadlines have passed, of what reached the agent
//  10. when the terminal answers both replies, the bytes forwarded to the
//      agent plus the held ones are exactly the typed bytes and the replies to
//      the program's queries, in the order they came in (no reply of the
//      client's own leaks, none of the program's is taken); not asserted
//      while a program DA1 has been waiting for ten seconds or more, which
//      the demux forgets
//  11. nothing stays held while the calibration is not frozen: the output and
//      the EchoAck held for the wait go with it (a snapshot ends the wait)
//  12. a resize leaves the calibration with a reason to ask again
//  13. the hold buffer has the size the calibration was given: the fuzzer
//      makes it small, and an overflow ends the wait with the output in
//
// Widths (round C4). With the calibration on, the model terminal (Real and
// Ref, in every snapshot too) draws with a fixed width policy that the last
// input byte picks (bit 3 clear: the default table): A and P one or two cells
// wide, VS16 widening or not. The mirror starts from the default and learns it
// by the replies. A snapshot body holds only ASCII then: the mirror's cells
// of a real snapshot are the agent's, not laid out again. Invariant a. is owed
// only when the model has the default table: the overlay may draw over a glyph
// that the mirror has narrower than the terminal does, which the verdicts are
// there to end. Asserted after every step:
//  14. no verdict contradicts the model: Narrow means the model draws the
//      class one cell wide, Wide means two (E: VS16 widens); the mirror's
//      policy is the one the verdicts give
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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zet/core/time.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/unicode.hpp"
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

// What the model terminal draws for `s`. Reference glyphs and the two target
// classes (Cyrillic of ambiguous width, private use) are drawn as the grid
// does, by the model policy. Every other code point has a width of its own
// from a hash of the input and the code point, in {0, 1, 2}, whatever the
// mirror's table says: nothing drawn, an "X", or a wide rocket.
std::string ModelDraw(std::string_view s, std::uint32_t seed) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += s[i++];
            continue;
        }
        std::size_t len = c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2);
        if (i + len > s.size()) len = s.size() - i;
        char32_t cp = len == 4 ? c & 0x07U : len == 3 ? c & 0x0FU : c & 0x1FU;
        for (std::size_t k = 1; k < len; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3FU);
        }
        if (grid::IsReferenceWidth(cp) || grid::IsCyrillicAmbiguous(cp) ||
            grid::IsPrivateUse(cp)) {
            out.append(s.substr(i, len));
        } else {
            std::uint64_t h = (seed ^ cp) * 0x9E3779B97F4A7C15ULL;
            h ^= h >> 29;
            h *= 0xBF58476D1CE4E5B9ULL;
            const std::uint64_t width = (h >> 32) % 3;
            if (width == 1) out += 'X';
            if (width == 2) out += "\xf0\x9f\x9a\x80";
        }
        i += len;
    }
    return out;
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

// `need` is a subsequence of `hay`: greedy matching is as good as any.
bool IsSubsequence(std::string_view need, std::string_view hay) {
    std::size_t k = 0;
    for (const char c : hay) {
        if (k < need.size() && need[k] == c) ++k;
    }
    return k == need.size();
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
    // REP repeats the last printed character: the mirror, which starts blank,
    // and the terminal, which has had a shell before, must agree on it, or a
    // REP before the first character makes them differ for a reason that is
    // no width at all. One character is printed and taken back on all three.
    if (calibrated) {
        static constexpr std::uint8_t SAME_LAST[] = {'x', 0x08, 0x1B, '[', 'K'};
        for (Mirror* m : {mirror.get(), term.get(), ref.get()}) {
            m->Feed(SAME_LAST, sizeof(SAME_LAST));
        }
    }
    // The model terminal's widths, from the last byte of the input.
    grid::WidthPolicy model;
    // Seeds the widths of the code points the model draws by hash.
    const std::uint32_t drawSeed =
        static_cast<std::uint32_t>(data[0]) * 0x01000193U ^
        static_cast<std::uint32_t>(data[1]) << 8 ^
        static_cast<std::uint32_t>(data[size - 1]) << 16 ^
        static_cast<std::uint32_t>(size);
    if (calibrated) {
        const std::uint8_t b = data[size - 1];
        // Only the target classes follow the policy; the rest of A, VS16 and
        // everything else is drawn by ModelDraw, never widened by a policy.
        if (((b >> 3) & 1) != 0) {
            model.Cyrillic = static_cast<std::uint8_t>(1 + (b & 1));
            model.PrivateUseBmp = static_cast<std::uint8_t>(1 + ((b >> 1) & 1));
            model.PrivateUsePlanes =
                static_cast<std::uint8_t>(1 + ((b >> 2) & 1));
        }
        term->G->SetWidthPolicy(model);
        ref->G->SetWidthPolicy(model);
        // Wide Cyrillic widens Cyrillic alone: Greek, é and the rest stay one
        // cell, so a verdict for Cyr says nothing the model contradicts.
        auto probe = grid::Grid::Create(8, 2, 0);
        if (probe.has_value()) {
            std::unique_ptr<grid::Grid> pg(probe.value());
            pg->SetWidthPolicy(model);
            grid::VtParser pp;
            static constexpr char GLYPHS[] =
                "\xce\xb1\xc3\xa9\xd0\x96\xe2\x94\x80";
            pp.Feed(reinterpret_cast<const std::byte*>(GLYPHS),
                    sizeof(GLYPHS) - 1, *pg);
            Check(pg->CursorCol() == 3 + model.Cyrillic);
        }
    }
    // The overlay is built for a terminal that lays out as the mirror does;
    // when it does not, a prediction can land on a glyph the mirror has
    // narrower than the terminal, and invariant a. is not owed.
    const bool sameWidths = model == grid::WidthPolicy{};
    // Set once the model has drawn a code point otherwise than the mirror
    // does, so that the rows it laid out
    // differ from the mirror's; cleared when a matched or fixed column leaves
    // the cursor's row without marks (the overlay keeps off a row the mirror
    // does not trust, and the undo owes the screen as it was again).
    bool drewClusters = false;
    std::uint32_t settledCols = 0;
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
    std::string expectedAll;   // typed + the replies to the program
    std::optional<Timestamp> waitingSince;
    struct Reply {
        Timestamp At;
        std::string Bytes;
        bool Own;  // the answer to the client's query, not the program's
    };
    std::vector<Reply> replies;
    const auto target = [&] {
        return CalibrationTarget{mirror->G.get(), &mirror->P, &engine};
    };
    // A small hold buffer now and then, so that the overflow is reached.
    const std::size_t lagMax = data[1] % 4 == 0
                                   ? 16 + static_cast<std::size_t>(data[0]) % 48
                                   : CPR_LAG_BYTES_MAX;
    if (calibrated) {
        cal = Calibration::Create(false, target(), lagMax);
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
        Check(!sameWidths || drewClusters || SameScreen(*term->G, *ref->G));
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
    std::uint64_t refDa1 = 0;  // the queries of the program already answered
    std::uint64_t refCpr = 0;
    // `md`: what the model terminal draws when it is not what the mirror is
    // fed (a ZWJ pair as one glyph, the cursor moved by DECBI).
    const auto feed = [&](const std::uint8_t* d, std::size_t n,
                          const std::uint8_t* md = nullptr,
                          std::size_t mn = 0) {
        if (md == nullptr) {
            md = d;
            mn = n;
        }
        term->Feed(md, mn);
        ref->Feed(md, mn);
        if (cal) {
            // The terminal answers the program's queries, whole, one reply
            // each, in the order they came.
            while (refDa1 < ref->P.Da1QueryCount() ||
                   refCpr < ref->P.CprQueryCount()) {
                const bool cpr = refCpr < ref->P.CprQueryCount();
                // Which came first is not known; position replies first.
                if (cpr) {
                    ++refCpr;
                    if (replyMode == 1 || replyMode == 3) {
                        replies.push_back(
                            {now + replyDelay,
                             "\x1b[" +
                                 std::to_string(term->G->CursorRow() + 1) +
                                 ";" +
                                 std::to_string(term->G->CursorCol() + 1) + "R",
                             false});
                    }
                } else {
                    ++refDa1;
                    if (replyMode == 1 || replyMode == 4) {
                        replies.push_back(
                            {now + replyDelay, "\x1b[?62;c", false});
                    }
                }
            }
        }
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
        if (!reply.empty()) replies.push_back({now + replyDelay, reply, true});
        cal->OnQueryWritten(now, target());
    };
    const auto stdinBytes = [&](std::string_view bytes, bool own = false) {
        if (bytes.empty()) return;
        if (!own) expectedAll.append(bytes);
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
                    const bool own = replies[k].Own;
                    replies.erase(replies.begin() +
                                  static_cast<std::ptrdiff_t>(k));
                    const std::size_t cut = r.size() > 3 ? r.size() / 2 : 0;
                    stdinBytes(std::string_view(r).substr(0, cut), own);
                    stdinBytes(std::string_view(r).substr(cut), own);
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
        const std::uint8_t op = data[i] % 10;
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
                // With the calibration a terminal one column wide is not
                // modelled: whether its cursor stays pending a wrap after a
                // resize is the terminal's own, and a width verdict would
                // read the difference as a glyph's.
                const auto c =
                    static_cast<std::uint16_t>(arg(0) % 60 + (cal ? 2 : 1));
                const auto r = static_cast<std::uint16_t>(arg(1) % 10 + 1);
                undo();
                if (cal) {
                    cal->OnResize(now, target());
                    // A resize always leaves a reason to ask again.
                    Check((cal->Reasons() & CalResize) != 0);
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
                // A real snapshot holds no query: the body is masked.
                std::array<std::uint8_t, 24> clean = echoOff ? shown : plain;
                for (std::size_t k = 0; k < n; ++k) {
                    if (clean[k] == 'c' || clean[k] == 'n' || clean[k] == 'Z' ||
                        (!sameWidths && clean[k] >= 0x80)) {
                        clean[k] = '.';
                    }
                }
                const std::uint8_t* body = clean.data() + (n > 2 ? 2 : n);
                if (calibrated) {
                    fresh2->G->SetWidthPolicy(model);
                    fresh3->G->SetWidthPolicy(model);
                }
                for (Mirror* m : {fresh.get(), fresh2.get(), fresh3.get()}) {
                    m->Feed(PREAMBLE, sizeof(PREAMBLE));
                    m->Feed(body, n > 2 ? n - 2 : 0);
                }
                mirror = std::move(fresh);
                term = std::move(fresh2);
                ref = std::move(fresh3);
                ++gen;
                out = 0;
                refDa1 = 0;
                refCpr = 0;
                if (cal) {
                    cal->OnSnapshot(now, target());
                    // The terminal gets the snapshot's bytes too, and the
                    // demultiplexer sees them (the client's NoteWritten).
                    cal->OnWritten(
                        {reinterpret_cast<const std::byte*>(PREAMBLE),
                         sizeof(PREAMBLE)},
                        now);
                    cal->OnWritten({reinterpret_cast<const std::byte*>(body),
                                    n > 2 ? n - 2 : 0},
                                   now);
                }
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
            case 8: {  // agent output of glyph tokens (round C4)
                // The mirror gets the tokens as they are; the model terminal
                // draws every code point that is not a reference glyph or of
                // a target class with a width of its own (ModelDraw), and
                // moves the cursor left for DECBI.
                struct Token {
                    const char* Wire;
                    const char* Model;
                };
                static constexpr Token TOKENS[] = {
                    {"\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x92\xbb",
                     "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x92\xbb"},
                    {"\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd",
                     "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd"},
                    {"\xf0\x9f\x91\xa8\xe2\x80\x8d",
                     "\xf0\x9f\x91\xa8\xe2\x80\x8d"},
                    {"\xf0\x9f\x92\xbb", "\xf0\x9f\x92\xbb"},
                    {"\xf0\x9f\xab\xa9", "\xf0\x9f\xab\xa9"},
                    {"\xf0\x9f\xab\xaa", "\xf0\x9f\xab\xaa"},
                    {"\xc2\xad", "\xc2\xad"},
                    {"\x1b"
                     "6",
                     "\x1b[D"},
                    {"\xd0\x96", "\xd0\x96"},
                    {"\xee\x82\xb0", "\xee\x82\xb0"},
                    {"\xe2\x98\x81\xef\xb8\x8f", "\xe2\x98\x81\xef\xb8\x8f"},
                    {"\xce\xb1", "\xce\xb1"},          // Greek, A
                    {"\xc3\xa9", "\xc3\xa9"},          // e acute, A
                    {"\xc5\x82", "\xc5\x82"},          // Latin, reference
                    {"\xe2\x94\x80", "\xe2\x94\x80"},  // box drawing, A
                    {"\xe2\x98\xb0", "\xe2\x98\xb0"},  // trigram
                    {"\xe1\x84\x80\xe1\x85\xa1", "\xe1\x84\x80\xe1\x85\xa1"},
                    {"\xe3\x81\x8b\xe3\x82\x99", "\xe3\x81\x8b\xe3\x82\x99"},
                    {"\xf0\x9f\x87\xb7", "\xf0\x9f\x87\xb7"},
                    {"\xd8\x80", "\xd8\x80"},
                    {"\xe4\xb8\xad", "\xe4\xb8\xad"},
                    {"X", "X"},
                    {" ", " "},
                    {"\r", "\r"},
                    {"\x1b[3C", "\x1b[3C"},
                    {"\x1b[2D", "\x1b[2D"},
                };
                constexpr std::size_t TOKEN_COUNT =
                    sizeof(TOKENS) / sizeof(TOKENS[0]);
                // Only between sequences: what the model draws differently is
                // a width, never how the bytes parse, so inside a sequence of
                // an earlier piece (or of the mirror's own parser) the tokens
                // go to the model as they are.
                const bool plainWire =
                    term->P.State() != grid::EParserState::Ground ||
                    term->P.Utf8Pending() ||
                    mirror->P.State() != grid::EParserState::Ground ||
                    mirror->P.Utf8Pending() || pieceLen != 0;
                std::string wire;
                std::string drawn;
                // The piece may end inside a joined pair: the bytes so far go
                // out as a piece of their own.
                const std::size_t cut = arg(n > 5 ? 5 : 0) % 2 == 0 && n > 1
                                            ? 1 + arg(n - 1) % (n - 1)
                                            : n;
                undo();
                for (std::size_t k = 0; k < n && k < 5; ++k) {
                    if (k == cut) {
                        feed(
                            reinterpret_cast<const std::uint8_t*>(wire.data()),
                            wire.size(),
                            reinterpret_cast<const std::uint8_t*>(drawn.data()),
                            drawn.size());
                        deliver(wire.size());
                        wire.clear();
                        drawn.clear();
                        undo();
                    }
                    const Token& tok = TOKENS[p[k] % TOKEN_COUNT];
                    const std::string drawnAs =
                        plainWire ? tok.Wire : ModelDraw(tok.Model, drawSeed);
                    if (std::string_view(tok.Wire) != drawnAs) {
                        drewClusters = true;
                    }
                    wire += tok.Wire;
                    drawn += drawnAs;
                }
                feed(reinterpret_cast<const std::uint8_t*>(wire.data()),
                     wire.size(),
                     reinterpret_cast<const std::uint8_t*>(drawn.data()),
                     drawn.size());
                deliver(wire.size());
                break;
            }
            case 9: {  // the program asks the terminal something
                static constexpr const char* QUERIES[] = {
                    "\x1b[c", "\x1b[0c", "\x1bZ", "\x1b[6n", "\x1b[5n"};
                const char* q = QUERIES[arg(0) % 5];
                undo();
                feed(reinterpret_cast<const std::uint8_t*>(q),
                     std::char_traits<char>::length(q));
                const std::size_t extra = arg(1) % 4;
                if (extra != 0) {
                    feed(shown.data(), extra < n ? extra : n);
                    deliver(std::char_traits<char>::length(q) +
                            (extra < n ? extra : n));
                } else {
                    deliver(std::char_traits<char>::length(q));
                }
                break;
            }
            default:  // another look at the clock
                engine.OnTick(now);
                break;
        }
        settle();
        draw();
        if (cal && cal->Matched() + cal->ColumnFixed() != settledCols) {
            settledCols = cal->Matched() + cal->ColumnFixed();
            const grid::Grid& g = *mirror->G;
            if (drewClusters && !g.WidthDoubtBefore(g.CursorRow(), g.Cols())) {
                drewClusters = false;
            }
        }
        CheckInvariants(engine, *mirror->G, seen);
        if (cal) {
            const ByteSpan held = cal->Demux().HeldBytes();
            const std::string heldStr(
                reinterpret_cast<const char*>(held.data()), held.size());
            Check(IsSubsequence(typedAll, forwardedAll + heldStr));
            // Waiting for a program DA1 reply for ten seconds is forgotten.
            if (cal->Demux().Ahead() + cal->Demux().Behind() == 0) {
                waitingSince.reset();
            } else if (!waitingSince) {
                waitingSince = now;
            }
            const bool aged =
                waitingSince && now - *waitingSince >= std::chrono::seconds(10);
            if (replyMode == 1 && !aged) {
                Check(forwardedAll + heldStr == expectedAll);
            }
            // No verdict contradicts the terminal.
            const auto contradicts = [&](grid::EWidthClass cls, bool wide) {
                const EWidthVerdict v = cal->Verdict(cls);
                return (v == EWidthVerdict::Narrow && wide) ||
                       (v == EWidthVerdict::Wide && !wide);
            };
            Check(
                !contradicts(grid::EWidthClass::Cyrillic, model.Cyrillic == 2));
            Check(!contradicts(grid::EWidthClass::PrivateUseBmp,
                               model.PrivateUseBmp == 2));
            Check(!contradicts(grid::EWidthClass::PrivateUsePlanes,
                               model.PrivateUsePlanes == 2));
            // The other classes have no verdict, whatever the model draws.
            for (const auto cls :
                 {grid::EWidthClass::Ambiguous, grid::EWidthClass::Emoji,
                  grid::EWidthClass::Cluster}) {
                Check(cal->Verdict(cls) == EWidthVerdict::Unknown);
            }
            // Nothing is kept for a wait that is over.
            if (!cal->Frozen()) {
                Check(!cal->HoldsLag() && !cal->HoldsDeferredAck());
            }
        }
        if (echoOff) {
            Check(engine.ConfirmedEpoch() == 0);
            for (const Prediction& pr : engine.Predictions()) {
                Check(!engine.IsVisible(pr));
            }
        }
    }

    // Every deadline passes: whatever the demux held is out, and the typed
    // bytes are all among what reached the agent.
    if (cal) {
        advance(std::chrono::seconds(30));
        settle();
        Check(IsSubsequence(typedAll, forwardedAll));
    }

    // The engine reads the mirror and never writes it: a call that only reads
    // leaves every cell and the cursor as it was.
    const std::uint64_t before = Hash(*mirror->G);
    engine.OnTick(now);
    engine.OnInput({}, in, now, mirror->View());
    Check(Hash(*mirror->G) == before);
    return 0;
}

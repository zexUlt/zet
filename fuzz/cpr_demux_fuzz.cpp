// Fuzz target for the stdin demultiplexer (design.md §9, "Разбор stdin").
//
// A script drives a model of the program, the user and the terminal. Input:
// the first byte picks the mode (odd: replies may be lost), then operations,
// each one byte selecting it, one byte giving the operand length (capped), the
// operand:
//   0  the program writes `CSI 6 n CSI c` (fish)
//   1  the program writes a DA1 query alone: CSI c, CSI 0 c, CSI 0;1 c, ESC Z
//   2  the program writes `CSI 6 n` alone (bash, readline)
//   3  the client writes its own query `CSI 6 n CSI c`
//   4  the terminal answers the next query in order; the reply goes into the
//      pipe (lost on an odd operand byte in lossy mode)
//   5  the user types a key: F3 with modifiers, lone Esc, Alt-[, arrows
//   6  the user pastes text, a reply look-alike in it on an odd operand byte
//   7  the terminal answers the next query inside a paste (the reply is lost)
//   8  the client reads from the pipe, the operand byte says how much
//   9  time passes     10  resize     11  cancel (snapshot)
//   12 the program writes a string with query text in it, or DECXCPR
//   13 the client reads the whole pipe
// The terminal answers every query in order and writes each reply in one
// piece; the pipe is cut at any byte by the reads.
//
// Asserted:
//   1. while no reply is lost or late and a reply is not read in pieces more
//      than CPR_HOLD apart: every own reply is stripped whole and raises
//      Answered with the model's position (nothing if the query was
//      abandoned by a resize or cancel), and nothing else is stripped; the
//      program's replies and the keys reach Forward byte for byte, in order
//   2. otherwise nothing but a reply is stripped, never a paste, never a reply
//      of the program that the terminal answered before any reply was
//      lost (unless it came in late enough for the count to have expired), and
//      a key only as the position in front of a DA1 reply
//   3. forwarded + stripped + held is the input, in order; nothing is held
//      past CPR_HOLD from its first byte, nor without an own entry, nor while
//      the program's DA1 reply is expected first; no strip inside a paste;
//      one event at most per query accepted
// At the end time runs past every deadline: nothing is held, the own entry is
// forgotten.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "zet/core/time.hpp"
#include "zet/predict/cpr_demux.hpp"
#include "zet/predict/limits.hpp"

using namespace zet;
using namespace zet::predict;

namespace {

#define REQUIRE_INV(cond)                                                \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "invariant at line %d: %s\n", __LINE__, \
                         #cond);                                         \
            __builtin_trap();                                            \
        }                                                                \
    } while (0)

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

/// A strip is a position reply, a DA1 reply, or a position followed by DA1.
bool IsReply(const std::string& s) {
    std::size_t i = 0;
    const auto da1 = [&]() {
        if (s.compare(i, 3, "\x1b[?") != 0) {
            return false;
        }
        std::size_t j = i + 3;
        while (j < s.size() && (IsDigit(s[j]) || s[j] == ';')) {
            ++j;
        }
        if (j >= s.size() || s[j] != 'c') {
            return false;
        }
        i = j + 1;
        return true;
    };
    if (s.compare(0, 2, "\x1b[") == 0 && s.size() > 2 && IsDigit(s[2])) {
        std::size_t j = 2;
        while (j < s.size() && IsDigit(s[j])) {
            ++j;
        }
        if (j >= s.size() || s[j] != ';') {
            return false;
        }
        ++j;
        const std::size_t second = j;
        while (j < s.size() && IsDigit(s[j])) {
            ++j;
        }
        if (j == second || j >= s.size() || s[j] != 'R') {
            return false;
        }
        i = j + 1;
        if (i == s.size()) {
            return true;
        }
        return da1() && i == s.size();
    }
    return da1() && i == s.size();
}

std::string Pos(unsigned row, unsigned col) {
    return "\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "R";
}

/// Replies of different terminals, short and long.
const std::array<const char*, 4> DA1_REPLIES{
    "\x1b[?62;22c", "\x1b[?1;2c", "\x1b[?64;1;2;6;9;15;16;17;18;21;22;28;29c",
    "\x1b[?61;4;6;7;14;21;22;23;24;28;32;42;52c"};

constexpr int TAG_KEY = -1;
constexpr int TAG_PASTE = -2;

struct Unit {
    bool Own{false};
    std::string Bytes;
    Timestamp QueriedAt;
    unsigned Row{0};
    unsigned Col{0};
    bool Delivered{false};  // put in the pipe
    bool Lost{false};
    bool Abandoned{false};
    bool Stripped{false};
    std::size_t Read{0};  // bytes read so far
    bool FirstReadSet{false};
    Timestamp FirstRead;
};

struct Harness : CprSink {
    std::unique_ptr<CprDemux> D = CprDemux::Create();
    Timestamp Now = Timestamp::FromNanos(1'000'000'000);

    // The model.
    std::vector<Unit> Units;
    std::size_t NextAnswer{0};  // the next unit the terminal answers
    std::string Pipe;
    std::vector<int> PipeTag;
    bool Lossy{false};
    bool Clean{true};
    std::size_t LostFirst{SIZE_MAX};  // index of the first lost unit

    // What the demultiplexer was given and returned.
    std::string Input;
    std::vector<int> InputTag;
    std::vector<Timestamp> InputAt;
    std::string Accounted;  // forwarded + stripped, in order
    std::string ForwardedNow;
    std::size_t StripsNow = 0;
    std::size_t EventsNow = 0;
    std::vector<std::size_t> StripStarts;
    std::size_t Queries = 0;
    std::size_t QueryEvents = 0;
    std::size_t FailedEvents = 0;
    bool ExpectAnswer{false};
    unsigned ExpectRow{0};
    unsigned ExpectCol{0};

    bool Allowed(const Unit& u, std::size_t index) const {
        if (u.Own) {
            return true;
        }
        if (LostFirst < index) {
            return true;  // after a reply that never came, order is gone
        }
        // Its count may have expired: a reply this late is nobody's order.
        return Now - u.QueriedAt >= CPR_TIMEOUT + CPR_FORGET;
    }

    void Forward(ByteSpan bytes) noexcept override {
        for (const std::byte b : bytes) {
            Accounted.push_back(static_cast<char>(b));
            ForwardedNow.push_back(static_cast<char>(b));
        }
    }
    void Strip(ByteSpan bytes) noexcept override {
        std::string text;
        for (const std::byte b : bytes) {
            text.push_back(static_cast<char>(b));
        }
        REQUIRE_INV(IsReply(text));
        const std::size_t at = Accounted.size();
        REQUIRE_INV(at + text.size() <= Input.size());
        REQUIRE_INV(Input.compare(at, text.size(), text) == 0);
        // The DA1 part is the last reply of the strip.
        const std::size_t da1At = text.rfind("\x1b[?");
        REQUIRE_INV(da1At != std::string::npos);
        for (std::size_t i = 0; i < text.size(); ++i) {
            REQUIRE_INV(InputTag[at + i] != TAG_PASTE);
        }
        const int tag = InputTag[at + da1At];
        REQUIRE_INV(tag >= 0);
        const auto& unit = Units[static_cast<std::size_t>(tag)];
        if (Clean) {
            // Exactly our whole reply, nothing else.
            REQUIRE_INV(unit.Own);
            REQUIRE_INV(text == unit.Bytes);
            for (std::size_t i = 0; i < text.size(); ++i) {
                REQUIRE_INV(InputTag[at + i] == tag);
            }
            ExpectAnswer = !unit.Abandoned;
            ExpectRow = unit.Row;
            ExpectCol = unit.Col;
        } else {
            REQUIRE_INV(Allowed(unit, static_cast<std::size_t>(tag)));
            for (std::size_t i = 0; i < da1At; ++i) {
                // The position part: a key, or a reply.
                REQUIRE_INV(InputTag[at + i] >= TAG_KEY);
            }
            for (std::size_t i = da1At; i < text.size(); ++i) {
                REQUIRE_INV(InputTag[at + i] == tag);
            }
        }
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (InputTag[at + i] >= 0) {
                Units[static_cast<std::size_t>(InputTag[at + i])].Stripped =
                    true;
            }
        }
        StripStarts.push_back(Accounted.size());
        Accounted += text;
        ++StripsNow;
    }
    void Event(const CprEvent& event) noexcept override {
        ++EventsNow;
        ++QueryEvents;
        if (event.Kind == ECprEvent::Failed) {
            ++FailedEvents;
        }
        if (Clean && event.Kind == ECprEvent::Answered) {
            REQUIRE_INV(ExpectAnswer);
            REQUIRE_INV(event.Row == ExpectRow);
            REQUIRE_INV(event.Col == ExpectCol);
            ExpectAnswer = false;
        }
    }

    /// The oracle that holds whatever happened.
    void Settle(bool wasQuiet, const std::string& chunk) {
        std::string whole = Accounted;
        for (const std::byte b : D->HeldBytes()) {
            whole.push_back(static_cast<char>(b));
        }
        REQUIRE_INV(whole == Input);
        if (wasQuiet) {
            REQUIRE_INV(StripsNow == 0);
            REQUIRE_INV(EventsNow == 0);
            REQUIRE_INV(ForwardedNow == chunk);
        }
        REQUIRE_INV(QueryEvents <= Queries);
        ForwardedNow.clear();
        StripsNow = 0;
        EventsNow = 0;
        REQUIRE_INV(D->HeldSize() <= CPR_HELD_MAX);
        if (D->HeldSize() > 0) {
            REQUIRE_INV(D->Outstanding());
            REQUIRE_INV(D->Ahead() == 0);
            const std::size_t first = Input.size() - D->HeldSize();
            REQUIRE_INV(Now < InputAt[first] + CPR_HOLD);
        }
        if (D->Off()) {
            REQUIRE_INV(!D->Outstanding());
        }
        if (const auto d = D->NextDeadline()) {
            REQUIRE_INV(*d > Now);
        }
    }

    void Read(std::size_t count) {
        count = std::min(count, Pipe.size());
        const std::string chunk = Pipe.substr(0, count);
        // Cleanliness: late, or a reply in pieces too far apart.
        std::map<int, std::size_t> parts;
        for (std::size_t i = 0; i < count; ++i) {
            if (PipeTag[i] >= 0) {
                ++parts[PipeTag[i]];
            }
        }
        for (const auto& [tag, n] : parts) {
            auto& u = Units[static_cast<std::size_t>(tag)];
            if (Now - u.QueriedAt >= CPR_TIMEOUT) {
                Clean = false;
            }
            if (Now - u.QueriedAt >= CPR_TIMEOUT + CPR_FORGET) {
                // Read after the entry or the count was forgotten: as lost.
                LostFirst = std::min(LostFirst, static_cast<std::size_t>(tag));
            }
            if (u.FirstReadSet && Now - u.FirstRead >= CPR_HOLD) {
                Clean = false;
            }
            if (!u.FirstReadSet) {
                u.FirstReadSet = true;
                u.FirstRead = Now;
            }
            u.Read += n;
        }
        for (std::size_t i = 0; i < count; ++i) {
            InputTag.push_back(PipeTag[i]);
            InputAt.push_back(Now);
        }
        Pipe.erase(0, count);
        PipeTag.erase(PipeTag.begin(),
                      PipeTag.begin() + static_cast<std::ptrdiff_t>(count));

        const bool quiet = !D->Outstanding() && D->HeldSize() == 0;
        ExpectAnswer = false;
        Input += chunk;
        D->OnInput(
            {reinterpret_cast<const std::byte*>(chunk.data()), chunk.size()},
            Now, *this);
        Settle(quiet && !D->Outstanding(), chunk);
        // An own reply was stripped: its answer was raised in the same call.
        REQUIRE_INV(!Clean || !ExpectAnswer);
    }

    void Push(const std::string& bytes, int tag) {
        Pipe += bytes;
        PipeTag.insert(PipeTag.end(), bytes.size(), tag);
    }

    void Output(const std::string& s) {
        D->OnOutput({reinterpret_cast<const std::byte*>(s.data()), s.size()},
                    Now);
    }

    void AddProgram(const std::string& query, const std::string& reply,
                    unsigned row, unsigned col) {
        Output(query);
        Unit u;
        u.Bytes = reply;
        u.QueriedAt = Now;
        u.Row = row;
        u.Col = col;
        Units.push_back(u);
    }

    void Answer(bool lose, bool inPaste) {
        if (NextAnswer >= Units.size()) {
            return;
        }
        const std::size_t index = NextAnswer++;
        auto& u = Units[index];
        u.Delivered = true;
        if (lose || inPaste) {
            u.Lost = true;
            Clean = false;
            LostFirst = std::min(LostFirst, index);
            if (inPaste) {
                Push("\x1b[200~ab" + u.Bytes + "cd\x1b[201~", TAG_PASTE);
            }
            return;
        }
        Push(u.Bytes, static_cast<int>(index));
    }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    Harness h;
    if (!h.D || size < 1) {
        return 0;
    }
    h.Lossy = (data[0] & 1) != 0;
    std::size_t at = 1;
    int ops = 0;
    while (at + 2 <= size && ops++ < 400) {
        const std::uint8_t op = data[at] % 14;
        const std::size_t len = std::min<std::size_t>(data[at + 1], 64);
        at += 2;
        const std::size_t n = std::min(len, size - at);
        const std::uint8_t* p = data + at;
        at += n;
        const unsigned row = n > 0 ? p[0] % 40 + 1 : 1;
        const unsigned col = n > 1 ? p[1] % 120 + 1 : 1;
        switch (op) {
            case 0:
                h.AddProgram("\x1b[6n\x1b[c", Pos(row, col) + DA1_REPLIES[0],
                             row, col);
                break;
            case 1: {
                static constexpr std::array<const char*, 4> QUERIES{
                    "\x1b[c", "\x1b[0c", "\x1b[0;1c", "\x1bZ"};
                const std::uint8_t k = n > 0 ? p[0] % 4 : 0;
                h.AddProgram(QUERIES[k], DA1_REPLIES[n > 1 ? p[1] % 4 : 0], 0,
                             0);
                break;
            }
            case 2:
                h.AddProgram("\x1b[6n", Pos(row, col), row, col);
                break;
            case 3:
                if (h.D->OnQuerySent(h.Now)) {
                    ++h.Queries;
                    Unit u;
                    u.Own = true;
                    u.QueriedAt = h.Now;
                    u.Row = row;
                    u.Col = col;
                    u.Bytes = Pos(row, col) + DA1_REPLIES[n > 2 ? p[2] % 4 : 0];
                    h.Units.push_back(u);
                }
                break;
            case 4:
                h.Answer(h.Lossy && n > 2 && (p[2] & 1) != 0, false);
                break;
            case 5: {
                static constexpr std::array<const char*, 8> KEYS{
                    "\x1b[1;2R", "\x1b[1;5R", "\x1b[1;3R", "\x1b[1;16R",
                    "\x1b",      "\x1b[",     "\x1b[A",    "\x1bOR"};
                h.Push(KEYS[n > 0 ? p[0] % 8 : 0], TAG_KEY);
                break;
            }
            case 6: {
                std::string text;
                for (std::size_t i = 1; i < n; ++i) {
                    // No ESC: the paste must not end early.
                    text.push_back(p[i] == 0x1b ? '['
                                                : static_cast<char>(p[i]));
                }
                if (n > 0 && (p[0] & 1) != 0) {
                    text += Pos(1, 1) + DA1_REPLIES[0];
                }
                h.Push("\x1b[200~" + text + "\x1b[201~", TAG_PASTE);
                break;
            }
            case 7:
                h.Answer(false, h.Lossy);
                break;
            case 8:
                h.Read(n > 0 ? (p[0] == 255 ? h.Pipe.size()
                                            : p[0] % (h.Pipe.size() + 1))
                             : 0);
                break;
            case 9: {
                const std::uint8_t v = n > 0 ? p[0] : 1;
                h.Now += v >= 250 ? std::chrono::seconds(v - 248)
                                  : std::chrono::milliseconds(2 * v);
                h.D->OnTick(h.Now, h);
                h.Settle(false, "");
                break;
            }
            case 10:
                for (auto& u : h.Units) {
                    if (u.Own && u.Read < u.Bytes.size()) {
                        u.Abandoned = true;
                    }
                }
                h.D->OnResize(h.Now, h);
                h.Settle(false, "");
                break;
            case 11:
                for (auto& u : h.Units) {
                    if (u.Own && u.Read < u.Bytes.size()) {
                        u.Abandoned = true;
                    }
                }
                h.D->OnCancel(h.Now, h);
                h.Settle(false, "");
                break;
            case 12: {
                const std::uint8_t k = n > 0 ? p[0] % 3 : 0;
                if (k == 0) {
                    h.Output("\x1b]0;[6n[c\x07\x1bP1$r[6n[c\x1b\\");
                } else if (k == 1) {
                    h.AddProgram("\x1b[?6n", "\x1b[?4;5R", 0, 0);
                } else {
                    h.Output("text");
                }
                break;
            }
            case 13:
                h.Read(h.Pipe.size());
                break;
        }
    }
    // Everything the terminal has written is read, in one go.
    h.Read(h.Pipe.size());
    if (h.Clean) {
        for (const auto& u : h.Units) {
            if (u.Own && u.Delivered && !u.Lost) {
                REQUIRE_INV(u.Stripped);
            }
        }
        REQUIRE_INV(h.FailedEvents == 0);
    }
    // Time runs out: nothing stays held, nothing stays queued.
    h.Now += CPR_TIMEOUT;
    h.D->OnTick(h.Now, h);
    h.Settle(false, "");
    REQUIRE_INV(h.D->HeldSize() == 0);
    h.Now += CPR_FORGET;
    h.D->OnTick(h.Now, h);
    h.Settle(false, "");
    REQUIRE_INV(!h.D->Outstanding());
    REQUIRE_INV(h.D->HeldSize() == 0);
    REQUIRE_INV(h.Accounted == h.Input);
    // No strip starts inside a bracketed paste, by a model that knows only
    // the two markers.
    std::vector<bool> inPaste(h.Input.size(), false);
    bool in = false;
    for (std::size_t i = 0; i < h.Input.size();) {
        if (!in && h.Input.compare(i, 6, "\x1b[200~") == 0) {
            i += 6;
            in = true;
        } else if (in && h.Input.compare(i, 6, "\x1b[201~") == 0) {
            i += 6;
            in = false;
        } else {
            inPaste[i++] = in;
        }
    }
    for (const std::size_t start : h.StripStarts) {
        REQUIRE_INV(start >= inPaste.size() || !inPaste[start]);
    }
    return 0;
}

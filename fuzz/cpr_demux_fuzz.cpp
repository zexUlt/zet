// Fuzz target for the stdin demultiplexer (design.md §9, "Разбор stdin").
//
// A script drives a model of the program, the user and the terminal. Input:
// the first byte picks the mode (odd: replies may be lost), then operations,
// each one byte selecting it, one byte giving the operand length (capped), the
// operand:
//   0  the program writes `CSI 6 n CSI c` (fish)
//   1  the program writes a DA1 query alone: CSI c, CSI 0 c, CSI 0;1 c, ESC Z
//   2  the program writes `CSI 6 n` alone (bash, readline)
//   3  the client asks CanQuery, and writes its own query `CSI 6 n CSI c`
//   4  the terminal answers the next query in order; the reply goes into the
//      pipe (lost on an odd operand byte in lossy mode)
//   5  the user types a key: F3 with modifiers, lone Esc, Alt-[, arrows
//   6  the user pastes text, a reply look-alike in it on an odd operand byte
//   7  the terminal answers the next query inside a paste (the reply is lost)
//   8  the client reads from the pipe, the operand byte says how much (254:
//      cut inside the DA1 reply, around the 128-byte cap)
//   9  time passes     10  resize     11  cancel (snapshot)
//   12 the program writes a string with query text in it, or DECXCPR
//   13 the client reads the whole pipe
//   14 a paste start with no end marker, ESC and half markers in its text
//   15 a paste with ESC and half end markers in its text (it may end early)
//   16 the terminal answers the next query in two writes: the position now,
//      the DA1 reply on op 17 or before the next answer
//   17 the DA1 half of op 16 is written (no-op if none is pending)
//   18 time runs to NextDeadline(), a tick follows: it must make progress
//   19 time runs to NextDeadline() - 1ns, a tick follows: it changes nothing
//   20 the terminal writes a DA1 reply nobody asked for
//   21 the program writes the operand bytes as they are (only the check that
//      the Ground fast path of OnOutput counts what a plain parser counts)
// Replies come in several shapes: DA1 of 127 and 128 bytes, over the cap, and
// positions with leading zeros or five digits. The terminal answers every
// query in order; the pipe is cut at any byte by the reads.
//
// Asserted:
//   1. in a clean epoch (no reply lost, late, over the cap, unasked, in a
//      paste, or read in pieces that the hold could not join): every own
//      reply is stripped whole and raises Answered with the model's position
//      (nothing if the query was abandoned by a resize or cancel), and
//      nothing else is stripped; the program's replies and the keys reach
//      Forward byte for byte, in order. A position that arrived complete may
//      wait for its DA1 reply until the entry's CPR_TIMEOUT. The epoch ends
//      at the first disturbance and a new one begins once the own entry is
//      gone and every reply has been read and counted off
//   2. otherwise nothing but a reply is stripped, never a paste, never a reply
//      of the program that the terminal answered before any reply was
//      lost (unless it came in late enough for the count to have expired, or
//      a count was non-zero for a whole span), and a key only as the position
//      in front of a DA1 reply
//   3. forwarded + stripped + held is the input, in order; nothing is held
//      past CPR_HOLD from its first byte (a complete position: past the
//      entry's CPR_TIMEOUT plus CPR_HOLD), nor without an own entry, nor while
//      the program's DA1 reply is expected first; no strip inside a paste;
//      one event at most per query accepted
//   4. Pending() is exactly a live own entry; Silent() is exactly "an event
//      Failed or TimedOut and no resize since", and already so inside the
//      callback
//   5. NextDeadline is tight: a tick a nanosecond before it changes nothing,
//      a tick at it makes progress
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
#include <tuple>
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

/// The shapes of a DA1 reply: four real ones, exactly the cap, one short of
/// it, and over it (never recognised). `extra` lengthens the last one.
constexpr unsigned DA1_SHAPES = 7;

std::string Da1Reply(unsigned shape, unsigned extra) {
    const auto sized = [](std::size_t size) {
        return "\x1b[?" + std::string(size - 4, '1') + "c";
    };
    switch (shape % DA1_SHAPES) {
        case 4:
            return sized(CPR_DA1_REPLY_MAX);
        case 5:
            return sized(CPR_DA1_REPLY_MAX - 1);
        case 6:
            return sized(CPR_DA1_REPLY_MAX + 1 + extra % 20);
        default:
            return DA1_REPLIES[shape % DA1_REPLIES.size()];
    }
}

bool Overlong(unsigned shape) { return shape % DA1_SHAPES == 6; }

/// Plain, zero-padded to five digits.
std::string PosText(unsigned row, unsigned col, bool padded) {
    if (!padded) {
        return Pos(row, col);
    }
    char buf[40];
    std::snprintf(buf, sizeof buf, "\x1b[%05u;%05uR", row, col);
    return buf;
}

/// The model of a bracketed paste: only the two markers.
bool EndsInPaste(const std::string& s) {
    bool in = false;
    for (std::size_t i = 0; i < s.size();) {
        if (!in && s.compare(i, 6, "\x1b[200~") == 0) {
            in = true;
            i += 6;
        } else if (in && s.compare(i, 6, "\x1b[201~") == 0) {
            in = false;
            i += 6;
        } else {
            ++i;
        }
    }
    return in;
}

/// `s` at `at` starts with a whole position reply.
bool StartsWithPosition(const std::string& s, std::size_t at) {
    if (s.compare(at, 2, "\x1b[") != 0) {
        return false;
    }
    std::size_t i = at + 2;
    std::size_t digits = 0;
    while (i < s.size() && IsDigit(s[i])) {
        ++i;
        ++digits;
    }
    if (digits == 0 || i >= s.size() || s[i] != ';') {
        return false;
    }
    ++i;
    digits = 0;
    while (i < s.size() && IsDigit(s[i])) {
        ++i;
        ++digits;
    }
    return digits > 0 && i < s.size() && s[i] == 'R';
}

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
    bool Unsolicited{false};  // nobody asked for it
    bool Overlong{false};     // its DA1 reply is over the cap
    bool Counted{false};    // a DA1 query of the program, counted by the demux
    bool Finished{false};   // its reply was read whole
    bool Closed{false};     // belongs to an epoch that is over
    std::size_t PosLen{0};  // length of the position in front of the DA1
    std::size_t Read{0};    // bytes read so far
    bool FirstReadSet{false};
    Timestamp FirstRead;
};

struct Harness : CprSink {
    std::unique_ptr<CprDemux> D = CprDemux::Create();
    std::unique_ptr<grid::Grid> PlainGrid{grid::Grid::Create(1, 1, 0).value()};
    grid::VtParser Plain;
    std::unique_ptr<CprDemux> Raw = CprDemux::Create();
    std::unique_ptr<grid::Grid> RawGrid{grid::Grid::Create(1, 1, 0).value()};
    grid::VtParser RawPlain;
    Timestamp Now = Timestamp::FromNanos(1'000'000'000);

    // The model.
    std::vector<Unit> Units;
    std::size_t NextAnswer{0};  // the next unit the terminal answers
    std::string Pipe;
    std::vector<int> PipeTag;
    bool Lossy{false};
    // Clean and LostFirst hold for an epoch: they are reset once the own
    // entry is gone and every reply is accounted for.
    bool Clean{true};
    std::size_t LostFirst{SIZE_MAX};  // index of the first lost unit
    std::string Stream;               // everything ever put in the pipe
    bool HasDeferred{false};
    std::string Deferred;
    int DeferredTag{0};
    bool LiveOwn{false};
    Timestamp OwnTimeoutAt;
    bool ExpectSilent{false};
    std::size_t ProgOutstanding{0};  // program DA1 queries not answered whole
    Timestamp ProgSince;             // when that count last left zero

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
        if (u.Unsolicited || LostFirst <= index) {
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
        LiveOwn = false;
        if (event.Kind == ECprEvent::Failed) {
            ++FailedEvents;
        }
        if (event.Kind != ECprEvent::Answered) {
            ExpectSilent = true;
            REQUIRE_INV(D->Silent());  // already set inside the callback
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
            Timestamp limit = InputAt[first] + CPR_HOLD;
            if (StartsWithPosition(Input, first)) {
                // Held for its DA1 reply: to the entry's deadline, and
                // CPR_HOLD from there.
                limit = std::max(limit, OwnTimeoutAt + CPR_HOLD);
            }
            REQUIRE_INV(Now < limit);
        }
        REQUIRE_INV(D->Pending() == LiveOwn);
        REQUIRE_INV(D->Silent() == ExpectSilent);
        if (LiveOwn) {
            REQUIRE_INV(Now < OwnTimeoutAt);
        }
        if (D->Off()) {
            REQUIRE_INV(!D->Outstanding());
        }
        if (const auto d = D->NextDeadline()) {
            REQUIRE_INV(*d > Now);
        }
        MaybeNewEpoch();
    }

    [[nodiscard]] bool AllAnswered() const {
        return std::none_of(Units.begin(), Units.end(),
                            [](const Unit& u) { return !u.Delivered; });
    }

    /// The strong oracle comes back once nothing of the past can matter: the
    /// own entry is gone, the demux counts nothing, every reply was read.
    void MaybeNewEpoch() {
        if (Clean && LostFirst == SIZE_MAX) {
            return;
        }
        if (D->Outstanding() || D->HeldSize() > 0 || D->Ahead() != 0 ||
            D->Behind() != 0 || !Pipe.empty() || HasDeferred || LiveOwn ||
            !AllAnswered()) {
            return;
        }
        Clean = true;
        LostFirst = SIZE_MAX;
        ProgOutstanding = 0;
        FailedEvents = 0;
        ExpectAnswer = false;
        for (auto& u : Units) {
            u.Closed = true;
            u.Finished = true;
        }
    }

    /// The first program DA1 query not answered whole (lost ones included),
    /// or the next unit's index when there is none.
    [[nodiscard]] std::size_t OldestOutstanding() const {
        for (std::size_t i = 0; i < Units.size(); ++i) {
            if (Units[i].Counted && !Units[i].Finished) {
                return i;
            }
        }
        return Units.size();
    }

    /// A count that was not zero for a whole span loses a query to ageing.
    /// The model does not track which: from the oldest one on, order is gone.
    void CheckProgAge() {
        if (ProgOutstanding > 0 &&
            Now - ProgSince >= CPR_TIMEOUT + CPR_FORGET) {
            Clean = false;
            LostFirst = std::min(LostFirst, OldestOutstanding());
            ProgSince = Now;
        }
    }

    using Sig = std::tuple<std::size_t, bool, bool, bool, bool, std::size_t,
                           std::size_t, std::size_t>;
    [[nodiscard]] Sig Signature() const {
        return {D->HeldSize(),      D->Pending(), D->Outstanding(),
                D->Silent(),        D->Off(),     Accounted.size(),
                StripStarts.size(), QueryEvents};
    }

    void TickAt(Timestamp at) {
        Now = at;
        CheckProgAge();
        D->OnTick(Now, *this);
        Settle(false, "");
    }

    void RunToDeadline(bool justBefore) {
        const auto d = D->NextDeadline();
        if (!d) {
            return;
        }
        const Sig before = Signature();
        if (justBefore) {
            TickAt(*d - std::chrono::nanoseconds(1));
            REQUIRE_INV(Signature() == before);
            REQUIRE_INV(D->NextDeadline() == d);
        } else {
            TickAt(*d);
            REQUIRE_INV(Signature() != before);
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
            // A position that arrived whole waits for its DA1 reply to the
            // entry's deadline (checked above as lateness); anything else
            // for CPR_HOLD from the first byte.
            const bool positionIn = u.Own && !u.Abandoned && u.Read >= u.PosLen;
            if (u.FirstReadSet && !positionIn &&
                Now - u.FirstRead >= CPR_HOLD) {
                Clean = false;
            }
            if (!u.FirstReadSet) {
                u.FirstReadSet = true;
                u.FirstRead = Now;
            }
            u.Read += n;
            if (u.Counted && !u.Overlong && !u.Finished &&
                u.Read >= u.Bytes.size()) {
                u.Finished = true;
                if (ProgOutstanding > 0) {
                    --ProgOutstanding;
                }
            }
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
        if (HasDeferred && tag != DeferredTag) {
            Clean = false;  // something between a position and its DA1 reply
        }
        Pipe += bytes;
        Stream += bytes;
        PipeTag.insert(PipeTag.end(), bytes.size(), tag);
    }

    void FlushDeferred() {
        if (!HasDeferred) {
            return;
        }
        HasDeferred = false;
        if (EndsInPaste(Stream)) {
            // A paste began between the two writes: the DA1 reply is in it.
            const auto index = static_cast<std::size_t>(DeferredTag);
            Units[index].Lost = true;
            Clean = false;
            LostFirst = std::min(LostFirst, index);
            Push(Deferred, TAG_PASTE);
        } else {
            Push(Deferred, DeferredTag);
        }
    }

    /// An over-long, unasked or otherwise unaccountable reply: order is gone
    /// from the oldest query still waiting for its answer.
    void OrderLost() {
        Clean = false;
        LostFirst = std::min(LostFirst, OldestOutstanding());
    }

    void Abandon() {
        for (auto& u : Units) {
            if (u.Own && u.Read < u.Bytes.size()) {
                u.Abandoned = true;
                // The held start of its reply keeps its CPR_HOLD only.
                if (u.Read > 0 && Now - u.FirstRead >= CPR_HOLD) {
                    Clean = false;
                }
            }
        }
    }

    /// After the call: the live entry is abandoned; a resize clears Silent.
    void AfterAbandon(bool resize) {
        LiveOwn = false;
        if (resize) {
            ExpectSilent = false;
        }
    }

    /// The output goes to the demultiplexer, whose Ground fast path skips
    /// bytes, and byte by byte to a plain parser: both must count the same
    /// DA1 queries.
    void Output(const std::string& s) {
        const ByteSpan bytes{reinterpret_cast<const std::byte*>(s.data()),
                             s.size()};
        D->OnOutput(bytes, Now);
        for (const std::byte b : bytes) {
            Plain.FeedByte(b, *PlainGrid);
        }
        REQUIRE_INV(Plain.Da1QueryCount() == D->Da1Seen());
    }

    /// Arbitrary bytes as program output to a second demultiplexer, for the
    /// same check; the model above knows nothing of them.
    void RawOutput(const std::uint8_t* p, std::size_t n) {
        const ByteSpan bytes{reinterpret_cast<const std::byte*>(p), n};
        Raw->OnOutput(bytes, Now);
        for (const std::byte b : bytes) {
            RawPlain.FeedByte(b, *RawGrid);
        }
        REQUIRE_INV(RawPlain.Da1QueryCount() == Raw->Da1Seen());
    }

    void AddProgram(const std::string& query, const std::string& reply,
                    unsigned row, unsigned col, bool counted = false,
                    bool overlong = false) {
        CheckProgAge();
        Output(query);
        Unit u;
        u.Bytes = reply;
        u.QueriedAt = Now;
        u.Row = row;
        u.Col = col;
        // The demux counts a DA1 query; an over-long reply is never
        // recognised, so it is never counted off.
        u.Counted = counted;
        u.Overlong = overlong;
        if (counted) {
            if (ProgOutstanding == 0) {
                ProgSince = Now;
            }
            ++ProgOutstanding;
        }
        Units.push_back(u);
    }

    /// The terminal writes a DA1 reply nobody asked for.
    void Unasked(const std::string& reply) {
        FlushDeferred();
        OrderLost();
        Unit u;
        u.Bytes = reply;
        u.QueriedAt = Now;
        u.Delivered = true;
        u.Unsolicited = true;
        Units.push_back(u);
        if (EndsInPaste(Stream)) {
            Push(reply, TAG_PASTE);
        } else {
            Push(reply, static_cast<int>(Units.size() - 1));
        }
    }

    /// `split`: the position is written now and the DA1 reply later.
    void Answer(bool lose, bool inPaste, bool split) {
        FlushDeferred();
        while (NextAnswer < Units.size() && Units[NextAnswer].Delivered) {
            ++NextAnswer;
        }
        if (NextAnswer >= Units.size()) {
            return;
        }
        const std::size_t index = NextAnswer++;
        auto& u = Units[index];
        u.Delivered = true;
        if (u.Overlong) {
            OrderLost();
        }
        const bool pasting = EndsInPaste(Stream);
        if (lose || inPaste || pasting) {
            u.Lost = true;
            Clean = false;
            LostFirst = std::min(LostFirst, index);
            if (pasting) {
                Push(u.Bytes, TAG_PASTE);
            } else if (inPaste) {
                Push("\x1b[200~ab" + u.Bytes + "cd\x1b[201~", TAG_PASTE);
            }
            return;
        }
        const std::size_t cut = PositionLength(u.Bytes);
        if (split && cut > 0 && cut < u.Bytes.size()) {
            Push(u.Bytes.substr(0, cut), static_cast<int>(index));
            HasDeferred = true;
            Deferred = u.Bytes.substr(cut);
            DeferredTag = static_cast<int>(index);
            return;
        }
        Push(u.Bytes, static_cast<int>(index));
    }

    /// Length of the position reply at the start of `bytes`, 0 if none.
    static std::size_t PositionLength(const std::string& bytes) {
        if (!StartsWithPosition(bytes, 0)) {
            return 0;
        }
        return bytes.find('R') + 1;
    }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    Harness h;
    if (!h.D || !h.Raw || size < 1) {
        return 0;
    }
    h.Lossy = (data[0] & 1) != 0;
    std::size_t at = 1;
    int ops = 0;
    while (at + 2 <= size && ops++ < 400) {
        const std::uint8_t op = data[at] % 22;
        const std::size_t len = std::min<std::size_t>(data[at + 1], 64);
        at += 2;
        const std::size_t n = std::min(len, size - at);
        const std::uint8_t* p = data + at;
        at += n;
        // Positions: plain, zero-padded to five digits, or five digits wide.
        const unsigned style = n > 3 ? p[3] % 3 : 0;
        unsigned row = n > 0 ? p[0] % 40 + 1 : 1;
        unsigned col = n > 1 ? p[1] % 120 + 1 : 1;
        if (style == 2) {
            row = row * 1000 + 7;
            col = col * 500 + 7;
        }
        const bool padded = style == 1;
        h.CheckProgAge();
        switch (op) {
            case 0:
                h.AddProgram("\x1b[6n\x1b[c",
                             PosText(row, col, padded) + DA1_REPLIES[0], row,
                             col, true);
                break;
            case 1: {
                static constexpr std::array<const char*, 4> QUERIES{
                    "\x1b[c", "\x1b[0c", "\x1b[0;1c", "\x1bZ"};
                const std::uint8_t k = n > 0 ? p[0] % 4 : 0;
                const unsigned shape = n > 1 ? p[1] : 0;
                h.AddProgram(QUERIES[k], Da1Reply(shape, n > 2 ? p[2] : 0), 0,
                             0, true, Overlong(shape));
                break;
            }
            case 2:
                h.AddProgram("\x1b[6n", PosText(row, col, padded), row, col);
                break;
            case 3: {
                const bool can = h.D->CanQuery(h.Now, h);
                h.Settle(false, "");
                const bool sent = h.D->OnQuerySent(h.Now, h);
                REQUIRE_INV(sent == can);
                if (sent) {
                    ++h.Queries;
                    Unit u;
                    u.Own = true;
                    u.QueriedAt = h.Now;
                    u.Row = row;
                    u.Col = col;
                    const unsigned shape = n > 2 ? p[2] : 0;
                    u.Overlong = Overlong(shape);
                    u.Bytes = PosText(row, col, padded) +
                              Da1Reply(shape, n > 4 ? p[4] : 0);
                    u.PosLen = Harness::PositionLength(u.Bytes);
                    h.Units.push_back(u);
                    h.LiveOwn = true;
                    h.OwnTimeoutAt = h.Now + CPR_TIMEOUT;
                }
                h.Settle(false, "");
                break;
            }
            case 4:
                h.Answer(h.Lossy && n > 2 && (p[2] & 1) != 0, false, false);
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
                h.Answer(false, h.Lossy, false);
                break;
            case 8:
                if (n > 0 && p[0] == 254) {
                    // Cut inside a DA1 reply, around the cap.
                    const std::size_t da1 = h.Pipe.find("\x1b[?");
                    if (da1 != std::string::npos) {
                        h.Read(da1 + CPR_DA1_REPLY_MAX - 2 +
                               (n > 1 ? p[1] % 4 : 0));
                    }
                } else {
                    h.Read(n > 0 ? (p[0] == 255 ? h.Pipe.size()
                                                : p[0] % (h.Pipe.size() + 1))
                                 : 0);
                }
                break;
            case 9: {
                const std::uint8_t v = n > 0 ? p[0] : 1;
                h.TickAt(h.Now +
                         (v >= 250
                              ? Duration(std::chrono::seconds(v - 248))
                              : Duration(std::chrono::milliseconds(2 * v))));
                break;
            }
            case 10:
                h.Abandon();
                h.D->OnResize(h.Now, h);
                h.AfterAbandon(true);
                h.Settle(false, "");
                break;
            case 11:
                h.Abandon();
                h.D->OnCancel(h.Now, h);
                h.AfterAbandon(false);
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
            case 14:
            case 15: {
                // ESC, half end markers and markers in the text.
                static constexpr std::array<char, 7> ALPHABET{
                    '\x1b', '[', '2', '0', '1', '~', 'x'};
                std::string text;
                for (std::size_t i = 0; i < n; ++i) {
                    text.push_back(ALPHABET[p[i] % ALPHABET.size()]);
                }
                h.Push("\x1b[200~" + text + (op == 15 ? "\x1b[201~" : ""),
                       TAG_PASTE);
                break;
            }
            case 16:
                h.Answer(h.Lossy && n > 2 && (p[2] & 1) != 0, false, true);
                break;
            case 17:
                h.FlushDeferred();
                break;
            case 18:
                h.RunToDeadline(false);
                break;
            case 19:
                h.RunToDeadline(true);
                break;
            case 20:
                h.Unasked(Da1Reply(n > 0 ? p[0] : 0, n > 1 ? p[1] : 0));
                break;
            case 21:
                h.RawOutput(p, n);
                break;
        }
    }
    h.FlushDeferred();
    // Everything the terminal has written is read, in one go.
    h.Read(h.Pipe.size());
    if (h.Clean) {
        for (const auto& u : h.Units) {
            if (u.Own && u.Delivered && !u.Lost && !u.Closed) {
                REQUIRE_INV(u.Stripped);
            }
        }
        REQUIRE_INV(h.FailedEvents == 0);
    }
    // Time runs out: nothing stays held, nothing stays queued. A position
    // held for the entry goes CPR_HOLD after its deadline.
    h.TickAt(h.Now + CPR_TIMEOUT + CPR_HOLD);
    REQUIRE_INV(h.D->HeldSize() == 0);
    h.TickAt(h.Now + CPR_FORGET);
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

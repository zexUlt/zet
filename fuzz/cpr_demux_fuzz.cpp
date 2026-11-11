// Fuzz target for the stdin demultiplexer (design.md §9, "Разбор stdin").
//
// Input: operations, each one byte selecting it, one byte giving the operand
// length (capped), the operand:
//   0  bytes the client writes to the terminal (the program's queries in them)
//   1  the client's own query is written
//   2  bytes read from stdin
//   3  a terminal's answer to our query (position and DA1), cut by the operand
//   4  time passes: the operand's first byte times 4 ms
//   5  resize     6  cancel (snapshot)
//   7  a program query: CSI 6 n, CSI c, CSI ? 6 n, or a paste marker in stdin
// Asserted after every call:
//   1. no byte lost or invented: the bytes forwarded, the strips and what is
//      held, in order, are exactly the stdin bytes read
//   2. a strip is a reply: position, DA1, or both; never anything else
//   3. while no query of ours is in the queue and nothing is held, stdin
//      passes through unchanged and raises no event
//   4. nothing is held without a pending query and every deadline lies in
//      the future once the call has settled time; the queue and the hold
//      buffer stay within their limits
//   5. no strip starts inside a bracketed paste (until the demultiplexer is
//      switched off by CPR_FORGET, which forgets the paste too)
//   6. one event at most per query accepted
// At the end time runs past every deadline: nothing is held, and the queue is
// forgotten.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "zet/core/time.hpp"
#include "zet/predict/cpr_demux.hpp"
#include "zet/predict/limits.hpp"

using namespace zet;
using namespace zet::predict;

namespace {

#define REQUIRE_INV(cond)     \
    do {                      \
        if (!(cond)) {        \
            __builtin_trap(); \
        }                     \
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

struct Harness : CprSink {
    std::unique_ptr<CprDemux> D = CprDemux::Create();
    Timestamp Now = Timestamp::FromNanos(1'000'000'000);
    std::string Input;      // every stdin byte given
    std::string Accounted;  // forwarded + stripped, in order
    std::string ForwardedNow;
    std::size_t StripsNow = 0;
    std::size_t EventsNow = 0;
    std::vector<std::size_t> StripStarts;
    std::size_t Queries = 0;
    std::size_t QueryEvents = 0;
    bool EverOff = false;

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
        REQUIRE_INV(IsReply(text));  // 2.
        StripStarts.push_back(Accounted.size());
        Accounted += text;
        ++StripsNow;
    }
    void Event(const CprEvent&) noexcept override {
        ++EventsNow;
        ++QueryEvents;
    }

    void Settle(bool wasQuiet, const std::string& chunk) {
        // 1. accounting
        std::string whole = Accounted;
        for (const std::byte b : D->HeldBytes()) {
            whole.push_back(static_cast<char>(b));
        }
        REQUIRE_INV(whole == Input);
        // 3.
        if (wasQuiet) {
            REQUIRE_INV(StripsNow == 0);
            REQUIRE_INV(EventsNow == 0);
            REQUIRE_INV(ForwardedNow == chunk);
        }
        // 6.
        REQUIRE_INV(QueryEvents <= Queries);
        ForwardedNow.clear();
        StripsNow = 0;
        EventsNow = 0;
        // 4.
        REQUIRE_INV(D->QueueSize() <= CPR_QUEUE_MAX);
        REQUIRE_INV(D->HeldSize() <= CPR_HELD_MAX);
        if (D->HeldSize() > 0) {
            REQUIRE_INV(D->Pending());
        }
        if (D->Off()) {
            EverOff = true;
            REQUIRE_INV(D->QueueSize() == 0);
        }
        if (const auto d = D->NextDeadline()) {
            REQUIRE_INV(*d > Now);
        }
    }

    void Read(const std::string& chunk) {
        const bool quiet = !D->Outstanding() && D->HeldSize() == 0;
        Input += chunk;
        D->OnInput(
            {reinterpret_cast<const std::byte*>(chunk.data()), chunk.size()},
            Now, *this);
        Settle(quiet && !D->Outstanding(), chunk);
    }
};

std::string Reply(const std::uint8_t* p, std::size_t n) {
    const unsigned row = n > 0 ? p[0] % 40 + 1 : 1;
    const unsigned col = n > 1 ? p[1] % 120 + 1 : 1;
    std::string s =
        "\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "R";
    if (n < 3 || (p[2] & 1) == 0) {
        s += "\x1b[?62;22c";
    }
    // The last byte cuts the answer: a read that stops inside it.
    if (n > 3 && p[3] != 0) {
        s.resize(1 + p[3] % s.size());
    }
    return s;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    Harness h;
    if (!h.D) {
        return 0;
    }
    std::size_t at = 0;
    int ops = 0;
    while (at + 2 <= size && ops++ < 400) {
        const std::uint8_t op = data[at] % 8;
        const std::size_t len = std::min<std::size_t>(data[at + 1], 64);
        at += 2;
        const std::size_t n = std::min(len, size - at);
        const std::uint8_t* p = data + at;
        at += n;
        const std::string operand(reinterpret_cast<const char*>(p), n);
        switch (op) {
            case 0:
                h.D->OnOutput({reinterpret_cast<const std::byte*>(p), n});
                break;
            case 1:
                if (h.D->OnQuerySent(h.Now)) {
                    ++h.Queries;
                }
                break;
            case 2:
                h.Read(operand);
                break;
            case 3:
                h.Read(Reply(p, n));
                break;
            case 4:
                h.Now += std::chrono::milliseconds(4 * (n > 0 ? p[0] : 1));
                h.D->OnTick(h.Now, h);
                h.Settle(false, "");
                break;
            case 5:
                h.D->OnResize(h.Now, h);
                h.Settle(false, "");
                break;
            case 6:
                h.D->OnCancel(h.Now, h);
                h.Settle(false, "");
                break;
            case 7: {
                static constexpr std::array<const char*, 5> SNIPPETS{
                    "\x1b[6n", "\x1b[c", "\x1b[?6n", "\x1b[200~", "\x1b[201~"};
                const std::uint8_t k = n > 0 ? p[0] % 5 : 0;
                if (k < 3) {
                    h.D->OnOutput(
                        {reinterpret_cast<const std::byte*>(SNIPPETS[k]),
                         std::char_traits<char>::length(SNIPPETS[k])});
                } else {
                    h.Read(SNIPPETS[k]);
                }
                break;
            }
        }
    }
    // Time runs out: nothing stays held, nothing stays queued.
    h.Now += CPR_TIMEOUT;
    h.D->OnTick(h.Now, h);
    h.Settle(false, "");
    REQUIRE_INV(h.D->HeldSize() == 0);
    h.Now += CPR_FORGET;
    h.D->OnTick(h.Now, h);
    h.Settle(false, "");
    REQUIRE_INV(h.D->Off() || !h.D->Outstanding());
    REQUIRE_INV(h.D->HeldSize() == 0);
    REQUIRE_INV(h.Accounted == h.Input);
    // 5. Where the input is inside a bracketed paste, by a model that knows
    // only the two markers.
    if (!h.EverOff) {
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
    }
    return 0;
}

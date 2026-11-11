// CprDemux (design.md §9, "Разбор stdin"; §16, round C): the queue of expected
// answers, the end marker, the hold, the deadlines, bracketed paste.
#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "zet/core/time.hpp"
#include "zet/predict/cpr_demux.hpp"
#include "zet/predict/limits.hpp"

using namespace zet;
using namespace zet::predict;
using namespace std::chrono_literals;

namespace {

struct Rig {
    std::unique_ptr<CprDemux> D = CprDemux::Create();
    Timestamp Now = Timestamp::FromNanos(1'000'000'000);
    CprOutput Out;
    /// Everything for the agent so far, and every event.
    std::string Fwd;
    std::vector<CprEvent> Events;
    /// Bytes taken out of the stream, in order.
    std::string Stripped;

    Rig() { REQUIRE(D != nullptr); }

    static ByteSpan B(std::string_view s) {
        return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
    }
    void Take() {
        for (const std::byte b : Out.Forward) {
            Fwd.push_back(static_cast<char>(b));
        }
        Events.insert(Events.end(), Out.Events.begin(), Out.Events.end());
        for (const auto& strip : Out.Strips) {
            for (std::size_t i = 0; i < strip.Length; ++i) {
                Stripped.push_back(static_cast<char>(strip.Bytes[i]));
            }
        }
        Out.Clear();
    }
    /// Bytes from stdin; returns what went to the agent in this call.
    std::string In(std::string_view s) {
        const auto before = Fwd.size();
        D->OnInput(B(s), Now, Out);
        Take();
        return Fwd.substr(before);
    }
    std::string Tick() {
        const auto before = Fwd.size();
        D->OnTick(Now, Out);
        Take();
        return Fwd.substr(before);
    }
    void Advance(Duration d) { Now += d; }
    void Output(std::string_view s) { D->OnOutput(B(s)); }
    bool Query() { return D->OnQuerySent(Now); }
    void Resize() {
        D->OnResize(Now, Out);
        Take();
    }
    void Cancel() {
        D->OnCancel(Now, Out);
        Take();
    }
    std::size_t Count(ECprEvent k) const {
        std::size_t n = 0;
        for (const auto& e : Events) {
            n += e.Kind == k;
        }
        return n;
    }
};

std::string Pos(int r, int c) {
    return "\x1b[" + std::to_string(r) + ";" + std::to_string(c) + "R";
}
const std::string DA1 = "\x1b[?62;22c";

}  // namespace

TEST_CASE("our reply alone is stripped and reported") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.D->Pending());
    CHECK(r.In(Pos(5, 10) + DA1).empty());
    CHECK(r.Stripped == Pos(5, 10) + DA1);
    REQUIRE(r.Events.size() == 1);
    CHECK(r.Events[0].Kind == ECprEvent::Answered);
    CHECK(r.Events[0].Row == 5);
    CHECK(r.Events[0].Col == 10);
    CHECK_FALSE(r.D->Pending());
    CHECK_FALSE(r.D->Outstanding());
    CHECK_FALSE(r.D->Silent());
    CHECK_FALSE(r.D->NextDeadline().has_value());
    // Nothing of ours is outstanding: the same bytes are input now.
    CHECK(r.In(Pos(5, 10) + DA1) == Pos(5, 10) + DA1);
}

TEST_CASE("our reply split at every byte is held and stripped") {
    const std::string reply = Pos(12, 34) + DA1;
    for (std::size_t cut = 1; cut < reply.size(); ++cut) {
        CAPTURE(cut);
        Rig r;
        REQUIRE(r.Query());
        CHECK(r.In(reply.substr(0, cut)).empty());
        CHECK(r.D->HeldSize() == cut);
        r.Advance(1ms);
        CHECK(r.In(reply.substr(cut)).empty());
        CHECK(r.D->HeldSize() == 0);
        REQUIRE(r.Events.size() == 1);
        CHECK(r.Events[0].Kind == ECprEvent::Answered);
        CHECK(r.Events[0].Row == 12);
        CHECK(r.Events[0].Col == 34);
        CHECK(r.Fwd.empty());
    }
}

TEST_CASE("our reply one byte per read is stripped") {
    const std::string reply = Pos(1, 1) + DA1;
    Rig r;
    REQUIRE(r.Query());
    for (const char c : reply) {
        r.Advance(1ms);
        r.In(std::string(1, c));
    }
    CHECK(r.Fwd.empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

TEST_CASE("keys around and inside the read are forwarded in order") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("ab" + Pos(2, 3) + DA1 + "cd") == "abcd");
    CHECK(r.Count(ECprEvent::Answered) == 1);

    Rig s;
    REQUIRE(s.Query());
    CHECK(s.In("x" + Pos(2, 3).substr(0, 4)) == "x");
    CHECK(s.In(Pos(2, 3).substr(4) + DA1.substr(0, 3)).empty());
    CHECK(s.In(DA1.substr(3) + "yz") == "yz");
    CHECK(s.Fwd == "xyz");
}

TEST_CASE("arrow keys and other escape sequences pass at once") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b[A") == "\x1b[A");
    CHECK(r.In("\x1bOP") == "\x1bOP");
    CHECK(r.In("\x1b[1;5C") == "\x1b[1;5C");
    CHECK(r.D->HeldSize() == 0);
    CHECK(r.D->Pending());
}

TEST_CASE("Shift-F3 without the end marker is a key") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b[1;2Rx") == "\x1b[1;2Rx");
    CHECK(r.D->Pending());
    // The real answer is still ours.
    CHECK(r.In(Pos(7, 8) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

TEST_CASE("an answer that reads like Shift-F3 is ours with the end marker") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b[1;2R" + DA1).empty());
    REQUIRE(r.Events.size() == 1);
    CHECK(r.Events[0].Row == 1);
    CHECK(r.Events[0].Col == 2);
}

TEST_CASE(
    "a key between the position and the end marker leaves it unanswered") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In(Pos(3, 4) + "a" + DA1) == Pos(3, 4) + "a");
    CHECK(r.Count(ECprEvent::Failed) == 1);
    CHECK(r.D->Silent());
    CHECK_FALSE(r.D->Pending());
}

TEST_CASE("a position at the end of the read waits for the marker, then goes") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b[1;2R").empty());
    CHECK(r.D->HeldSize() == 6);
    r.Advance(CPR_HOLD);
    CHECK(r.Tick() == "\x1b[1;2R");
    CHECK(r.D->Pending());
}

TEST_CASE("the program's query is answered before ours") {
    Rig r;
    r.Output("vim\x1b[6n");
    REQUIRE(r.Query());
    // The terminal answers in order: the program's first.
    CHECK(r.In(Pos(9, 9)) == Pos(9, 9));
    CHECK(r.D->Pending());
    CHECK(r.In(Pos(2, 2) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
    CHECK(r.Events.back().Row == 2);
}

TEST_CASE("the program's query written after ours is answered after ours") {
    Rig r;
    REQUIRE(r.Query());
    r.Output("\x1b[6n\x1b[c");
    CHECK(r.In(Pos(2, 2) + DA1 + Pos(9, 9) + DA1) == Pos(9, 9) + DA1);
    CHECK(r.Count(ECprEvent::Answered) == 1);
    CHECK(r.D->QueueSize() == 0);
}

TEST_CASE(
    "the program's CPR and DA1 pair in front of ours both go to the agent") {
    Rig r;
    r.Output("\x1b[6n\x1b[c");
    REQUIRE(r.Query());
    const std::string prog = Pos(1, 1) + DA1;
    CHECK(r.In(prog + Pos(4, 5) + DA1) == prog);
    CHECK(r.Events.size() == 1);
    // Both DA1 forms count.
    Rig s;
    s.Output("\x1b[0c");
    REQUIRE(s.Query());
    CHECK(s.In(DA1 + Pos(4, 5) + DA1) == DA1);
}

TEST_CASE("the program's query split between output chunks counts once") {
    Rig r;
    r.Output("a\x1b[6");
    r.Output("nb");
    REQUIRE(r.Query());
    CHECK(r.In(Pos(9, 9) + Pos(2, 2) + DA1) == Pos(9, 9));
    CHECK(r.Count(ECprEvent::Answered) == 1);

    // Not counted before the last byte: nothing in the queue yet.
    Rig s;
    s.Output("\x1b[6");
    CHECK(s.D->QueueSize() == 0);
    s.Output("n");
    CHECK(s.D->QueueSize() == 1);
}

TEST_CASE("query text inside OSC and DCS strings is not the program's query") {
    Rig r;
    r.Output("\x1b]0;[6n[c\x07");
    r.Output("\x1bP1$r[6n[c\x1b\\");
    CHECK(r.D->QueueSize() == 0);
    REQUIRE(r.Query());
    CHECK(r.In(Pos(2, 2) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

TEST_CASE("DECXCPR from the program is not queued and its answer passes") {
    Rig r;
    r.Output("\x1b[?6n");
    CHECK(r.D->QueueSize() == 0);
    REQUIRE(r.Query());
    const std::string decxcpr = "\x1b[?4;5R";
    CHECK(r.In(decxcpr) == decxcpr);
    CHECK(r.In(Pos(2, 2) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

TEST_CASE("DA2 and a DA1 query with a parameter are not DA1") {
    Rig r;
    r.Output("\x1b[>c\x1b[1c");
    CHECK(r.D->QueueSize() == 0);
}

TEST_CASE("bracketed paste goes through byte for byte") {
    const std::string paste =
        "\x1b[200~" + Pos(1, 1) + DA1 + "text" + Pos(2, 2) + "\x1b[201~";
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In(paste) == paste);
    CHECK(r.D->Pending());
    CHECK(r.Events.empty());
    // After the paste the reply is ours again.
    CHECK(r.In(Pos(3, 3) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

TEST_CASE("a paste cut at every byte is forwarded byte for byte") {
    const std::string paste = "\x1b[200~" + Pos(1, 1) + DA1 + "ab\x1b[201~";
    for (std::size_t cut = 1; cut < paste.size(); ++cut) {
        CAPTURE(cut);
        Rig r;
        REQUIRE(r.Query());
        r.In(paste.substr(0, cut));
        r.Advance(1ms);
        r.In(paste.substr(cut));
        // The first read may be held; once the paste is closed nothing of it
        // is lost, however the cut fell.
        r.Advance(CPR_HOLD);
        r.Tick();
        CHECK(r.Fwd == paste);
        CHECK(r.Events.empty());
        CHECK(r.D->Pending());
    }
}

TEST_CASE("a paste start split across reads is recognised when not pending") {
    Rig r;
    CHECK(r.In("\x1b[2") == "\x1b[2");
    CHECK(r.In("00~") == "00~");
    REQUIRE(r.Query());
    CHECK(r.In(Pos(1, 1) + DA1) == Pos(1, 1) + DA1);
    CHECK(r.In("\x1b[201~") == "\x1b[201~");
    CHECK(r.In(Pos(2, 2) + DA1).empty());
}

TEST_CASE("a partial reply is held, then released after CPR_HOLD") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b[5;1").empty());
    const auto deadline = r.D->NextDeadline();
    REQUIRE(deadline.has_value());
    CHECK(*deadline == r.Now + CPR_HOLD);

    r.Advance(CPR_HOLD - 1ms);
    CHECK(r.Tick().empty());
    r.Advance(1ms);
    CHECK(r.Tick() == "\x1b[5;1");
    CHECK(r.D->HeldSize() == 0);

    // The rest of that reply is not ours any more.
    CHECK(r.In("0R") == "0R");
    // The marker finds our query without a position: unanswered.
    CHECK(r.In(DA1).empty());
    CHECK(r.Count(ECprEvent::Failed) == 1);
    CHECK(r.D->Silent());
}

TEST_CASE("the hold runs out at the next read too") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b[5").empty());
    r.Advance(CPR_HOLD);
    CHECK(r.In("x") == "\x1b[5x");
}

TEST_CASE("the hold is not extended by the following bytes of the same reply") {
    Rig r;
    REQUIRE(r.Query());
    const auto t0 = r.Now;
    CHECK(r.In("\x1b[").empty());
    r.Advance(30ms);
    CHECK(r.In("5;").empty());
    CHECK(*r.D->NextDeadline() == t0 + CPR_HOLD);
}

TEST_CASE("nothing is held when no query of ours is pending") {
    Rig r;
    CHECK(r.In("\x1b") == "\x1b");
    CHECK(r.In("\x1b[5;1") == "\x1b[5;1");
    CHECK_FALSE(r.D->NextDeadline().has_value());
}

TEST_CASE("an Esc typed alone waits for the hold and is then forwarded") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b").empty());
    CHECK(r.D->HeldSize() == 1);
    r.Advance(CPR_HOLD);
    CHECK(r.Tick() == "\x1b");
    // And the key after it goes at once.
    CHECK(r.In("j") == "j");
}

TEST_CASE("an Esc followed by a key is forwarded at once") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b").empty());
    CHECK(r.In("j") == "\x1bj");
    CHECK(r.D->HeldSize() == 0);
}

TEST_CASE("a timeout makes the terminal silent and a late reply is stripped") {
    Rig r;
    REQUIRE(r.Query());
    const auto deadline = r.D->NextDeadline();
    REQUIRE(deadline.has_value());
    CHECK(*deadline == r.Now + CPR_TIMEOUT);

    r.Advance(CPR_TIMEOUT - 1ms);
    r.Tick();
    CHECK_FALSE(r.D->Silent());
    r.Advance(1ms);
    r.Tick();
    CHECK(r.D->Silent());
    CHECK(r.Count(ECprEvent::TimedOut) == 1);
    CHECK_FALSE(r.D->Pending());
    CHECK(r.D->Outstanding());

    // The late reply is thrown away without an event.
    r.Advance(100ms);
    CHECK(r.In(Pos(2, 2) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 0);
    CHECK_FALSE(r.D->Outstanding());
    CHECK(r.D->Silent());
}

TEST_CASE("a read after the deadline sees the query already expired") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT);
    CHECK(r.In(Pos(2, 2) + DA1).empty());
    CHECK(r.Count(ECprEvent::TimedOut) == 1);
    CHECK(r.Count(ECprEvent::Answered) == 0);
}

TEST_CASE("after CPR_FORGET stdin passes through untouched") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    const auto forget = r.D->NextDeadline();
    REQUIRE(forget.has_value());
    CHECK(*forget == r.Now + CPR_FORGET);
    r.Advance(CPR_FORGET);
    r.Tick();
    CHECK(r.D->Off());
    CHECK(r.D->QueueSize() == 0);
    CHECK_FALSE(r.D->NextDeadline().has_value());
    CHECK(r.In(Pos(2, 2) + DA1) == Pos(2, 2) + DA1);
    // The program's queries are not counted either.
    r.Output("\x1b[6n");
    CHECK(r.D->QueueSize() == 0);
}

TEST_CASE("a resize clears silent and the query can be asked again") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    REQUIRE(r.D->Silent());

    r.Advance(1s);
    r.Resize();
    CHECK_FALSE(r.D->Silent());
    REQUIRE(r.Query());
    // The terminal answers the old query first, then the new one.
    CHECK(r.In(Pos(8, 8) + DA1 + Pos(3, 4) + DA1).empty());
    REQUIRE(r.Count(ECprEvent::Answered) == 1);
    CHECK(r.Events.back().Row == 3);
    CHECK(r.Events.back().Col == 4);
    CHECK_FALSE(r.D->Outstanding());
}

TEST_CASE("a resize after the queue was forgotten switches parsing back on") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    r.Advance(CPR_FORGET);
    r.Tick();
    REQUIRE(r.D->Off());
    r.Resize();
    CHECK_FALSE(r.D->Silent());
    REQUIRE(r.Query());
    CHECK_FALSE(r.D->Off());
    CHECK(r.In(Pos(3, 4) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

TEST_CASE("a resize abandons the pending query: its answer is thrown away") {
    Rig r;
    REQUIRE(r.Query());
    r.Resize();
    CHECK_FALSE(r.D->Pending());
    REQUIRE(r.Query());
    CHECK(r.In(Pos(8, 8) + DA1 + Pos(3, 4) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
    CHECK(r.Events.back().Row == 3);
}

TEST_CASE(
    "a cancelled query is stripped silently and does not make it silent") {
    Rig r;
    REQUIRE(r.Query());
    r.Cancel();
    CHECK(r.In(Pos(8, 8) + DA1).empty());
    CHECK(r.Events.empty());
    CHECK_FALSE(r.D->Silent());
}

TEST_CASE("cancelling releases a held beginning") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b[").empty());
    r.Cancel();
    CHECK(r.Fwd == "\x1b[");
}

TEST_CASE("a second query is refused while one is pending") {
    Rig r;
    REQUIRE(r.Query());
    CHECK_FALSE(r.Query());
    CHECK(r.D->QueueSize() == 1);
}

TEST_CASE("an end marker without a position is an unanswered query") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In(DA1).empty());
    CHECK(r.Count(ECprEvent::Failed) == 1);
    CHECK(r.D->Silent());
    CHECK_FALSE(r.D->Outstanding());
}

TEST_CASE("a program query the terminal never answered is skipped") {
    Rig r;
    r.Output("\x1b[c");
    REQUIRE(r.Query());
    CHECK(r.In(Pos(2, 2) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

TEST_CASE(
    "replies for the program pass as they are, and bytes with no query pass") {
    Rig r;
    r.Output("\x1b[6n");
    CHECK(r.In(Pos(4, 4)) == Pos(4, 4));
    CHECK(r.D->QueueSize() == 0);
    CHECK(r.In(Pos(4, 4)) == Pos(4, 4));
    CHECK(r.In(DA1) == DA1);
}

TEST_CASE("numbers out of range or malformed are input") {
    for (const char* text :
         {"\x1b[0;1R", "\x1b[1;0R", "\x1b[100000;1R", "\x1b[1;2;3R", "\x1b[;1R",
          "\x1b[?1;2R", "\x1b[1;1$R"}) {
        const std::string s = text;
        CAPTURE(s);
        Rig q;
        REQUIRE(q.Query());
        CHECK(q.In(s + DA1) == s);
        CHECK(q.Count(ECprEvent::Answered) == 0);
    }
}

TEST_CASE("the queue stays small however many queries the program sends") {
    Rig r;
    for (int i = 0; i < 1000; ++i) {
        r.Output("\x1b[6n");
    }
    CHECK(r.D->QueueSize() == 1);
    for (int i = 0; i < 1000; ++i) {
        r.Output("\x1b[6n\x1b[c");
    }
    CHECK(r.D->QueueSize() <= CPR_QUEUE_MAX);
    REQUIRE(r.Query());
    CHECK(r.D->QueueSize() <= CPR_QUEUE_MAX);
    CHECK(r.D->Pending());
}

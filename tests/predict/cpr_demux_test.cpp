// CprDemux (design.md §9, "Разбор stdin"; §16, round C): the DA1 anchor, the
// program's DA1 counts, the one own entry, the hold, the deadlines, bracketed
// paste.
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

struct Rig : CprSink {
    std::unique_ptr<CprDemux> D = CprDemux::Create();
    Timestamp Now = Timestamp::FromNanos(1'000'000'000);
    /// Everything for the agent so far, every event, and the bytes taken out.
    std::string Fwd;
    std::vector<CprEvent> Events;
    /// What Silent() said inside each Event callback.
    std::vector<bool> SilentInEvent;
    std::string Stripped;

    Rig() { REQUIRE(D != nullptr); }

    void Forward(ByteSpan bytes) noexcept override {
        for (const std::byte b : bytes) {
            Fwd.push_back(static_cast<char>(b));
        }
    }
    void Strip(ByteSpan bytes) noexcept override {
        for (const std::byte b : bytes) {
            Stripped.push_back(static_cast<char>(b));
        }
    }
    void Event(const CprEvent& event) noexcept override {
        Events.push_back(event);
        SilentInEvent.push_back(D->Silent());
    }

    static ByteSpan B(std::string_view s) {
        return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
    }
    /// Bytes from stdin; returns what went to the agent in this call.
    std::string In(std::string_view s) {
        const auto before = Fwd.size();
        D->OnInput(B(s), Now, *this);
        return Fwd.substr(before);
    }
    std::string Tick() {
        const auto before = Fwd.size();
        D->OnTick(Now, *this);
        return Fwd.substr(before);
    }
    void Advance(Duration d) { Now += d; }
    void Output(std::string_view s) { D->OnOutput(B(s), Now); }
    bool Query() { return D->OnQuerySent(Now, *this); }
    bool Can() { return D->CanQuery(Now, *this); }
    void Resize() { D->OnResize(Now, *this); }
    void Cancel() { D->OnCancel(Now, *this); }
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
std::string LongDa1(std::size_t size) {
    return "\x1b[?" + std::string(size - 4, '1') + "c";
}

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
    // CPR_HOLD does not release a complete position while the entry is live.
    r.Advance(CPR_HOLD);
    CHECK(r.Tick().empty());
    CHECK(r.D->HeldSize() == 6);
    // At the deadline the entry dies; CPR_HOLD counts from there.
    r.Advance(CPR_TIMEOUT - CPR_HOLD);
    CHECK(r.Tick().empty());
    CHECK(r.Count(ECprEvent::TimedOut) == 1);
    r.Advance(CPR_HOLD - 1ms);
    CHECK(r.Tick().empty());
    r.Advance(1ms);
    CHECK(r.Tick() == "\x1b[1;2R");
    CHECK(r.D->HeldSize() == 0);
}

TEST_CASE("the program's CPR in front of ours is not tracked and passes") {
    Rig r;
    r.Output("vim\x1b[6n");  // readline, bash: no DA1 follows
    CHECK(r.D->Ahead() == 0);
    REQUIRE(r.Query());
    // The terminal answers in order: the program's first.
    CHECK(r.In(Pos(9, 9) + Pos(2, 2) + DA1) == Pos(9, 9));
    CHECK(r.Count(ECprEvent::Answered) == 1);
    CHECK(r.Events.back().Row == 2);

    // The same when the program's reply is read alone: it waits for a DA1
    // that does not come, then goes.
    Rig s;
    s.Output("\x1b[6n");
    REQUIRE(s.Query());
    CHECK(s.In(Pos(9, 9)).empty());
    s.Advance(1ms);
    CHECK(s.In(Pos(2, 2) + DA1) == Pos(9, 9));
    CHECK(s.Count(ECprEvent::Answered) == 1);
}

TEST_CASE("the program's query written after ours is answered after ours") {
    Rig r;
    REQUIRE(r.Query());
    r.Output("\x1b[6n\x1b[c");
    CHECK(r.D->Behind() == 1);
    CHECK(r.D->Ahead() == 0);
    CHECK(r.In(Pos(2, 2) + DA1 + Pos(9, 9) + DA1) == Pos(9, 9) + DA1);
    CHECK(r.Count(ECprEvent::Answered) == 1);
    CHECK(r.D->Behind() == 0);
    CHECK(r.D->Ahead() == 0);
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
    r.Output("a\x1b[");
    r.Output("0");
    CHECK(r.D->Ahead() == 0);
    r.Output("cb");
    CHECK(r.D->Ahead() == 1);
    REQUIRE(r.Query());
    CHECK(r.In(DA1 + Pos(2, 2) + DA1) == DA1);
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

TEST_CASE("query text inside OSC and DCS strings is not the program's query") {
    Rig r;
    r.Output("\x1b]0;[6n[c\x07");
    r.Output("\x1bP1$r[6n[c\x1b\\");
    CHECK(r.D->Ahead() == 0);
    REQUIRE(r.Query());
    CHECK(r.In(Pos(2, 2) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

TEST_CASE("DECXCPR from the program is not counted and its answer passes") {
    Rig r;
    r.Output("\x1b[?6n");
    CHECK(r.D->Ahead() == 0);
    REQUIRE(r.Query());
    const std::string decxcpr = "\x1b[?4;5R";
    CHECK(r.In(decxcpr) == decxcpr);
    CHECK(r.In(Pos(2, 2) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

TEST_CASE("DA2, DA3 and a DA1 query with a nonzero parameter are not DA1") {
    Rig r;
    r.Output("\x1b[>c\x1b[=c\x1b[1c");
    CHECK(r.D->Ahead() == 0);
}

TEST_CASE("DA1 forms xterm answers count: CSI 0;1 c and ESC Z") {
    Rig r;
    r.Output("\x1b[0;1c");
    CHECK(r.D->Ahead() == 1);
    r.Output("\x1bZ");
    CHECK(r.D->Ahead() == 2);
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
    CHECK_FALSE(r.D->Outstanding());
    CHECK_FALSE(r.D->NextDeadline().has_value());
    CHECK(r.In(Pos(2, 2) + DA1) == Pos(2, 2) + DA1);
    CHECK(r.In("\x1b") == "\x1b");
    CHECK(r.D->HeldSize() == 0);
}

TEST_CASE(
    "while off the program's DA1 queries are still counted and answered") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    r.Advance(CPR_FORGET);
    r.Tick();
    REQUIRE(r.D->Off());
    r.Output("\x1b[6n\x1b[c\x1bZ");
    CHECK(r.D->Ahead() == 2);
    // Byte for byte, however the replies are cut; nothing is held.
    const std::string fish = Pos(9, 9) + DA1;
    CHECK(r.In(fish.substr(0, 9)) == fish.substr(0, 9));
    CHECK(r.In(fish.substr(9)) == fish.substr(9));
    CHECK(r.D->Ahead() == 1);
    CHECK(r.In(DA1) == DA1);
    CHECK(r.D->Ahead() == 0);
    // A reply that nobody asked for is no one's: the count stays at zero.
    CHECK(r.In(DA1) == DA1);
    CHECK(r.D->Ahead() == 0);
    // And text in a paste is not a reply.
    r.Output("\x1b[c");
    const std::string paste = "\x1b[200~" + DA1 + "\x1b[201~";
    CHECK(r.In(paste) == paste);
    CHECK(r.D->Ahead() == 1);
}

TEST_CASE("a program pair written while off is not ours after switching on") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    r.Advance(CPR_FORGET);
    r.Tick();
    REQUIRE(r.D->Off());
    r.Output("\x1b[6n\x1b[c");  // a prompt written while off, reply in flight
    r.Resize();
    REQUIRE(r.Query());
    const std::string fish = Pos(9, 9) + DA1;
    CHECK(r.In(fish + Pos(2, 2) + DA1) == fish);
    REQUIRE(r.Count(ECprEvent::Answered) == 1);
    CHECK(r.Events.back().Row == 2);
}

TEST_CASE("a resize clears silent; the retry waits for the late reply") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    REQUIRE(r.D->Silent());

    r.Advance(1s);
    r.Resize();
    CHECK_FALSE(r.D->Silent());
    // The dead entry is still the first in line: one own entry at a time.
    CHECK_FALSE(r.Query());
    CHECK(r.In(Pos(8, 8) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 0);
    REQUIRE(r.Query());
    CHECK(r.In(Pos(3, 4) + DA1).empty());
    REQUIRE(r.Count(ECprEvent::Answered) == 1);
    CHECK(r.Events.back().Row == 3);
    CHECK(r.Events.back().Col == 4);
    CHECK_FALSE(r.D->Outstanding());
}

TEST_CASE("a reply lost for good delays the retry on resize until CPR_FORGET") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT);
    r.Tick();  // lost, e.g. inside a paste
    r.Advance(1s);
    r.Resize();
    CHECK_FALSE(r.Query());
    r.Advance(CPR_FORGET - 1s - 1ms);
    r.Tick();
    CHECK_FALSE(r.Query());
    r.Advance(1ms);
    r.Tick();
    CHECK(r.D->Off());
    REQUIRE(r.Query());
    CHECK(r.In(Pos(3, 4) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

TEST_CASE("an own reply lost inside a paste ends as an expired entry") {
    Rig r;
    REQUIRE(r.Query());
    const std::string paste =
        "\x1b[200~" + std::string("text") + Pos(2, 2) + DA1 + "\x1b[201~";
    CHECK(r.In(paste) == paste);
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    CHECK(r.Count(ECprEvent::TimedOut) == 1);
    r.Resize();
    CHECK_FALSE(r.Query());
    r.Advance(CPR_FORGET);
    r.Tick();
    CHECK(r.D->Off());
    REQUIRE(r.Query());
    CHECK(r.In(Pos(3, 4) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

TEST_CASE("a resize after the entry was forgotten switches parsing back on") {
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
    CHECK(r.D->Outstanding());
    CHECK(r.In(Pos(8, 8) + DA1).empty());
    CHECK(r.Events.empty());
    REQUIRE(r.Query());
    CHECK(r.In(Pos(3, 4) + DA1).empty());
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

TEST_CASE("cancelling does not release a held beginning early") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b[").empty());
    r.Cancel();
    CHECK(r.Fwd.empty());
    CHECK(r.D->HeldSize() == 2);
    // The cancelled entry is dead, and the beginning is still its own.
    r.Advance(1ms);
    CHECK(r.In("5;1R" + DA1).empty());
    CHECK(r.Events.empty());

    Rig s;
    REQUIRE(s.Query());
    CHECK(s.In("\x1b[").empty());
    s.Cancel();
    s.Advance(CPR_HOLD);
    CHECK(s.Tick() == "\x1b[");
}

TEST_CASE("a timeout does not release a held beginning early either") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT - 10ms);
    CHECK(r.In("\x1b[1").empty());
    r.Advance(10ms);
    r.Tick();  // the entry died; the hold has 40 ms left
    CHECK(r.Count(ECprEvent::TimedOut) == 1);
    CHECK(r.Fwd.empty());
    CHECK(r.D->HeldSize() == 3);
    r.Advance(1ms);
    CHECK(r.In("2;5R" + DA1).empty());  // the late reply, stripped
    CHECK(r.Fwd.empty());
}

TEST_CASE("a second query is refused while an own entry exists") {
    Rig r;
    REQUIRE(r.Query());
    CHECK_FALSE(r.Query());
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    CHECK_FALSE(r.Query());  // dead, not gone
}

TEST_CASE("an end marker without a position is an unanswered query") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In(DA1).empty());
    CHECK(r.Count(ECprEvent::Failed) == 1);
    CHECK(r.D->Silent());
    CHECK_FALSE(r.D->Outstanding());
}

TEST_CASE("a lost reply of the program keeps our reply from being claimed") {
    // The terminal never answers the program's DA1. Our reply then reads as
    // the program's: forwarded, and our query expires. The count ages out.
    Rig r;
    r.Output("\x1b[c");
    REQUIRE(r.Query());
    CHECK(r.In(Pos(2, 2) + DA1) == Pos(2, 2) + DA1);
    CHECK(r.D->Ahead() == 0);
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    CHECK(r.Count(ECprEvent::TimedOut) == 1);
}

TEST_CASE("a count of the program's queries expires after TIMEOUT + FORGET") {
    const Duration span = CPR_TIMEOUT + CPR_FORGET;
    Rig r;
    r.Output("\x1b[c");
    r.Advance(span - 1ms);
    r.Tick();
    CHECK(r.D->Ahead() == 1);
    r.Advance(1ms);
    r.Tick();
    CHECK(r.D->Ahead() == 0);
    REQUIRE(r.Query());
    CHECK(r.In(Pos(2, 2) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

TEST_CASE("a count ages by its oldest query, one dropped per span") {
    const Duration span = CPR_TIMEOUT + CPR_FORGET;
    Rig r;
    r.Output("\x1b[c");
    r.Advance(span - 1ms);
    r.Output("\x1b[c");  // a young query does not keep the old one alive
    CHECK(r.D->Ahead() == 2);
    r.Advance(1ms);
    r.Tick();
    CHECK(r.D->Ahead() == 1);
    // The other one is a span old a span after it was written.
    r.Advance(span - 2ms);
    r.Tick();
    CHECK(r.D->Ahead() == 1);
    r.Advance(1ms);
    r.Tick();
    CHECK(r.D->Ahead() == 0);
}

TEST_CASE("a reply that is answered does not reset the clock of the count") {
    // fish asks at every prompt: the count is never zero. One lost reply must
    // not live as long as the user is active.
    Rig r;
    r.Output("\x1bZ");  // never answered
    for (int i = 1; i <= 6; ++i) {
        r.Advance(5s);
        r.Output("\x1b[6n\x1b[c");
        const std::string fish = Pos(i, 3) + DA1;
        CHECK(r.In(fish) == fish);
    }
    CHECK(r.D->Ahead() == 0);
    r.Advance(100ms);
    REQUIRE(r.Query());
    CHECK(r.In(Pos(9, 1) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

TEST_CASE("the paste residual does not misalign the next own query") {
    // Our reply is lost inside a paste and fish's takes its place: Behind of
    // that moment becomes Ahead, and fish keeps it fresh. It still ages out.
    Rig r;
    REQUIRE(r.Query());
    r.Output("\x1b[6n\x1b[c");
    const std::string paste = "\x1b[200~x" + Pos(2, 2) + DA1 + "\x1b[201~";
    CHECK(r.In(paste) == paste);
    r.In(Pos(3, 1) + DA1);
    for (int i = 0; i < 6; ++i) {
        r.Advance(5s);
        r.Output("\x1b[6n\x1b[c");
        const std::string fish = Pos(4, 1) + DA1;
        CHECK(r.In(fish) == fish);
    }
    r.Advance(1s);
    r.Resize();
    REQUIRE(r.Query());
    r.Output("\x1b[6n\x1b[c");
    const std::string fish = Pos(5, 1) + DA1;
    CHECK(r.In(Pos(9, 1) + DA1 + fish) == fish);
    REQUIRE_FALSE(r.Events.empty());
    CHECK(r.Events.back().Row == 9);
}

TEST_CASE("a lost reply expires the count while our query waits") {
    Rig r;
    r.Output("\x1b[c");
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    r.Advance(CPR_FORGET);
    r.Tick();
    CHECK(r.D->Off());
    CHECK(r.D->Ahead() == 0);
}

TEST_CASE(
    "the queries written after the forgotten entry are ahead of a new one") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(10ms);
    r.Output("\x1b[c");
    CHECK(r.D->Behind() == 1);
    r.Advance(CPR_TIMEOUT - 10ms);
    r.Tick();
    r.Advance(CPR_FORGET - 1ms);
    r.Output("\x1b[c");  // keeps the count young
    r.Advance(1ms);
    r.Tick();
    REQUIRE(r.D->Off());
    CHECK(r.D->Behind() == 0);
    CHECK(r.D->Ahead() == 2);
}

TEST_CASE(
    "replies for the program pass as they are, and bytes with no query pass") {
    Rig r;
    r.Output("\x1b[6n");
    CHECK(r.In(Pos(4, 4)) == Pos(4, 4));
    CHECK(r.In(Pos(4, 4)) == Pos(4, 4));
    CHECK(r.In(DA1) == DA1);
    r.Output("\x1b[c");
    CHECK(r.In(DA1) == DA1);
    CHECK(r.D->Ahead() == 0);
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

TEST_CASE("the state does not depend on how many queries the program sends") {
    Rig r;
    for (int i = 0; i < 1000; ++i) {
        r.Output("\x1b[6n\x1b[c");
    }
    CHECK(r.D->Ahead() == 1000);
    REQUIRE(r.Query());
    for (int i = 0; i < 500; ++i) {
        r.Output("\x1b[c");
    }
    CHECK(r.D->Behind() == 500);
    CHECK(r.D->Pending());
}

// ---------------------------------------------------------------------------
// The review of the queue model (RV1..RV9), under the DA1 anchor
// ---------------------------------------------------------------------------

TEST_CASE(
    "a long DA1 reply (xterm vt420, Windows Terminal) is the end marker") {
    for (const std::string& da1 :
         {std::string("\x1b[?64;1;2;6;9;15;16;17;18;21;22;28;29c"),
          std::string("\x1b[?61;4;6;7;14;21;22;23;24;28;32;42;52c")}) {
        CAPTURE(da1.size());
        Rig r;
        REQUIRE(r.Query());
        CHECK(r.In(Pos(5, 1) + da1).empty());
        CHECK(r.Count(ECprEvent::Answered) == 1);
    }
}

TEST_CASE("a DA1 reply up to the limit is recognised, one byte more is not") {
    const auto reply = [](std::size_t size) {
        // ESC [ ? <digits> c is size bytes in all.
        return "\x1b[?" + std::string(size - 4, '1') + "c";
    };
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In(Pos(5, 1) + reply(CPR_DA1_REPLY_MAX)).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);

    Rig s;
    REQUIRE(s.Query());
    const std::string tooLong = reply(CPR_DA1_REPLY_MAX + 1);
    CHECK(s.In(Pos(5, 1) + tooLong) == Pos(5, 1) + tooLong);
    CHECK(s.Count(ECprEvent::Answered) == 0);
    CHECK(s.D->Pending());
}

TEST_CASE("a long reply cut at every byte is held and stripped") {
    const std::string reply =
        Pos(12, 34) + "\x1b[?64;1;2;6;9;15;16;17;18;21;22;28;29c";
    for (std::size_t cut = 1; cut < reply.size(); ++cut) {
        CAPTURE(cut);
        Rig r;
        REQUIRE(r.Query());
        CHECK(r.In(reply.substr(0, cut)).empty());
        r.Advance(1ms);
        CHECK(r.In(reply.substr(cut)).empty());
        CHECK(r.Count(ECprEvent::Answered) == 1);
        CHECK(r.Fwd.empty());
    }
}

TEST_CASE("Shift-F3 in front of the program's pair does not give us the pair") {
    Rig r;
    r.Output("\x1b[6n\x1b[c");  // fish, before our query
    REQUIRE(r.Query());
    const std::string f3 = "\x1b[1;2R";
    const std::string fish = Pos(9, 9) + DA1;
    const std::string ours = Pos(2, 2) + DA1;
    CHECK(r.In(f3 + fish + ours) == f3 + fish);
    REQUIRE(r.Count(ECprEvent::Answered) == 1);
    CHECK(r.Events.back().Row == 2);
}

TEST_CASE("many pairs of the program in front of ours are all the program's") {
    Rig r;
    std::string fish;
    for (int i = 1; i <= 100; ++i) {
        r.Output("\x1b[6n\x1b[c");  // fish prompts to a slow terminal
        fish += Pos(i, 1) + DA1;
    }
    REQUIRE(r.Query());
    CHECK(r.In(fish + Pos(40, 2) + DA1) == fish);
    REQUIRE(r.Count(ECprEvent::Answered) == 1);
    CHECK(r.Events.back().Row == 40);
}

TEST_CASE("a burst of resizes to a slow terminal keeps replies aligned") {
    Rig r;
    REQUIRE(r.Query());
    for (int i = 1; i <= 33; ++i) {
        r.Resize();
        CHECK_FALSE(r.Query());  // one own entry; the dead one comes first
        r.Advance(10ms);
    }
    CHECK(r.In(Pos(1, 1) + DA1).empty());  // the only reply there is
    CHECK(r.Events.empty());
    REQUIRE(r.Query());
    CHECK(r.In(Pos(33, 1) + DA1).empty());
    REQUIRE(r.Count(ECprEvent::Answered) == 1);
    CHECK(r.Events.back().Row == 33);
}

TEST_CASE("DECID (ESC Z) is answered with DA1: the reply is the program's") {
    Rig r;
    r.Output("\x1bZ");
    REQUIRE(r.Query());
    CHECK(r.In(DA1 + Pos(2, 2) + DA1) == DA1);
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

TEST_CASE("held bytes are held no longer than CPR_HOLD after they arrived") {
    Rig r;
    REQUIRE(r.Query());
    const auto t0 = r.Now;
    CHECK(r.In("\x1b[1;2R\x1b[").empty());  // Shift-F3, then Alt-[
    r.Advance(40ms);
    CHECK(r.In("1") == "\x1b[1;2R");
    REQUIRE(r.D->NextDeadline().has_value());
    CHECK(*r.D->NextDeadline() <= t0 + CPR_HOLD);
    r.Advance(10ms);
    CHECK(r.Tick() == "\x1b[1");
}

TEST_CASE("a late reply split across reads does not leak") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    r.Advance(100ms);
    CHECK(r.In("\x1b[1").empty());
    r.Advance(1ms);
    CHECK(r.In("2;5R" + DA1).empty());
    CHECK(r.Fwd.empty());
    CHECK_FALSE(r.D->Outstanding());
}

TEST_CASE(
    "the rest of a reply after the hold ran out still answers the entry") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b[?6").empty());
    r.Advance(CPR_HOLD);
    CHECK(r.Tick() == "\x1b[?6");
    // The terminal did answer, late; the beginning is gone, the entry is not.
    CHECK(r.In("2c") == "2c");
    CHECK(r.Count(ECprEvent::Failed) == 1);
    CHECK_FALSE(r.D->Outstanding());
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

TEST_CASE("F3 with modifiers is a key in every state") {
    const std::string keys[] = {"\x1b[1;2R", "\x1b[1;5R", "\x1b[1;3R",
                                "\x1b[1;16R"};
    for (const std::string& f3 : keys) {
        CAPTURE(f3);
        {  // no entry
            Rig r;
            CHECK(r.In(f3) == f3);
            CHECK(r.D->HeldSize() == 0);
        }
        {  // live
            Rig r;
            REQUIRE(r.Query());
            CHECK(r.In(f3 + "x") == f3 + "x");
            CHECK(r.D->Pending());
            // At the end of a read it waits for a DA1.
            CHECK(r.In(f3).empty());
            r.Advance(CPR_HOLD);
            CHECK(r.Tick().empty());
            // The next bytes are not a DA1 reply: it is a key, and goes now.
            // Our reply behind it is still ours.
            CHECK(r.In(Pos(7, 8) + DA1) == f3);
            CHECK(r.Count(ECprEvent::Answered) == 1);
        }
        {  // dead
            Rig r;
            REQUIRE(r.Query());
            r.Advance(CPR_TIMEOUT);
            r.Tick();
            CHECK(r.In(f3 + "x") == f3 + "x");
            CHECK(r.D->Outstanding());
            CHECK(r.In(Pos(7, 8) + DA1).empty());
            CHECK(r.Count(ECprEvent::Answered) == 0);
        }
        {  // the program's DA1 reply is ahead
            Rig r;
            r.Output("\x1b[c");
            REQUIRE(r.Query());
            CHECK(r.In(f3) == f3);
            CHECK(r.D->HeldSize() == 0);
            CHECK(r.D->Ahead() == 1);
            // A DA1 right after it is the program's.
            CHECK(r.In(DA1) == DA1);
            CHECK(r.D->Ahead() == 0);
            CHECK(r.D->Pending());
        }
    }
}

TEST_CASE("bash with a lost reply: its CSI 6 n changes nothing for us") {
    Rig r;
    r.Output("\x1b[6n");  // readline asks for the position, no DA1 follows
    REQUIRE(r.Query());
    r.Output("\x1b[6n");  // and again after our query
    // The terminal lost the first answer; ours and the second one come.
    CHECK(r.In(Pos(2, 2) + DA1 + Pos(9, 9)) == Pos(9, 9));
    CHECK(r.Count(ECprEvent::Answered) == 1);
    CHECK(r.Events.back().Row == 2);
    CHECK(r.D->Ahead() == 0);
}

TEST_CASE("a terminal that answers the position but not DA1") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In(Pos(2, 2)).empty());
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    r.Advance(CPR_HOLD);
    CHECK(r.Tick() == Pos(2, 2));  // it looked like a key
    CHECK(r.Count(ECprEvent::TimedOut) == 1);
    CHECK(r.Count(ECprEvent::Answered) == 0);
    CHECK(r.D->Silent());
}

TEST_CASE("a terminal that answers DA1 but not the position") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In(DA1).empty());
    CHECK(r.Count(ECprEvent::Failed) == 1);
    CHECK(r.Count(ECprEvent::Answered) == 0);
    CHECK(r.D->Silent());
    CHECK_FALSE(r.D->Outstanding());
}

TEST_CASE("holding stops while the program's DA1 reply is ahead") {
    Rig r;
    r.Output("\x1b[c");
    REQUIRE(r.Query());
    CHECK(r.In("\x1b") == "\x1b");
    CHECK(r.In("\x1b[") == "\x1b[");
    CHECK(r.D->HeldSize() == 0);
    // The reply split across reads is still told apart.
    CHECK(r.In("\x1b[?6") == "\x1b[?6");
    CHECK(r.In("2c") == "2c");
    CHECK(r.D->Ahead() == 0);
    // Now the next one is ours and is held.
    CHECK(r.In("\x1b").empty());
}

TEST_CASE("our reply is claimed only by the DA1 that follows the program's") {
    Rig r;
    r.Output("\x1b[6n\x1b[c");
    REQUIRE(r.Query());
    r.Output("\x1b[6n\x1b[c");  // behind us
    const std::string a = Pos(9, 9) + DA1;
    const std::string b = Pos(2, 2) + DA1;
    const std::string c = Pos(7, 7) + DA1;
    CHECK(r.In(a + b + c) == a + c);
    CHECK(r.Count(ECprEvent::Answered) == 1);
    CHECK(r.Events.back().Row == 2);
    CHECK(r.D->Ahead() == 0);
    CHECK(r.D->Behind() == 0);
}

// Cases that kill the mutants found in review.

// Kills: Consume does not promote Behind_ to Ahead_.
TEST_CASE("after our reply the queries written behind it are first in line") {
    Rig r;
    REQUIRE(r.Query());
    r.Output("\x1b[c");  // behind
    CHECK(r.In(Pos(2, 2) + DA1).empty());
    CHECK(r.D->Ahead() == 1);
    // A new query of ours before the program's reply arrives: the next DA1 is
    // the program's, not ours.
    REQUIRE(r.Query());
    CHECK(r.In(DA1) == DA1);
    CHECK(r.D->Pending());
    CHECK(r.In(Pos(3, 3) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 2);
}

// Kills: promotion at forget keeps AheadAt_ instead of taking BehindAt_.
TEST_CASE("a count promoted by forgetting ages from its newest query") {
    const Duration span = CPR_TIMEOUT + CPR_FORGET;
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    r.Advance(CPR_FORGET - 5s);
    r.Output("\x1b[c");  // behind, written 5 s before the entry is forgotten
    r.Advance(5s);
    r.Tick();
    REQUIRE(r.D->Off());
    REQUIRE(r.D->Ahead() == 1);
    r.Advance(span - 5s - 1ms);
    r.Tick();
    CHECK(r.D->Ahead() == 1);
    r.Advance(1ms);
    r.Tick();
    CHECK(r.D->Ahead() == 0);
}

// Kills: Behind_ expiry at > instead of >=.
TEST_CASE(
    "a count behind the entry written with it is gone when it is forgotten") {
    const Duration span = CPR_TIMEOUT + CPR_FORGET;
    Rig r;
    REQUIRE(r.Query());
    r.Output("\x1b[c");
    r.Advance(span);
    r.Tick();
    CHECK(r.D->Off());
    CHECK(r.D->Ahead() == 0);
    CHECK(r.D->Behind() == 0);
}

// Kills: the hold runs out (>) instead of at (>=) CPR_HOLD, via NextDeadline.
TEST_CASE("a tick exactly at CPR_HOLD releases, the deadline says so") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b[").empty());
    r.Advance(CPR_HOLD);
    CHECK(r.Tick() == "\x1b[");
}

// Kills: PasteEndMatched_ not reset on a mismatch.
TEST_CASE("a half end marker in a paste does not end it later") {
    Rig r;
    REQUIRE(r.Query());
    const std::string lookalike = Pos(1, 1) + DA1;
    const std::string paste = "\x1b[200~\x1b[20X1~" + lookalike + "\x1b[201~";
    CHECK(r.In(paste) == paste);
    CHECK(r.D->Pending());
}

// Kills: an ESC in the middle of a half end marker is not a new beginning.
TEST_CASE("ESC ESC and the end marker in a paste end it") {
    Rig r;
    REQUIRE(r.Query());
    const std::string paste = "\x1b[200~a\x1b\x1b[201~";
    CHECK(r.In(paste) == paste);
    CHECK(r.In(Pos(7, 7) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

// Kills: the Scan cap on a lone DA1 reply (no position in front).
TEST_CASE("a lone DA1 reply up to the cap is ours, one more is not") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In(LongDa1(CPR_DA1_REPLY_MAX)).empty());
    CHECK(r.Count(ECprEvent::Failed) == 1);

    Rig s;
    REQUIRE(s.Query());
    const std::string tooLong = LongDa1(CPR_DA1_REPLY_MAX + 1);
    CHECK(s.In(tooLong) == tooLong);
    CHECK(s.D->Pending());

    Rig t;
    REQUIRE(t.Query());
    CHECK(t.In(LongDa1(38)).empty());  // a real vt420 reply, mutant cap 20
    CHECK(t.Count(ECprEvent::Failed) == 1);
}

// Kills: the cap in the look-ahead after a position, at the exact size.
TEST_CASE("a DA1 reply of exactly the cap after a position, split in two") {
    Rig r;
    REQUIRE(r.Query());
    const std::string da = LongDa1(CPR_DA1_REPLY_MAX);
    CHECK(r.In(Pos(3, 3) + da.substr(0, 127)).empty());
    r.Advance(1ms);
    CHECK(r.In(da.substr(127)).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

// Kills: an unterminated DA1 prefix of exactly the cap is still held.
TEST_CASE("an unterminated DA1 prefix of CPR_DA1_REPLY_MAX bytes is not held") {
    Rig r;
    REQUIRE(r.Query());
    const std::string prefix =
        "\x1b[?" + std::string(CPR_DA1_REPLY_MAX - 3, '1');
    REQUIRE(prefix.size() == CPR_DA1_REPLY_MAX);
    CHECK(r.In(prefix) == prefix);
    CHECK(r.D->HeldSize() == 0);
    // And one byte short of it is still held.
    Rig s;
    REQUIRE(s.Query());
    CHECK(s.In(prefix.substr(0, CPR_DA1_REPLY_MAX - 1)).empty());
    CHECK(s.D->HeldSize() == CPR_DA1_REPLY_MAX - 1);
}

// Kills: OnOutput does not expire an old count before adding to it.
TEST_CASE("a new query does not revive a count that has aged out") {
    Rig r;
    r.Output("\x1b[c");
    r.Advance(CPR_TIMEOUT + CPR_FORGET);
    r.Output("\x1b[c");  // no tick in between
    CHECK(r.D->Ahead() == 1);
}

// Kills: forgetting the entry does not release a beginning held a moment ago.
TEST_CASE(
    "a beginning held just before CPR_FORGET is released with the entry") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    r.Advance(CPR_FORGET - 10ms);
    CHECK(r.In("\x1b[").empty());
    r.Advance(10ms);
    CHECK(r.Tick() == "\x1b[");
    CHECK(r.D->Off());
    CHECK(r.D->HeldSize() == 0);
}

// Kills: more than five digits accepted in a position.
TEST_CASE("six digits with leading zeros are not a position") {
    Rig r;
    REQUIRE(r.Query());
    const std::string text = "\x1b[000001;000001R" + DA1;
    CHECK(r.In(text) == "\x1b[000001;000001R");
    // The DA1 alone is ours, without a position: unanswered.
    CHECK(r.Count(ECprEvent::Failed) == 1);
}

// Kills: CSI 0200~ taken for a paste start.
TEST_CASE("CSI 0200~ is not a bracketed paste") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b[0200~").empty() == false);
    CHECK(r.In(Pos(4, 4) + DA1).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
}

// Kills: the first number of a position taken with six digits.
TEST_CASE("a six-digit first number is not a position either") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b[000001;1R" + DA1) == "\x1b[000001;1R");
    CHECK(r.Count(ECprEvent::Failed) == 1);
}

// Kills: look-ahead holds a DA1 prefix that already reached the cap.
TEST_CASE("a position followed by an unterminated DA1 prefix of the cap goes") {
    Rig r;
    REQUIRE(r.Query());
    const std::string prefix =
        "\x1b[?" + std::string(CPR_DA1_REPLY_MAX - 3, '1');
    CHECK(r.In(Pos(3, 3) + prefix) == Pos(3, 3) + prefix);
    CHECK(r.D->HeldSize() == 0);
}

// ---------------------------------------------------------------------------
// A complete position is held for its DA1 reply while the entry is live
// ---------------------------------------------------------------------------

TEST_CASE("a position 300 ms before its DA1 reply is ours") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In(Pos(5, 6)).empty());
    r.Advance(300ms);
    CHECK(r.Tick().empty());
    CHECK(r.D->HeldSize() == Pos(5, 6).size());
    CHECK(r.In(DA1).empty());
    CHECK(r.Fwd.empty());
    REQUIRE(r.Count(ECprEvent::Answered) == 1);
    CHECK(r.Events.back().Row == 5);
    CHECK(r.Events.back().Col == 6);
    CHECK_FALSE(r.D->Outstanding());
    CHECK(r.D->HeldSize() == 0);
}

TEST_CASE("a position followed by a part of the DA1 reply waits for the rest") {
    Rig r;
    REQUIRE(r.Query());
    const std::string da = DA1;
    CHECK(r.In(Pos(5, 6) + da.substr(0, 4)).empty());
    r.Advance(300ms);
    CHECK(r.Tick().empty());
    CHECK(r.In(da.substr(4)).empty());
    CHECK(r.Count(ECprEvent::Answered) == 1);
    CHECK(r.Fwd.empty());
}

TEST_CASE("a position followed by a plain key is forwarded at once") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b[1;2R").empty());
    r.Advance(1ms);
    CHECK(r.In("a") == "\x1b[1;2Ra");
    CHECK(r.D->HeldSize() == 0);
    CHECK(r.D->Pending());
}

TEST_CASE("the hold of a complete position ends at the entry's deadline") {
    Rig r;
    REQUIRE(r.Query());
    const Timestamp deadline = r.Now + CPR_TIMEOUT;
    r.Advance(100ms);
    CHECK(r.In("\x1b[1;2R").empty());
    REQUIRE(r.D->NextDeadline().has_value());
    CHECK(*r.D->NextDeadline() == deadline);
    // The entry dies; CPR_HOLD counts from the deadline.
    r.Now = deadline;
    CHECK(r.Tick().empty());
    CHECK(r.Count(ECprEvent::TimedOut) == 1);
    REQUIRE(r.D->NextDeadline().has_value());
    CHECK(*r.D->NextDeadline() == deadline + CPR_HOLD);
    r.Now = deadline + CPR_HOLD - 1ms;
    CHECK(r.Tick().empty());
    r.Now = deadline + CPR_HOLD;
    CHECK(r.Tick() == "\x1b[1;2R");
}

TEST_CASE("a DA1 reply inside the hold after the deadline is a late reply") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(100ms);
    CHECK(r.In(Pos(5, 6)).empty());
    r.Advance(CPR_TIMEOUT - 100ms + CPR_HOLD - 1ms);
    CHECK(r.In(DA1).empty());
    CHECK(r.Count(ECprEvent::TimedOut) == 1);
    CHECK(r.Count(ECprEvent::Answered) == 0);
    CHECK(r.Fwd.empty());
    CHECK_FALSE(r.D->Pending());
}

TEST_CASE("with a dead entry a complete position holds CPR_HOLD from arrival") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    r.Advance(1s);
    CHECK(r.In("\x1b[1;2R").empty());
    REQUIRE(r.D->NextDeadline().has_value());
    CHECK(*r.D->NextDeadline() == r.Now + CPR_HOLD);
    r.Advance(CPR_HOLD);
    CHECK(r.Tick() == "\x1b[1;2R");
}

TEST_CASE("partial beginnings keep CPR_HOLD while the entry is live") {
    for (const char* text :
         {"\x1b", "\x1b[", "\x1b[12;", "\x1b[12", "\x1b[?6"}) {
        CAPTURE(text);
        Rig r;
        REQUIRE(r.Query());
        CHECK(r.In(text).empty());
        REQUIRE(r.D->NextDeadline().has_value());
        CHECK(*r.D->NextDeadline() == r.Now + CPR_HOLD);
        r.Advance(CPR_HOLD);
        CHECK(r.Tick() == text);
    }
}

TEST_CASE("a cancelled entry's held position has CPR_HOLD only") {
    Rig r;
    REQUIRE(r.Query());
    CHECK(r.In("\x1b[1;2R").empty());
    r.Advance(CPR_HOLD);
    r.Cancel();
    CHECK(r.Fwd == "\x1b[1;2R");
}

// ---------------------------------------------------------------------------
// The API contract
// ---------------------------------------------------------------------------

TEST_CASE("CanQuery is false while the entry is live or dead, true once gone") {
    Rig r;
    CHECK(r.Can());
    REQUIRE(r.Query());
    CHECK_FALSE(r.Can());
    r.Advance(CPR_TIMEOUT);
    CHECK_FALSE(r.Can());  // dead
    CHECK(r.Count(ECprEvent::TimedOut) == 1);
    r.Advance(CPR_FORGET - 1ms);
    CHECK_FALSE(r.Can());
    r.Advance(1ms);
    CHECK(r.Can());  // forgotten by the call itself, no tick before it
    CHECK(r.D->Off());
}

TEST_CASE("CanQuery is true again after the late reply of a dead entry") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    r.In(Pos(2, 2) + DA1);
    CHECK(r.Can());
}

TEST_CASE("CanQuery releases what the forgetting releases") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT);
    r.Tick();
    r.Advance(CPR_FORGET - 10ms);
    CHECK(r.In("\x1b[").empty());
    r.Advance(10ms);
    CHECK(r.Can());
    CHECK(r.Fwd == "\x1b[");
}

TEST_CASE("OnQuerySent settles first: no tick is needed after CPR_FORGET") {
    Rig r;
    REQUIRE(r.Query());
    r.Advance(CPR_TIMEOUT + CPR_FORGET);
    CHECK(r.Query());
    CHECK(r.Count(ECprEvent::TimedOut) == 1);
    CHECK(r.D->Pending());
}

TEST_CASE("Silent is already set inside the event callback") {
    {
        Rig r;
        REQUIRE(r.Query());
        r.Advance(CPR_TIMEOUT);
        r.Tick();
        REQUIRE(r.Events.size() == 1);
        CHECK(r.Events[0].Kind == ECprEvent::TimedOut);
        CHECK(r.SilentInEvent[0]);
    }
    {
        Rig r;
        REQUIRE(r.Query());
        r.In(DA1);
        REQUIRE(r.Events.size() == 1);
        CHECK(r.Events[0].Kind == ECprEvent::Failed);
        CHECK(r.SilentInEvent[0]);
    }
    {
        Rig r;
        REQUIRE(r.Query());
        r.In(Pos(2, 2) + DA1);
        REQUIRE(r.Events.size() == 1);
        CHECK(r.Events[0].Kind == ECprEvent::Answered);
        CHECK_FALSE(r.SilentInEvent[0]);
    }
}

TEST_CASE("a reply that starts behind an expired position keeps its own hold") {
    // The program asked `CSI 6 n` alone, so the terminal's answer to it is a
    // position that looks like ours. Our entry was abandoned by a resize (dead:
    // the hold is CPR_HOLD from arrival). The program's position expires while
    // the first byte of our own reply, which arrived 2 ms after it, is held
    // behind it: only the position goes, and the rest of our reply is stripped.
    Rig r;
    REQUIRE(r.Query());
    r.Resize();
    CHECK(r.In(Pos(28, 10)).empty());
    r.Advance(2ms);
    CHECK(r.In("\x1b").empty());
    r.Advance(CPR_HOLD - 2ms);
    CHECK(r.Tick() == Pos(28, 10));
    CHECK(r.D->HeldSize() == 1);
    CHECK(r.In(Pos(1, 1).substr(1) + DA1).empty());
    CHECK(r.Fwd == Pos(28, 10));
    CHECK(r.Stripped == Pos(1, 1) + DA1);
    CHECK_FALSE(r.D->Outstanding());
}

TEST_CASE("a tail as old as its position's hold goes with the position") {
    Rig r;
    REQUIRE(r.Query());
    r.Resize();
    CHECK(r.In(Pos(28, 10) + "\x1b[").empty());
    r.Advance(CPR_HOLD);
    CHECK(r.Tick() == Pos(28, 10) + "\x1b[");
    CHECK(r.D->HeldSize() == 0);
}

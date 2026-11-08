// A mirror grid plus the engine watching it, driven the way the client will
// drive them: output is fed to the mirror and then announced, input is
// announced as it is sent, and an EchoAck covers everything so far.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "zet/core/time.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"
#include "zet/predict/engine.hpp"

namespace zet::predict::test {

inline std::unique_ptr<grid::Grid> MakeGrid(int cols, int rows) {
    auto r = grid::Grid::Create(static_cast<std::uint16_t>(cols),
                                static_cast<std::uint16_t>(rows), 0);
    return std::unique_ptr<grid::Grid>(r.has_value() ? r.value() : nullptr);
}

inline ByteSpan Bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

struct Rig {
    std::unique_ptr<grid::Grid> G;
    grid::VtParser P;
    PredictionEngine E;
    Timestamp Now = Timestamp::FromNanos(1'000'000'000);
    std::uint64_t In = 0;
    std::uint64_t Out = 0;
    std::uint32_t Gen = 0;

    Rig(int cols, int rows, EDisplayMode mode)
        : G(MakeGrid(cols, rows)), E(MirrorView{*G, P}, mode) {}

    MirrorView View() { return MirrorView{*G, P}; }

    /// The agent's bytes reach the mirror.
    void Output(std::string_view s) {
        P.Feed(Bytes(s).data(), s.size(), *G);
        Out += s.size();
        E.OnOutput(Out, Now, View());
    }

    /// The user types; the bytes go to the agent.
    void Type(std::string_view s) {
        In += s.size();
        E.OnInput(Bytes(s), In, Now, View());
    }

    /// An EchoAck for everything fed and sent so far.
    void Ack() { E.OnEchoAck(EchoAck{Gen, Out, In}, Now, View()); }

    /// The first EchoAck of a connection: a baseline for everything so far.
    void Baseline() { E.OnBaseline(EchoAck{Gen, Out, In}, Now, View()); }

    void Advance(std::chrono::milliseconds d) { Now += d; }

    /// Type, let the agent echo, ack: the prompt-and-one-key start that
    /// confirms the first epoch. Leaves the cursor after the echoed text.
    void Confirm(std::string_view key = "a") {
        Type(key);
        Output(key);
        Ack();
    }

    /// Two echoed keys of different glyphs: the least that confirms an epoch
    /// (design.md M7). Leaves the cursor two columns on.
    void ConfirmEpoch() {
        Confirm("a");
        Confirm("A");
    }

    /// A prompt: CR makes the column known.
    void Prompt(std::string_view s = "\r$ ") { Output(s); }

    [[nodiscard]] std::size_t Count() const { return E.Predictions().size(); }
    [[nodiscard]] bool AnyVisible() const {
        for (const Prediction& p : E.Predictions()) {
            if (E.IsVisible(p)) return true;
        }
        return false;
    }
};

}  // namespace zet::predict::test

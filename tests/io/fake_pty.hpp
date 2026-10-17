#pragma once

#include <cstddef>
#include <deque>
#include <vector>

#include "zet/core/bytes.hpp"
#include "zet/io/errno.hpp"
#include "zet/io/pty.hpp"

/// A pty and the child on it, in memory: what the agent's tests drive when
/// the point is the agent and not the kernel.
///
/// Allowed by the mock rule of docs/design.md §16 only because the contract
/// in pty_contract_test.cpp runs on it and on a real pty with zet_pty_echo
/// alike. It models that pair, not a terminal in general: the child either
/// echoes or emits, exactly as zet_pty_echo does.
///
/// Three bounded queues stand for the kernel's buffer towards the child, what
/// the child has read but not yet written back, and the kernel's buffer
/// towards the master. A byte moves on only while there is room ahead of it,
/// which is the whole point: a writer that never reads the output fills all
/// three and then gets EAGAIN, the way the ET deadlock of 243078a needed a
/// real pty to show.
namespace zet::io::test {

class FakePty {
public:
    struct Capacity {
        std::size_t Input{4096};
        std::size_t Child{4096};
        std::size_t Output{4096};
    };

    /// A child that writes back everything it reads, and answers a resize
    /// with the same report zet_pty_echo prints.
    [[nodiscard]] static FakePty Echo(Capacity capacity);

    /// A child that writes `output` and exits.
    [[nodiscard]] static FakePty Emitter(Capacity capacity, ByteSpan output);

    /// As write(2) on a non-blocking master.
    [[nodiscard]] IoResult<std::size_t> Write(ByteSpan bytes);

    /// As ReadPty: 0 once the child has exited and its output is read.
    [[nodiscard]] IoResult<std::size_t> Read(MutableByteSpan into);

    [[nodiscard]] IoResult<void> Resize(WindowSize size);

    [[nodiscard]] bool Readable() const noexcept;
    [[nodiscard]] bool Writable() const noexcept;
    /// The child has exited. Output may still be waiting to be read.
    [[nodiscard]] bool HungUp() const noexcept { return Exited_; }

private:
    explicit FakePty(Capacity capacity) : Capacity_(capacity) {}

    /// Lets the child run until it cannot move another byte.
    void Settle();

    Capacity Capacity_;
    bool Echoes_{false};
    bool Exited_{false};
    std::deque<std::byte> Input_;
    std::deque<std::byte> Child_;
    std::deque<std::byte> Output_;
    /// What an emitter has yet to write; it exits when this is empty.
    std::deque<std::byte> Script_;
};

}  // namespace zet::io::test

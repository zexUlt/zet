#pragma once

#include <cstdint>

#include "zet/core/bytes.hpp"
#include "zet/io/errno.hpp"
#include "zet/io/fd.hpp"
#include "zet/io/terminal.hpp"

/// Pseudo-terminals, per docs/design.md §10 and §16: the agent's end of the
/// shell it runs.
namespace zet::io {

/// Both ends of one pseudo-terminal. The master is non-blocking and both are
/// close-on-exec; the slave is opened without becoming anyone's controlling
/// terminal, which is Spawn's to arrange in the child.
struct Pty {
    UniqueFd Master;
    UniqueFd Slave;
};

[[nodiscard]] IoResult<Pty> OpenPty() noexcept;

/// One read from the master, retried on EINTR.
///
/// Returns 0 at the end of the stream. Linux does not say so with a 0 the way
/// a pipe does: once every descriptor of the slave is closed, a read on the
/// master fails with EIO, and that EIO comes only after whatever output was
/// still buffered has been read. It is reported here as 0, so the caller sees
/// one end of stream for a pty, a pipe and a socket alike.
[[nodiscard]] IoResult<std::size_t> ReadPty(int master,
                                            MutableByteSpan into) noexcept;

}  // namespace zet::io

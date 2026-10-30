#pragma once

#include "zet/io/errno.hpp"

namespace zet::io {

/// The absolute, canonical path of the running executable (design.md §10).
///
/// On Linux the path is read from /proc/self/exe via readlink.
/// On macOS _NSGetExecutablePath returns the raw path; realpath then
/// resolves any symlinks or relative components, matching the Linux result.
///
/// Returns the path as a std::string, or an IoResult error if the syscall
/// fails (e.g. the kernel is very old and lacks /proc on Linux, or
/// _NSGetExecutablePath returns -1 on macOS).
[[nodiscard]] IoResult<std::string> SelfPath() noexcept;

}  // namespace zet::io

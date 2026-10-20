#include "zet/io/seccomp.hpp"

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include <array>
#include <cerrno>
#include <cstddef>

namespace zet::io {

namespace {

#if defined(__x86_64__)
// Not AUDIT_ARCH: linux/audit.h already uses the name, for an audit field.
constexpr std::uint32_t NATIVE_AUDIT_ARCH = AUDIT_ARCH_X86_64;
// x32 shares the x86_64 audit arch and tells its calls apart by this bit.
constexpr std::uint32_t FOREIGN_ABI_BIT = 0x40000000U;
#elif defined(__aarch64__)
constexpr std::uint32_t NATIVE_AUDIT_ARCH = AUDIT_ARCH_AARCH64;
constexpr std::uint32_t FOREIGN_ABI_BIT = 0;
#else
#error "no seccomp filter for this architecture"
#endif

/// Each listed only where this architecture has it: aarch64 never had open,
/// creat or the other pre-*at calls, and naming them there would not compile.
constexpr std::array FILE_OPERATIONS{
#ifdef __NR_open
    __NR_open,
#endif
#ifdef __NR_creat
    __NR_creat,
#endif
    __NR_openat,
#ifdef __NR_openat2
    __NR_openat2,
#endif
#ifdef __NR_unlink
    __NR_unlink,
#endif
    __NR_unlinkat,
#ifdef __NR_rmdir
    __NR_rmdir,
#endif
#ifdef __NR_rename
    __NR_rename,
#endif
#ifdef __NR_renameat
    __NR_renameat,
#endif
#ifdef __NR_renameat2
    __NR_renameat2,
#endif
#ifdef __NR_mkdir
    __NR_mkdir,
#endif
    __NR_mkdirat,
#ifdef __NR_mknod
    __NR_mknod,
#endif
    __NR_mknodat,
#ifdef __NR_link
    __NR_link,
#endif
    __NR_linkat,
#ifdef __NR_symlink
    __NR_symlink,
#endif
    __NR_symlinkat,
#ifdef __NR_chmod
    __NR_chmod,
#endif
    __NR_fchmod,         __NR_fchmodat,
#ifdef __NR_fchmodat2
    __NR_fchmodat2,
#endif
#ifdef __NR_chown
    __NR_chown,
#endif
#ifdef __NR_lchown
    __NR_lchown,
#endif
    __NR_fchown,         __NR_fchownat,       __NR_truncate,
// io_uring submits openat, unlinkat, renameat and the rest as ring entries,
// never through the numbers above: allowed, it is a way around all of them.
// Nothing in the mux needs it.
#ifdef __NR_io_uring_setup
    __NR_io_uring_setup, __NR_io_uring_enter, __NR_io_uring_register,
#endif
};

constexpr std::array EXECUTION{__NR_execve, __NR_execveat};

constexpr std::uint32_t RETURN_EPERM =
    SECCOMP_RET_ERRNO | (static_cast<std::uint32_t>(EPERM) & SECCOMP_RET_DATA);

/// Two loads and two checks for the arch and the ABI, one check per call,
/// the allow, and the two returns the jumps land on.
constexpr std::size_t PROGRAM_SIZE =
    4 + EXECUTION.size() + FILE_OPERATIONS.size() + 3;

constexpr sock_filter Statement(std::uint16_t code, std::uint32_t k) noexcept {
    return sock_filter{.code = code, .jt = 0, .jf = 0, .k = k};
}

constexpr sock_filter Jump(std::uint16_t code, std::uint32_t k,
                           std::uint8_t ifTrue, std::uint8_t ifFalse) noexcept {
    return sock_filter{.code = code, .jt = ifTrue, .jf = ifFalse, .k = k};
}

/// The filter, built at compile time. Layout:
///   load arch; arch != ours → kill
///   load nr;   nr has the foreign-ABI bit → kill
///   nr == each exec call → kill
///   nr == each file operation → EPERM
///   allow
///   kill:  RET KILL_PROCESS      (the jumps above land here)
///   eperm: RET ERRNO(EPERM)
constexpr std::array<sock_filter, PROGRAM_SIZE> BuildProgram() noexcept {
    std::array<sock_filter, PROGRAM_SIZE> program{};
    std::size_t at = 0;
    const std::size_t kill = PROGRAM_SIZE - 2;
    const std::size_t eperm = PROGRAM_SIZE - 1;
    // A jump's offset counts from the instruction after it.
    const auto to = [&](std::size_t target) {
        return static_cast<std::uint8_t>(target - at - 1);
    };

    program.at(at) =
        Statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch));
    ++at;
    program.at(at) =
        Jump(BPF_JMP | BPF_JEQ | BPF_K, NATIVE_AUDIT_ARCH, 0, to(kill));
    ++at;
    program.at(at) =
        Statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr));
    ++at;
    // With no foreign ABI the mask is zero and the test never jumps.
    program.at(at) =
        Jump(BPF_JMP | BPF_JSET | BPF_K, FOREIGN_ABI_BIT, to(kill), 0);
    ++at;
    for (const long number : EXECUTION) {
        program.at(at) = Jump(BPF_JMP | BPF_JEQ | BPF_K,
                              static_cast<std::uint32_t>(number), to(kill), 0);
        ++at;
    }
    for (const long number : FILE_OPERATIONS) {
        program.at(at) = Jump(BPF_JMP | BPF_JEQ | BPF_K,
                              static_cast<std::uint32_t>(number), to(eperm), 0);
        ++at;
    }
    program.at(at) = Statement(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    program.at(kill) = Statement(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    program.at(eperm) = Statement(BPF_RET | BPF_K, RETURN_EPERM);
    return program;
}

// A jump is eight bits wide: every target must be within 255 of its jump.
static_assert(PROGRAM_SIZE < 256);

constexpr auto NO_FILE_OPERATIONS = BuildProgram();

}  // namespace

IoResult<void> InstallSeccomp(ESeccompProfile profile) noexcept {
    switch (profile) {
        case ESeccompProfile::NoFileOperations:
            break;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return std::unexpected(LastErrno("prctl"));
    }
    // The kernel takes a pointer to non-const and does not write through it.
    sock_fprog program{
        .len = static_cast<unsigned short>(NO_FILE_OPERATIONS.size()),
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        .filter = const_cast<sock_filter*>(NO_FILE_OPERATIONS.data())};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
        return std::unexpected(LastErrno("prctl"));
    }
    return {};
}

}  // namespace zet::io

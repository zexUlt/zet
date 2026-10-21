#include "deliver.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "zet/crypto/crypto.hpp"

namespace zet::client {

namespace {

constexpr char HEX_CHARS[] = "0123456789abcdef";

}  // namespace

std::vector<std::byte> LoadAgentBinary(std::string_view agentDir,
                                       std::string_view arch,
                                       std::string* reason) noexcept {
    std::string path;
    path.reserve(agentDir.size() + 1 + 10 + arch.size());
    path.append(agentDir);
    path += '/';
    path.append("zet-agent-");
    path.append(arch);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return {};
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 0) {
        static_cast<void>(::close(fd));
        return {};
    }

    constexpr std::size_t maxAgentSize = 16 * 1024 * 1024;  // 16 MiB
    const auto size = static_cast<std::size_t>(st.st_size);
    if (size > maxAgentSize) {
        if (reason != nullptr) {
            *reason = "agent binary too large (> 16 MiB): " + std::string{arch};
        }
        static_cast<void>(::close(fd));
        return {};
    }
    std::vector<std::byte> data(size);
    std::size_t read = 0;
    while (read < size) {
        const auto n = ::read(fd, data.data() + read, size - read);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            static_cast<void>(::close(fd));
            return {};
        }
        read += static_cast<std::size_t>(n);
    }

    static_cast<void>(::close(fd));
    return data;
}

std::string HashHex(std::span<const std::byte> data) noexcept {
    using namespace zet::crypto;
    std::array<std::byte, HASH256_SIZE> digest{};
    Hash(digest, ByteSpan{data.data(), data.size()});

    std::string hex(HASH256_SIZE * 2, '\0');
    for (std::size_t i = 0; i < HASH256_SIZE; ++i) {
        const auto byte = std::to_integer<std::uint8_t>(digest[i]);
        hex[i * 2] = HEX_CHARS[byte >> 4U];
        hex[i * 2 + 1] = HEX_CHARS[byte & 0x0FU];
    }
    return hex;
}

}  // namespace zet::client

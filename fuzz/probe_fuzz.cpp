// The probe scanner (ProbeScanner), with random input split into arbitrary
// chunks.  The split point is taken from the first byte: every chunk size
// from 1 (byte-by-byte) to 255 (chunks of up to 255 bytes) is exercised.
// The scanner must never abort, access out-of-bounds memory, or produce an
// inconsistent Consumed count.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <variant>

#include "zet/core/bytes.hpp"
#include "zet/wire/probe.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    if (size == 0) {
        return 0;
    }
    // First byte encodes the chunk size (1..255); 0 is treated as 1.
    const std::size_t chunkSize =
        data[0] == 0 ? 1 : static_cast<std::size_t>(data[0]);

    const zet::ByteSpan input{reinterpret_cast<const std::byte*>(data + 1),
                              size - 1};

    zet::wire::ProbeScanner scanner;
    std::size_t offset = 0;
    while (offset < input.size()) {
        const std::size_t n = std::min(chunkSize, input.size() - offset);
        const zet::ByteSpan chunk = input.subspan(offset, n);
        const auto scan = scanner.Feed(chunk);

        // Consumed must be within [0, chunk.size()].
        if (scan.Consumed > chunk.size()) {
            __builtin_trap();
        }

        // If an answer was found, Consumed must be ≤ n.
        if (scan.Answer && scan.Consumed > n) {
            __builtin_trap();
        }

        offset += scan.Consumed;

        // When no answer is found, the whole chunk must be consumed.
        if (!scan.Answer && scan.Consumed != n) {
            __builtin_trap();
        }

        // If we got an answer, consume any leftover bytes by continuing to
        // feed them until the input is exhausted.  This exercises the scanner
        // state after an answer (residual bytes belong to the bootstrap armor).
        if (scan.Answer) {
            // Touch Arch and Reason fields so the sanitizer can check them.
            if (const auto* info =
                    std::get_if<zet::wire::ProbeInfo>(&*scan.Answer)) {
                volatile char sink = info->Arch.empty() ? 0 : info->Arch[0];
                (void)sink;
            }
            if (const auto* f =
                    std::get_if<zet::wire::ProbeFailed>(&*scan.Answer)) {
                volatile char sink = f->Reason.empty() ? 0 : f->Reason[0];
                (void)sink;
            }
            // Feed leftover bytes until the input is exhausted.
            // (offset was already advanced past scan.Consumed by the outer
            // `offset += scan.Consumed` above; do not advance again here.)
            while (offset < input.size()) {
                const std::size_t n2 =
                    std::min(chunkSize, input.size() - offset);
                const auto scan2 = scanner.Feed(input.subspan(offset, n2));
                if (scan2.Consumed > n2) {
                    __builtin_trap();
                }
                offset += scan2.Consumed > 0 ? scan2.Consumed : n2;
            }
            break;
        }
    }
    return 0;
}

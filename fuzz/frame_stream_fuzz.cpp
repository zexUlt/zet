// The reassembler, fed the same bytes twice in different pieces.
//
// The M1 criterion is that any chunking of the stream yields the same sequence
// of messages (design.md §17). The property test picks its splits from a list a
// person wrote down; this one runs the split the input asks for against the
// coarsest split there is and compares what came out. A divergence is the
// finding — "did not crash" is only the floor here.
//
// Input layout: a control byte, a length-prefixed chunk program, and the stream
// itself. The control byte picks the stage limit and when it widens, so a run
// covers MAX_HANDSHAKE_FRAME, MAX_FRAME, and the RaiseLimit step between them —
// a declared length legal on one side of that step and not the other is exactly
// what a single limit would never reach.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "zet/core/byte_reader.hpp"
#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"
#include "zet/wire/frame.hpp"
#include "zet/wire/frame_stream.hpp"
#include "zet/wire/limits.hpp"

namespace {

using zet::ByteSpan;
using zet::EProtoError;
using zet::MutableByteSpan;
using zet::wire::FrameStream;
using zet::wire::FrameView;
using zet::wire::HEADER_SIZE;
using zet::wire::MAX_FRAME;
using zet::wire::MAX_HANDSHAKE_FRAME;

/// A chunk size standing for "as much as the stream will take". The reference
/// run asks for nothing else; a chunk program reaches it with a zero byte.
constexpr std::size_t WHOLE_REMAINDER = static_cast<std::size_t>(-1);

/// Chunk sizes cycle through at most this many bytes of the input. Longer
/// programs would spend input on a schedule nobody reads to the end, and the
/// stream is what the mutator should be working on.
constexpr std::size_t MAX_PROGRAM_LENGTH = 32;

/// Next() is called once per Append plus once per frame it hands back, and a
/// frame costs at least HEADER_SIZE + 1 bytes of the stream — so a run stays
/// well under two calls per input byte. Exceeding that means a call which
/// neither consumed input nor produced a frame is repeating: a spin, which no
/// amount of further input resolves.
constexpr std::size_t MAX_NEXT_CALLS_PER_BYTE = 2;

/// Which frame the limit widens on is drawn from three bits of the control
/// byte; zero leaves the stream on the limit it started with.
constexpr std::uint8_t RAISE_AFTER_MASK = 0x7;

/// A frame taken off the stream, kept by value: the buffer has long moved on by
/// the time the two runs are compared.
struct DecodedFrame {
    std::uint32_t Length{0};
    std::uint8_t Epoch{0};
    std::vector<std::byte> Body;

    bool operator==(const DecodedFrame&) const = default;
};

enum class EEnding : std::uint8_t {
    /// Every byte was fed and the stream is waiting for more.
    Drained,
    /// Next() reported an error, which is where the run stops.
    Failed,
    /// The stream would take no more bytes and had handed out no error — a
    /// state the accounting is not supposed to allow.
    Stalled,
};

struct RunResult {
    std::vector<DecodedFrame> Frames;
    EEnding Ending{EEnding::Drained};
    /// Meaningful when Ending is Failed.
    EProtoError Error{};
    /// Bytes still held back, meaningful when Ending is Drained.
    std::size_t Leftover{0};
};

[[nodiscard]] bool SameOutcome(const RunResult& left,
                               const RunResult& right) noexcept {
    if (left.Frames != right.Frames || left.Ending != right.Ending) {
        return false;
    }
    if (left.Ending == EEnding::Failed) {
        return left.Error == right.Error;
    }
    // Only a run that fed the whole stream can be held to what it kept back:
    // one that stopped at an error stopped after a different number of bytes in
    // each chunking.
    if (left.Ending == EEnding::Drained) {
        return left.Leftover == right.Leftover;
    }
    return true;
}

/// Chunk sizes cycled from the program bytes. A zero byte asks for everything
/// available, so one program can hold both extremes; an empty program means one
/// byte at a time, the split that tears every field apart.
class ChunkProgram {
public:
    explicit ChunkProgram(ByteSpan program) noexcept : Program_(program) {}

    [[nodiscard]] std::size_t Next() noexcept {
        if (Program_.empty()) {
            return 1;
        }
        const auto size = std::to_integer<std::uint8_t>(Program_[Pos_]);
        Pos_ = (Pos_ + 1) % Program_.size();
        return size == 0 ? WHOLE_REMAINDER : static_cast<std::size_t>(size);
    }

private:
    ByteSpan Program_;
    std::size_t Pos_{0};
};

void CheckFrame(const FrameView& frame, const FrameStream& stream,
                ByteSpan storage) noexcept {
    // A frame that was handed over describes bytes that are really there, and
    // they are the caller's own buffer: a body pointing outside it, or a
    // Consumed past the capacity, would hand the next layer a span over memory
    // the stream does not own.
    const bool consistent =
        frame.Body.size() == frame.Header.Length &&
        frame.Consumed == HEADER_SIZE + frame.Header.Length &&
        frame.Consumed <= stream.Capacity() && frame.Header.Length != 0 &&
        frame.Header.Length <= stream.MaxBodyLength() &&
        frame.Body.data() >= storage.data() &&
        frame.Body.data() + frame.Body.size() <=
            storage.data() + storage.size();
    if (!consistent) {
        __builtin_trap();
    }
}

/// Takes every frame the stream will hand over. Returns false once the run is
/// over, which is what an error means: the offending bytes stay at the front,
/// so there is nothing further to read.
[[nodiscard]] bool Collect(FrameStream& stream, ByteSpan storage,
                           std::size_t raiseAfterFrames, std::size_t& budget,
                           RunResult& result) {
    while (true) {
        if (budget == 0) {
            __builtin_trap();
        }
        --budget;

        const auto next = stream.Next();
        if (!next) {
            // The bytes that caused it are still at the front, so a second look
            // has to reach the same verdict. A stream that moved on would be
            // parsing whatever the sender put after a frame we never
            // understood — resynchronisation the peer gets to steer.
            const auto again = stream.Next();
            if (again.has_value() || again.error() != next.error()) {
                __builtin_trap();
            }
            result.Ending = EEnding::Failed;
            result.Error = next.error();
            return false;
        }
        if (!next->has_value()) {
            return true;
        }

        const FrameView& frame = **next;
        CheckFrame(frame, stream, storage);
        result.Frames.push_back(DecodedFrame{
            .Length = frame.Header.Length,
            .Epoch = frame.Header.Epoch,
            .Body =
                std::vector<std::byte>{frame.Body.begin(), frame.Body.end()},
        });

        // Counted in frames rather than in appends: the two runs disagree about
        // appends by construction, and a limit that widened at a different
        // point in the stream would make them differ for a reason that is not a
        // bug.
        if (result.Frames.size() == raiseAfterFrames) {
            stream.RaiseLimit(MAX_FRAME);
        }
    }
}

/// Feeds `bytes` in the pieces `nextChunk` asks for, clipped to what the stream
/// still has room for — which is what a caller reading off a socket does, and
/// makes the largest single piece anyone can hand over one capacity.
template <typename TChunker>
[[nodiscard]] RunResult Drain(ByteSpan bytes, std::size_t bodyCapacity,
                              std::uint32_t initialLimit,
                              std::size_t raiseAfterFrames,
                              TChunker nextChunk) {
    std::vector<std::byte> storage(HEADER_SIZE + bodyCapacity);
    FrameStream stream{MutableByteSpan{storage}, initialLimit};

    RunResult result;
    std::size_t budget = MAX_NEXT_CALLS_PER_BYTE * (bytes.size() + 1);
    std::size_t offset = 0;

    while (offset < bytes.size()) {
        const std::size_t take =
            std::min({std::max<std::size_t>(nextChunk(), 1),
                      bytes.size() - offset, stream.Room()});
        if (take == 0) {
            result.Ending = EEnding::Stalled;
            return result;
        }

        // Room() said this much fits and there is no other ground on which an
        // append is refused, so a failure here means the two disagree.
        if (!stream.Append(bytes.subspan(offset, take))) {
            __builtin_trap();
        }
        offset += take;

        if (!Collect(stream, ByteSpan{storage}, raiseAfterFrames, budget,
                     result)) {
            return result;
        }
    }

    result.Leftover = stream.Buffered();
    return result;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    zet::ByteReader reader{
        ByteSpan{reinterpret_cast<const std::byte*>(data), size}};

    const auto control = reader.ReadU8();
    if (!control) {
        return 0;
    }
    const auto programLength = reader.ReadU8();
    if (!programLength) {
        return 0;
    }
    const auto program =
        reader.ReadBytes(std::min({static_cast<std::size_t>(*programLength),
                                   MAX_PROGRAM_LENGTH, reader.Remaining()}));
    if (!program) {
        return 0;
    }
    const ByteSpan bytes = reader.Rest();

    const std::uint32_t initialLimit =
        (*control & 1U) != 0 ? MAX_FRAME : MAX_HANDSHAKE_FRAME;
    const auto raiseAfterFrames =
        static_cast<std::size_t>((*control >> 1U) & RAISE_AFTER_MASK);

    // The storage caps the effective limit, so it is kept wide enough that
    // MAX_HANDSHAKE_FRAME is the binding one — sized to the input instead, the
    // two stages would agree on every length and RaiseLimit would do nothing
    // observable. Past that, one input is all a run can ever have to hold.
    const std::size_t bodyCapacity =
        std::min(std::max<std::size_t>(bytes.size(), MAX_HANDSHAKE_FRAME + 1),
                 static_cast<std::size_t>(MAX_FRAME));

    ChunkProgram chunks{*program};
    const RunResult split =
        Drain(bytes, bodyCapacity, initialLimit, raiseAfterFrames,
              [&chunks] { return chunks.Next(); });
    const RunResult whole =
        Drain(bytes, bodyCapacity, initialLimit, raiseAfterFrames,
              [] { return WHOLE_REMAINDER; });

    if (!SameOutcome(split, whole)) {
        __builtin_trap();
    }
    return 0;
}

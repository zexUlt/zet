#include "zet/wire/armor.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace zet;
using namespace zet::wire;

ByteSpan AsBytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::vector<std::byte> Bytes(std::string_view text) {
    const auto span = AsBytes(text);
    return {span.begin(), span.end()};
}

std::string Encode(ByteSpan bytes) {
    std::string out((bytes.size() + 2) / 3 * 4, '\0');
    const auto n = Base64Encode(bytes, std::span{out});
    REQUIRE(n.has_value());
    out.resize(*n);
    return out;
}

std::optional<std::vector<std::byte>> Decode(std::string_view text) {
    std::vector<std::byte> out(text.size());
    const auto n = Base64Decode(text, MutableByteSpan{out});
    if (!n) {
        return std::nullopt;
    }
    out.resize(*n);
    return out;
}

std::string Armor(ByteSpan frame) {
    std::string out(ArmoredSize(frame.size()), '\0');
    const auto n = WriteArmor(frame, std::span{out});
    REQUIRE(n.has_value());
    CHECK(*n == out.size());
    return out;
}

std::vector<std::byte> Frame(std::size_t size, std::uint8_t seed) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>(seed + i * 13);
    }
    return bytes;
}

/// Every block the stream holds, fed in chunks of `chunk` bytes.
std::vector<std::vector<std::byte>> Scan(ArmorScanner& scanner,
                                         std::string_view stream,
                                         std::size_t chunk) {
    std::vector<std::vector<std::byte>> blocks;
    for (std::size_t from = 0; from < stream.size(); from += chunk) {
        ByteSpan piece = AsBytes(stream.substr(from, chunk));
        while (!piece.empty()) {
            const auto scan = scanner.Feed(piece);
            REQUIRE(scan.Consumed <= piece.size());
            if (scan.Block) {
                blocks.emplace_back(scan.Block->begin(), scan.Block->end());
            } else {
                REQUIRE(scan.Consumed == piece.size());
            }
            piece = piece.subspan(scan.Consumed);
        }
    }
    return blocks;
}

std::vector<std::vector<std::byte>> Scan(std::string_view stream,
                                         std::size_t chunk = 4096) {
    ArmorScanner scanner;
    return Scan(scanner, stream, chunk);
}

}  // namespace

TEST_CASE("base64 matches RFC 4648 §10") {
    const std::array<std::pair<std::string_view, std::string_view>, 7> vectors{
        {{"", ""},
         {"f", "Zg=="},
         {"fo", "Zm8="},
         {"foo", "Zm9v"},
         {"foob", "Zm9vYg=="},
         {"fooba", "Zm9vYmE="},
         {"foobar", "Zm9vYmFy"}}};
    for (const auto& [plain, encoded] : vectors) {
        CAPTURE(plain);
        CHECK(Encode(AsBytes(plain)) == encoded);
        CHECK(Decode(encoded) == Bytes(plain));
    }
}

TEST_CASE("base64 round-trips every byte value") {
    std::vector<std::byte> all(256);
    for (std::size_t i = 0; i < all.size(); ++i) {
        all[i] = static_cast<std::byte>(i);
    }
    for (std::size_t length = 0; length <= all.size(); ++length) {
        CAPTURE(length);
        const auto prefix = ByteSpan{all}.first(length);
        const auto back = Decode(Encode(prefix));
        REQUIRE(back.has_value());
        CHECK(std::equal(back->begin(), back->end(), prefix.begin(),
                         prefix.end()));
    }
}

TEST_CASE("base64 refuses every text that is not the one spelling") {
    for (const std::string_view text :
         {"Zg=", "Zg", "Z===", "====", "Zg==Zg==", "Z=g=", "Zh==", "Zm9=",
          "Zm 9", "Zm9v\n", "Zm-v", "Zm_v", "Zm9\x80"}) {
        CAPTURE(text);
        CHECK_FALSE(Decode(text).has_value());
    }
}

TEST_CASE("base64 refuses an output buffer that is too small") {
    std::array<char, 3> small{};
    CHECK(Base64Encode(AsBytes("foo"), std::span{small}).error() ==
          EProtoError::BufferTooSmall);
    std::array<std::byte, 2> tiny{};
    CHECK(Base64Decode("Zm9v", MutableByteSpan{tiny}).error() ==
          EProtoError::BufferTooSmall);
}

TEST_CASE("an armored frame is found again, whatever the chunking") {
    const auto frame = Frame(106, 0x21);
    const std::string stream = Armor(ByteSpan{frame});
    CHECK(stream.front() == '\n');
    for (const std::size_t chunk : {1UL, 2UL, 7UL, 64UL, 4096UL}) {
        CAPTURE(chunk);
        const auto blocks = Scan(stream, chunk);
        REQUIRE(blocks.size() == 1);
        CHECK(blocks.front() == frame);
    }
}

TEST_CASE("noise around a block is ignored") {
    const auto frame = Frame(40, 0x05);
    std::string stream = "Last login: yesterday\r\nwelcome to the box\n";
    stream += std::string(10'000, 'x');  // a line far past MAX_ARMOR_LINE
    stream += "\n\x01\x02\xff binary\n";
    stream += Armor(ByteSpan{frame});
    stream += "bye\n";
    const auto blocks = Scan(stream, 333);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks.front() == frame);
}

TEST_CASE("a marker glued to a prompt still starts a block") {
    // The armor opens with its own newline for this case.
    const auto frame = Frame(20, 0x11);
    const auto blocks = Scan("user@host:~$ " + Armor(ByteSpan{frame}));
    REQUIRE(blocks.size() == 1);
    CHECK(blocks.front() == frame);
}

TEST_CASE("carriage returns from a pty do not break a block") {
    const auto frame = Frame(70, 0x31);
    std::string crlf;
    for (const char c : Armor(ByteSpan{frame})) {
        if (c == '\n') {
            crlf += '\r';
        }
        crlf += c;
    }
    const auto blocks = Scan(crlf);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks.front() == frame);
}

TEST_CASE("broken blocks are skipped and the search goes on") {
    const auto frame = Frame(30, 0x44);
    const std::string good = Armor(ByteSpan{frame});
    const std::string begin = std::string{ARMOR_BEGIN} + "\n";
    const std::string end = std::string{ARMOR_END} + "\n";

    const std::vector<std::string> broken{
        begin + "Zm9v!\n" + end,                     // not the alphabet
        begin + "Zm9\n" + end,                       // not a whole quad
        begin + "\n" + end,                          // an empty line
        begin + "Zm9v\nnoise in between\n" + end,    // interrupted
        begin + std::string(500, 'A') + "\n" + end,  // line too long
        begin + "Zm9v\n",                            // a BEGIN again
    };
    std::size_t lines = 0;
    std::string tooMuch = begin;
    while (tooMuch.size() < MAX_ARMOR_TEXT + 2 * ARMOR_LINE_WIDTH) {
        tooMuch += std::string(ARMOR_LINE_WIDTH, 'A') + "\n";
        ++lines;
    }
    tooMuch += end;

    for (const auto& prefix : broken) {
        CAPTURE(prefix);
        ArmorScanner scanner;
        const auto blocks = Scan(scanner, prefix + good, 5);
        REQUIRE(blocks.size() == 1);
        CHECK(blocks.front() == frame);
        CHECK(scanner.SkippedBlocks() == 1);
    }
    ArmorScanner scanner;
    const auto blocks = Scan(scanner, tooMuch + good, 64);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks.front() == frame);
    CHECK(scanner.SkippedBlocks() == 1);
    CHECK(lines > 0);
}

TEST_CASE("an END without a BEGIN is just a line") {
    const auto frame = Frame(12, 0x09);
    ArmorScanner scanner;
    const auto blocks = Scan(
        scanner, std::string{ARMOR_END} + "\n" + Armor(ByteSpan{frame}), 4096);
    REQUIRE(blocks.size() == 1);
    CHECK(scanner.SkippedBlocks() == 0);
}

TEST_CASE("two blocks come out one per call, in order") {
    const auto first = Frame(10, 0x01);
    const auto second = Frame(11, 0x02);
    const auto blocks =
        Scan(Armor(ByteSpan{first}) + Armor(ByteSpan{second}), 4096);
    REQUIRE(blocks.size() == 2);
    CHECK(blocks[0] == first);
    CHECK(blocks[1] == second);
}

TEST_CASE("armor refuses a frame too wide and a buffer too small") {
    const auto wide = Frame(MAX_ARMORED_FRAME + 1, 0);
    std::string out(ArmoredSize(wide.size()), '\0');
    CHECK(WriteArmor(ByteSpan{wide}, std::span{out}).error() ==
          EProtoError::LengthLimitExceeded);
    const auto frame = Frame(10, 0);
    std::string small(ArmoredSize(frame.size()) - 1, '\0');
    CHECK(WriteArmor(ByteSpan{frame}, std::span{small}).error() ==
          EProtoError::BufferTooSmall);
}

TEST_CASE("the widest frame fits through the armor") {
    const auto frame = Frame(MAX_ARMORED_FRAME, 0x77);
    const auto blocks = Scan(Armor(ByteSpan{frame}));
    REQUIRE(blocks.size() == 1);
    CHECK(blocks.front() == frame);
}

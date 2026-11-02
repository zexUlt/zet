#include "zet/app/byte_queue.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <vector>

#include "zet/core/bytes.hpp"

namespace {

using zet::ByteSpan;
using zet::app::ByteQueue;

}  // namespace

TEST_CASE("ByteQueue takes nothing past its limit") {
    ByteQueue queue{8};
    const std::vector<std::byte> five(5, std::byte{1});
    CHECK(queue.Append(ByteSpan{five}));
    CHECK_FALSE(queue.Append(ByteSpan{five}));
    CHECK(queue.Size() == 5);
    CHECK(queue.Room() == 3);
}

TEST_CASE("ByteQueue takes up to a ceiling above its limit") {
    ByteQueue queue{8};
    const std::vector<std::byte> six(6, std::byte{2});
    CHECK(queue.Append(ByteSpan{six}));
    CHECK(queue.Append(ByteSpan{six}, 12));
    CHECK(queue.Size() == 12);
    // Past its own limit the queue has no room, rather than wrapping.
    CHECK(queue.Room() == 0);
    CHECK_FALSE(queue.Append(ByteSpan{six}, 12));
    const std::vector<std::byte> one(1, std::byte{3});
    CHECK_FALSE(queue.Append(ByteSpan{one}));
    queue.Consume(12);
    CHECK(queue.Room() == 8);
}

#include "zet/core/secret.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using Key = std::array<std::uint8_t, 4>;
using zet::Secret;

bool AllZero(const Key& k) {
    for (auto b : k) {
        if (b != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("a secret hands out its value only through Expose") {
    const Secret<Key> key{Key{1, 2, 3, 4}};

    CHECK(key.Expose()[0] == 1);
    CHECK(key.Expose()[3] == 4);

    // The point of the type: there is no other way out. If any of these ever
    // start compiling, key material can reach a log line by accident.
    static_assert(!std::is_convertible_v<Secret<Key>, Key>);
    static_assert(!std::is_constructible_v<Key, Secret<Key>>);
}

TEST_CASE("secrets cannot be copied, only moved") {
    static_assert(!std::is_copy_constructible_v<Secret<Key>>);
    static_assert(!std::is_copy_assignable_v<Secret<Key>>);
    static_assert(std::is_nothrow_move_constructible_v<Secret<Key>>);
    static_assert(std::is_nothrow_move_assignable_v<Secret<Key>>);
}

// Reading a moved-from object is normally a bug, and the analyser is right to
// say so. Here it is the assertion: Secret guarantees the source is left wiped
// rather than merely unspecified, and that guarantee is only worth anything if
// something checks it.

TEST_CASE("moving wipes the source") {
    Secret<Key> from{Key{9, 9, 9, 9}};
    const Secret<Key> to{std::move(from)};

    CHECK(to.Expose()[0] == 9);
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
    CHECK(AllZero(from.Expose()));
}

TEST_CASE("move assignment wipes both the old value and the source") {
    Secret<Key> from{Key{7, 7, 7, 7}};
    Secret<Key> to{Key{1, 2, 3, 4}};

    to = std::move(from);

    CHECK(to.Expose()[0] == 7);
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
    CHECK(AllZero(from.Expose()));
}

TEST_CASE("an explicit wipe clears the value") {
    Secret<Key> key{Key{5, 6, 7, 8}};
    REQUIRE(key.Expose()[0] == 5);

    key.Wipe();

    CHECK(AllZero(key.Expose()));
}

TEST_CASE("a default-constructed secret starts zeroed") {
    const Secret<Key> key;
    CHECK(AllZero(key.Expose()));
}

TEST_CASE("only trivially copyable payloads are accepted") {
    // Secret<std::string> would wipe the string object rather than the buffer
    // it points at, leaving the key in freed memory while looking safe.
    static_assert(zet::CSecretPayload<Key>);
    static_assert(!zet::CSecretPayload<std::string>);
}

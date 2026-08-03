#pragma once

#include <cstddef>

namespace zet {

/// Overwrites `size` bytes at `data` with zeroes, and stays overwritten.
///
/// A plain memset over memory that is about to die is a dead store, and the
/// compiler is entitled to delete it — which is how key material survives in
/// freed pages. Defined out of line so there is exactly one implementation to
/// audit and no chance of it being inlined into something that optimises it
/// away differently.
///
/// From M1 this becomes sodium_memzero and the change is a single line here.
void SecureZero(void* data, std::size_t size) noexcept;

}  // namespace zet

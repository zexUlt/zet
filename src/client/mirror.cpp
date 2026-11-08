#include "mirror.hpp"

#include <new>
#include <utility>

namespace zet::client {

std::unique_ptr<Mirror> Mirror::Create(std::uint16_t cols,
                                       std::uint16_t rows) noexcept {
    auto grid = grid::Grid::Create(cols, rows, 0);
    if (!grid) {
        return nullptr;
    }
    auto owned = std::unique_ptr<grid::Grid>(*grid);
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    return std::unique_ptr<Mirror>(new (std::nothrow) Mirror(std::move(owned)));
}

void Mirror::Feed(ByteSpan bytes) noexcept {
    if (!bytes.empty()) {
        Parser_.Feed(bytes.data(), bytes.size(), *Grid_);
    }
}

bool Mirror::ResetFromSnapshot(ByteSpan snapshot) noexcept {
    auto fresh = grid::Grid::Create(Grid_->Cols(), Grid_->Rows(), 0);
    if (!fresh) {
        return false;
    }
    Grid_.reset(*fresh);
    Parser_.Reset();
    Feed(snapshot);
    return true;
}

bool Mirror::Resize(std::uint16_t cols, std::uint16_t rows) noexcept {
    return Grid_->Resize(cols, rows).has_value();
}

}  // namespace zet::client

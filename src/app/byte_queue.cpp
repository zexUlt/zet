#include "zet/app/byte_queue.hpp"

#include <algorithm>

namespace zet::app {

bool ByteQueue::Append(ByteSpan bytes) { return Append(bytes, Limit_); }

bool ByteQueue::Append(ByteSpan bytes, std::size_t ceiling) {
    if (Size() > ceiling || bytes.size() > ceiling - Size()) {
        return false;
    }
    Bytes_.insert(Bytes_.end(), bytes.begin(), bytes.end());
    return true;
}

void ByteQueue::Consume(std::size_t count) noexcept {
    Head_ += std::min(count, Size());
    if (Head_ == Bytes_.size()) {
        Bytes_.clear();
        Head_ = 0;
    } else if (Head_ > Bytes_.size() / 2) {
        Bytes_.erase(Bytes_.begin(),
                     Bytes_.begin() + static_cast<std::ptrdiff_t>(Head_));
        Head_ = 0;
    }
}

}  // namespace zet::app

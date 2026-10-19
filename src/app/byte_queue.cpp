#include "zet/app/byte_queue.hpp"

#include <algorithm>

namespace zet::app {

bool ByteQueue::Append(ByteSpan bytes) {
    if (bytes.size() > Room()) {
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

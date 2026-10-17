#include "zet/app/fd_sink.hpp"

#include <unistd.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <string_view>

namespace zet::app {

namespace {

/// Longer records are cut, not split: a log line is for a human.
constexpr std::size_t LOG_LINE_MAX = 512;

class Line {
public:
    void Add(std::string_view text) noexcept {
        const std::size_t n = std::min(text.size(), Room());
        for (std::size_t i = 0; i < n; ++i) {
            Bytes_[Size_ + i] = text[i];
        }
        Size_ += n;
    }

    template <typename TNumber>
    void AddNumber(TNumber value) noexcept {
        std::array<char, 24> digits{};
        const auto [end, error] =
            std::to_chars(digits.data(), digits.data() + digits.size(), value);
        if (error == std::errc{}) {
            Add(std::string_view{
                digits.data(), static_cast<std::size_t>(end - digits.data())});
        }
    }

    void WriteTo(int fd) noexcept {
        if (Size_ == Bytes_.size()) {
            --Size_;
        }
        Bytes_[Size_++] = '\n';
        static_cast<void>(::write(fd, Bytes_.data(), Size_));
    }

private:
    [[nodiscard]] std::size_t Room() const noexcept {
        // One byte stays free for the newline.
        return Bytes_.size() - 1 - Size_;
    }

    std::array<char, LOG_LINE_MAX> Bytes_{};
    std::size_t Size_{0};
};

void Write(const log::Record& record, void* context) noexcept {
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(context));
    Line line;
    line.AddNumber(record.At.SinceEpoch().count() / 1'000'000);
    line.Add(" ");
    line.Add(log::Describe(record.Level));
    line.Add(" ");
    line.Add(record.Message);
    for (const log::Field& field : record.Fields) {
        line.Add(" ");
        line.Add(field.Key());
        line.Add("=");
        switch (field.Kind()) {
            case log::Field::EKind::Text:
                line.Add(field.AsText());
                break;
            case log::Field::EKind::Unsigned:
                line.AddNumber(field.AsUnsigned());
                break;
            case log::Field::EKind::Signed:
                line.AddNumber(field.AsSigned());
                break;
        }
    }
    line.WriteTo(fd);
}

}  // namespace

void LogToFd(int fd) noexcept {
    log::SetSink(&Write,
                 reinterpret_cast<void*>(static_cast<std::intptr_t>(fd)));
}

}  // namespace zet::app

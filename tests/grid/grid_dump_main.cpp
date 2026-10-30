// zet_grid_dump: read raw bytes from stdin, render through VtParser+Grid,
// emit the visible screen as JSON {"rows": [...]} on stdout.
//
// Usage: zet_grid_dump --cols 80 --rows 24 < capture.bin
//
// This is a test helper only (not part of the protocol core); it may use
// stdio and std::string.

#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace zet::grid;

static void usage(const char* argv0) {
    std::fprintf(stderr, "Usage: %s --cols N --rows N\n", argv0);
}

int main(int argc, char** argv) {
    int cols = 80, rows = 24;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--cols") == 0 && i + 1 < argc)
            cols = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--rows") == 0 && i + 1 < argc)
            rows = std::atoi(argv[++i]);
        else {
            usage(argv[0]);
            return 1;
        }
    }
    if (cols <= 0 || rows <= 0) {
        usage(argv[0]);
        return 1;
    }

    // Create grid.
    auto res =
        Grid::Create(static_cast<std::uint16_t>(cols > 4096 ? 4096 : cols),
                     static_cast<std::uint16_t>(rows > 1024 ? 1024 : rows), 0);
    if (!res.has_value()) {
        std::fprintf(stderr, "Grid::Create failed\n");
        return 1;
    }
    Grid* g = res.value();
    VtParser p;

    // Read all of stdin.
    std::vector<std::uint8_t> buf;
    buf.reserve(65536);
    int c;
    while ((c = std::getchar()) != EOF)
        buf.push_back(static_cast<std::uint8_t>(c));

    p.Feed(reinterpret_cast<const std::byte*>(buf.data()), buf.size(), *g);

    // Emit JSON.
    std::printf("{\"rows\":[");
    for (int r = 0; r < rows; ++r) {
        if (r > 0) std::printf(",");
        std::printf("\"");
        for (int col = 0; col < cols; ++col) {
            const Cell& cell = g->CellAt(static_cast<std::uint16_t>(r),
                                         static_cast<std::uint16_t>(col));
            const char32_t cp = (cell.Codepoint != 0) ? cell.Codepoint : U' ';
            // Encode as UTF-8.
            if (cp < 0x80) {
                const char ch = static_cast<char>(cp);
                // JSON-escape special chars.
                if (ch == '"')
                    std::printf("\\\"");
                else if (ch == '\\')
                    std::printf("\\\\");
                else if (ch < 0x20)
                    std::printf("\\u%04x", static_cast<unsigned>(ch));
                else
                    std::putchar(ch);
            } else if (cp < 0x800) {
                std::putchar(static_cast<char>(0xC0 | (cp >> 6)));
                std::putchar(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                std::putchar(static_cast<char>(0xE0 | (cp >> 12)));
                std::putchar(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                std::putchar(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                std::putchar(static_cast<char>(0xF0 | (cp >> 18)));
                std::putchar(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                std::putchar(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                std::putchar(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        std::printf("\"");
    }
    std::printf("]}\n");
    delete g;
    return 0;
}

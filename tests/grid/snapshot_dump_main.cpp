// zet_snapshot_dump — test helper: render bytes from stdin through
// VtParser+Grid, serialise with Snapshot::Create, write snapshot bytes raw to
// stdout.
//
// Usage: zet_snapshot_dump [--cols N] [--rows N] [--scrollback N]
//
// Exit 0 on success, 1 on error (SnapshotTooLarge, allocation failure, etc.)
//
// This is a test helper only (not part of the protocol core); it may use
// stdio and std::string.

#include "zet/grid/grid.hpp"
#include "zet/grid/limits.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/vt_parser.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace zet::grid;

static void Usage(const char* argv0) {
    std::fprintf(stderr, "Usage: %s [--cols N] [--rows N] [--scrollback N]\n",
                 argv0);
}

int main(int argc, char** argv) {
    int cols = 80, rows = 24;
    int scrollback = static_cast<int>(DEFAULT_SCROLLBACK_LIMIT);
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--cols") == 0 && i + 1 < argc)
            cols = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--rows") == 0 && i + 1 < argc)
            rows = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--scrollback") == 0 && i + 1 < argc)
            scrollback = std::atoi(argv[++i]);
        else {
            Usage(argv[0]);
            return 1;
        }
    }
    if (cols <= 0 || rows <= 0 || scrollback < 0) {
        Usage(argv[0]);
        return 1;
    }

    auto res = Grid::Create(
        static_cast<std::uint16_t>(cols > static_cast<int>(MAX_COLS)
                                       ? static_cast<int>(MAX_COLS)
                                       : cols),
        static_cast<std::uint16_t>(rows > static_cast<int>(MAX_ROWS)
                                       ? static_cast<int>(MAX_ROWS)
                                       : rows),
        static_cast<std::uint32_t>(scrollback));
    if (!res.has_value()) {
        std::fprintf(stderr, "Grid::Create failed\n");
        return 1;
    }
    Grid* g = res.value();
    VtParser p;

    std::vector<std::uint8_t> buf;
    buf.reserve(65536);
    int c;
    while ((c = std::getchar()) != EOF)
        buf.push_back(static_cast<std::uint8_t>(c));

    p.Feed(reinterpret_cast<const std::byte*>(buf.data()), buf.size(), *g);

    // Use MaxBytes=0 so Snapshot::Create applies SNAPSHOT_MAX_BYTES.
    auto snap_res = Snapshot::Create(*g);
    if (!snap_res.has_value()) {
        const char* reason = "unknown";
        switch (snap_res.error()) {
            case EGridError::SnapshotTooLarge:
                reason = "SnapshotTooLarge";
                break;
            case EGridError::AllocationFailed:
                reason = "AllocationFailed";
                break;
            default:
                break;
        }
        std::fprintf(stderr, "Snapshot::Create failed: %s\n", reason);
        delete g;
        return 1;
    }
    Snapshot* snap = snap_res.value();

    const auto bytes = snap->Bytes();
    const auto* data = reinterpret_cast<const char*>(bytes.data());
    if (std::fwrite(data, 1, bytes.size(), stdout) != bytes.size()) {
        std::fprintf(stderr, "fwrite failed\n");
        delete snap;
        delete g;
        return 1;
    }

    delete snap;
    delete g;
    return 0;
}

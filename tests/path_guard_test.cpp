// The path sandbox is the only thing between a gRPC caller and an arbitrary
// file, and it had no test at all.
//
// ROI_ALLOWED_ROOTS is resolved once per process by design, so the roots are
// fixed here and the cases vary the path instead. Rejection of a missing or
// relative ROI_ALLOWED_ROOTS is a startup property and is covered by check.sh
// running the service binary itself.

#include "grpc/path_guard.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__,       \
                #cond);                                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

namespace {

bool rejects(const std::string& path)
{
    try {
        require_allowed_path(path, "test");
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

std::string accepts(const std::string& path)
{
    return require_allowed_path(path, "test");
}

} // namespace

int main()
{
    const fs::path base = fs::temp_directory_path() / "roi_path_guard_test";
    const fs::path root = base / "allowed";
    const fs::path outside = base / "outside";
    fs::remove_all(base);
    fs::create_directories(root / "nested");
    fs::create_directories(outside);
    { std::ofstream(outside / "secret.txt") << "x"; }

    setenv("ROI_ALLOWED_ROOTS", root.c_str(), 1);
    CHECK(init_allowed_roots().size() == 1);

    // Inside, existing and not yet existing alike -- an output path names a file
    // that is not there yet.
    CHECK(accepts((root / "clip.mp4").string()) == (root / "clip.mp4").string());
    CHECK(accepts((root / "nested" / "out.mp4").string())
        == (root / "nested" / "out.mp4").string());

    // Relative paths never resolve against a root.
    CHECK(rejects("clip.mp4"));
    CHECK(rejects("./clip.mp4"));
    CHECK(rejects("nested/out.mp4"));

    // Traversal, both plain and disguised by a real directory in the middle.
    CHECK(rejects((root / ".." / "outside" / "secret.txt").string()));
    CHECK(rejects((root / "nested" / ".." / ".." / "outside" / "secret.txt").string()));

    // Plainly outside.
    CHECK(rejects((outside / "secret.txt").string()));
    CHECK(rejects("/etc/passwd"));

    // A sibling whose name merely starts with the root's is not under it.
    const fs::path lookalike = base / "allowed_evil";
    fs::create_directories(lookalike);
    CHECK(rejects((lookalike / "clip.mp4").string()));

    // A symlink pointing out of the sandbox is followed, then rejected.
    std::error_code ec;
    fs::create_symlink(outside, root / "escape", ec);
    if (!ec) {
        CHECK(rejects((root / "escape" / "secret.txt").string()));
    }

    // Degenerate input.
    CHECK(rejects(""));
    CHECK(rejects(std::string("/tmp/a\0b", 8)));

    // The root itself is not a file to read or write.
    CHECK(rejects(root.string()));

    fs::remove_all(base);
    std::puts("path_guard_test ok");
    return 0;
}

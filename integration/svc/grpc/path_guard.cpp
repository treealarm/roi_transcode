#include "grpc/path_guard.h"

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
#ifdef _WIN32
constexpr char k_root_sep = ';';
#else
constexpr char k_root_sep = ':';
#endif

std::vector<fs::path> parse_allowed_roots()
{
    const char* raw = std::getenv("ROI_ALLOWED_ROOTS");
    if (!raw || !*raw) {
        throw std::runtime_error(
            "ROI_ALLOWED_ROOTS is required (separator-separated absolute directories)");
    }
    std::vector<fs::path> roots;
    std::string token;
    std::istringstream in(raw);
    while (std::getline(in, token, k_root_sep)) {
        if (token.empty()) {
            continue;
        }
        fs::path p(token);
        if (!p.is_absolute()) {
            throw std::runtime_error(
                "ROI_ALLOWED_ROOTS entries must be absolute: " + token);
        }
        std::error_code ec;
        auto canon = fs::weakly_canonical(p, ec);
        if (ec) {
            throw std::runtime_error(
                "ROI_ALLOWED_ROOTS cannot resolve " + token + ": " + ec.message());
        }
        roots.push_back(std::move(canon));
    }
    if (roots.empty()) {
        throw std::runtime_error("ROI_ALLOWED_ROOTS has no usable entries");
    }
    return roots;
}

// Resolved once. Re-reading the environment and canonicalizing every root on
// each request bought nothing: the variable cannot change under a running
// process.
const std::vector<fs::path>& allowed_roots()
{
    static const std::vector<fs::path> roots = parse_allowed_roots();
    return roots;
}

bool is_under(const fs::path& child, const fs::path& root)
{
    const auto rel = child.lexically_relative(root);
    // Empty means unrelated; "." means the root itself, which is a directory and
    // so never a file to read or write. Letting it through would trade a clear
    // rejection here for an obscure open failure later.
    if (rel.empty() || rel == ".") {
        return false;
    }
    for (const auto& part : rel) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}
} // namespace

std::string require_allowed_path(const std::string& raw, const char* field)
{
    if (raw.empty() || raw.find('\0') != std::string::npos) {
        throw std::runtime_error(std::string(field) + " is empty or contains NUL");
    }
    const fs::path p(raw);
    if (!p.is_absolute()) {
        throw std::runtime_error(std::string(field) + " must be an absolute path");
    }
    std::error_code ec;
    const auto canon = fs::weakly_canonical(p, ec);
    if (ec) {
        throw std::runtime_error(
            std::string(field) + " cannot resolve: " + ec.message());
    }
    for (const auto& root : allowed_roots()) {
        if (is_under(canon, root)) {
            return canon.string();
        }
    }
    throw std::runtime_error(
        std::string(field) + " is outside ROI_ALLOWED_ROOTS: " + canon.string());
}

const std::vector<std::string>& init_allowed_roots()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const auto& root : allowed_roots()) {
            out.push_back(root.string());
        }
        return out;
    }();
    return names;
}

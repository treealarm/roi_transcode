#pragma once

#include <string>
#include <vector>

// Parse and validate ROI_ALLOWED_ROOTS once, returning the canonical roots.
// Call this at startup: the roots are otherwise resolved on the first request,
// so a misconfigured deployment would look healthy until someone used it.
// Throws std::runtime_error if the variable is missing, relative or unusable.
const std::vector<std::string>& init_allowed_roots();

// Canonicalize `raw` and require it to sit under one of ROI_ALLOWED_ROOTS
// (':' on POSIX, ';' on Windows). Throws std::runtime_error on violation.
std::string require_allowed_path(const std::string& raw, const char* field);

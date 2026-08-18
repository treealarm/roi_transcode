#pragma once

#include <string>

// Canonicalize `raw` and require it to sit under one of ROI_ALLOWED_ROOTS
// (':' on POSIX, ';' on Windows). Throws std::runtime_error on violation.
std::string require_allowed_path(const std::string& raw, const char* field);

// git_path.hpp
#pragma once
#include "duckdb/common/string.hpp"

namespace duckdb {

// Normalize a repo-internal path used in git URIs (after the repo root).
// Rules:
// - Strip leading/trailing '/'.
// - Collapse consecutive '/' to a single '/'.
// - Drop a *leading* "./" (once or repeatedly).
// - **Do not** translate '\' to '/' (backslashes are literal).
// - **Do not** fold mid-path "./" segments (Git treats them literally).
// - **Forbid** any segment equal to ".." (security / escaping root).
// - Return the normalized path (may be empty).
// Throws BinderException on invalid input.
string NormalizeRepoPathSpec(const string &in);

} // namespace duckdb

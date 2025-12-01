#pragma once
#include <string>
#include <git2.h>

namespace duckdb {

// Returns true iff `path` changed in `commit` relative to its parent(s).
// Semantics (match `git log -- <path>`, not `--follow`):
// - Root commit: include if the path exists in the root tree
// - Single parent: include if any delta for the path exists
// - Merge commit: include only if the path changed vs *all* parents
bool FileChangedInCommit(git_repository *repo, git_commit *commit, const std::string &path);

} // namespace duckdb

#pragma once

#include "duckdb.hpp"
#include "git_filesystem.hpp"
#include <git2.h>
#include <unordered_map>

namespace duckdb {

//===--------------------------------------------------------------------===//
// GitContextManager - Unified Git URI Processing Architecture
//===--------------------------------------------------------------------===//

class GitContextManager {
public:
    // Unified result structure for all git URI processing
    struct GitContext {
        git_object *resolved_object;    // Validated reference object (caller must free)
        string repo_path;              // Absolute repository path (for opening in LocalTableFunctionState)
        string file_path;              // File path within repository
        string final_ref;              // Final resolved reference

        // Constructor
        GitContext(git_object *obj, const string &rp, const string &fp, const string &ref);

        // Move constructor
        GitContext(GitContext &&other) noexcept;

        // Destructor - only free the resolved_object
        ~GitContext();

        // Delete copy constructor and assignment to prevent accidental copies
        GitContext(const GitContext &) = delete;
        GitContext &operator=(const GitContext &) = delete;
        GitContext &operator=(GitContext &&) = delete;
    };

    // Singleton access
    static GitContextManager &Instance();
    
    // THE UNIVERSAL GIT PROCESSING FUNCTION
    // Handles URI parsing, repository discovery, and ref validation
    // Returns validated paths and resolved reference object
    // Each table function opens repository in its own LocalTableFunctionState (thread-safe)
    GitContext ProcessGitUri(const string &uri_or_path, const string &fallback_ref = "HEAD");

private:
    // Private constructor for singleton
    GitContextManager() = default;

    // Validate and resolve reference (UNIFIED APPROACH)
    // Opens repo temporarily, validates, then closes it
    git_object *ValidateAndResolveReference(const string &repo_path, const string &ref);
};

} // namespace duckdb
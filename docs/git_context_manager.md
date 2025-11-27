# GitContextManager

## Overview

GitContextManager is the core component that provides unified Git URI processing and validation across all `git_*` table functions. It centralizes URI parsing, repository path resolution, and reference validation to ensure consistent behavior across all git table functions.

## Purpose

Before GitContextManager, each git function (`git_log`, `git_tree`, `git_read`, etc.) implemented its own:
- URI parsing logic
- Repository path resolution
- Reference validation
- Error handling

This led to:
- Inconsistent error messages
- Duplicate code (~200 lines across 6 functions)
- Different behavior for the same invalid inputs

GitContextManager solves this by providing a single, shared validation layer.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    GitContextManager                             │
│                      (Singleton)                                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  1. Parse URI       2. Open Repo (temp)     3. Validate Ref     │
│     (GitPath)   →    (for validation)   →    (git_revparse)     │
│                           ↓                                      │
│                    Close repo immediately                        │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
                        Returns: GitContext
                        - resolved_object (RAII managed)
                        - repo_path (absolute, for opening)
                        - file_path (relative)
                        - final_ref (validated)
                                    │
                                    ▼
           ┌────────────────────────────────────────────┐
           │      LocalTableFunctionState               │
           │         (Per-thread)                       │
           ├────────────────────────────────────────────┤
           │  Opens repository using ctx.repo_path      │
           │  Owns repository for duration of query     │
           │  LATERAL optimization: cached_repo         │
           └────────────────────────────────────────────┘
```

## Usage

### Basic Pattern

All git table functions follow this pattern:

```cpp
// BIND PHASE: Validate and get paths
auto ctx = GitContextManager::Instance().ProcessGitUri(uri);

// GitContext contains validated information (no repository handle)
git_object *resolved_object = ctx.resolved_object;  // Validated reference object
string repo_path = ctx.repo_path;                   // Absolute path (for opening repo)
string file_path = ctx.file_path;                   // File path within repo
string final_ref = ctx.final_ref;                   // Validated reference

// Store paths in FunctionData (read-only, shared across threads)
auto bind_data = make_uniq<GitLogFunctionData>(repo_path, file_path);

// EXECUTE PHASE: Open repository in LocalTableFunctionState
// Each thread opens its own repository handle
struct GitLogLocalState : public LocalTableFunctionState {
    git_repository *repo = nullptr;  // Per-thread repository

    ~GitLogLocalState() {
        if (repo) git_repository_free(repo);  // RAII cleanup
    }
};

// In execute function:
if (!local_state.initialized) {
    git_repository_open(&local_state.repo, bind_data.resolved_repo_path.c_str());
    local_state.initialized = true;
}
```

### Example: git_log Implementation

```cpp
// FunctionData: Read-only, shared across all threads
struct GitLogFunctionData : public TableFunctionData {
    string repo_path;           // Original input
    string resolved_repo_path;  // Absolute path for opening
    string file_path;           // File filter (optional)
};

// LocalTableFunctionState: Per-thread, owns resources
struct GitLogLocalState : public LocalTableFunctionState {
    git_repository *repo = nullptr;
    git_revwalk *walker = nullptr;
    bool initialized = false;

    ~GitLogLocalState() {
        if (walker) git_revwalk_free(walker);
        if (repo) git_repository_free(repo);
    }
};

unique_ptr<FunctionData> GitLogBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<string> &names) {
    auto params = ParseUnifiedGitParams(input, 1);

    // Validate URI and get paths (no repo handle returned)
    auto ctx = GitContextManager::Instance().ProcessGitUri(params.repo_path_or_uri, params.ref);

    // Store validated paths in bind data
    auto result = make_uniq<GitLogFunctionData>(params.repo_path_or_uri, ctx.repo_path);
    result->file_path = ctx.file_path;
    return result;
}

void GitLogFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitLogFunctionData>();  // Read-only
    auto &local_state = data_p.local_state->Cast<GitLogLocalState>(); // Per-thread

    // Open repository in this thread's local state
    if (!local_state.initialized) {
        git_repository_open(&local_state.repo, bind_data.resolved_repo_path.c_str());
        git_revwalk_new(&local_state.walker, local_state.repo);
        local_state.initialized = true;
    }

    // Use local_state.repo (thread-safe)
}
```

### URI Processing

GitContextManager handles multiple URI formats:

```cpp
// Git URIs
ProcessGitUri("git://README.md@HEAD")
  → repo_path: ".", file_path: "README.md", ref: "HEAD"

ProcessGitUri("git://../other-repo@main")
  → repo_path: "/absolute/path/to/other-repo", file_path: "", ref: "main"

ProcessGitUri("git://src/main.cpp@feature-branch")
  → repo_path: ".", file_path: "src/main.cpp", ref: "feature-branch"

// Filesystem paths (with fallback ref)
ProcessGitUri(".", "HEAD")
  → repo_path: "/absolute/path/to/repo", file_path: "", ref: "HEAD"

ProcessGitUri("../other-repo", "main")
  → repo_path: "/absolute/path/to/other-repo", file_path: "", ref: "main"
```

## Thread-Safe Repository Management

### No Global Caching

GitContextManager **does not cache repositories**. It only validates URIs and returns paths. This design ensures:
- **Thread-safety**: No shared mutable state
- **Simplicity**: Clear ownership model
- **Correctness**: No stale repository handles

### Per-Thread Caching in LATERAL Functions

For LATERAL joins, repository caching happens in `LocalTableFunctionState` (per-thread):

```cpp
struct GitLogLocalState : public LocalTableFunctionState {
    // Regular table function state
    git_repository *repo = nullptr;
    bool initialized = false;

    // LATERAL optimization: per-thread cache
    string cached_repo_path;
    git_repository *cached_repo = nullptr;

    ~GitLogLocalState() {
        if (repo) git_repository_free(repo);
        if (cached_repo) git_repository_free(cached_repo);
    }
};

// In LATERAL execute function:
if (state.cached_repo_path != resolved_repo_path) {
    // Different repo - close old, open new
    if (state.cached_repo) {
        git_repository_free(state.cached_repo);
    }
    git_repository_open(&state.cached_repo, resolved_repo_path.c_str());
    state.cached_repo_path = resolved_repo_path;
}
// Reuse state.cached_repo for this row
```

### Performance Impact

**LATERAL joins with same repository** (optimized):
```sql
-- Each thread opens repository once, reuses for all rows
SELECT * FROM (VALUES (1), (2), (3)) t
CROSS JOIN LATERAL git_log_each('.');
-- Opens repository 1 time per thread, 50-80% faster
```

**LATERAL joins with different repositories**:
```sql
-- Cache hit/miss depends on consecutive rows
SELECT * FROM (VALUES ('.'), ('../other'), ('.')) t(repo)
CROSS JOIN LATERAL git_log_each(t.repo);
-- Opens: repo1, repo2, repo1 (cache miss on last)
-- Still faster than no caching: validates once, opens twice vs opens three times
```

## Error Handling

### Consistent Error Messages

All functions return the same errors for the same problems:

```sql
-- Invalid reference - all functions return same error
SELECT * FROM git_log('git://repo@invalid@@@ref');
-- Error: unable to parse OID

SELECT * FROM git_tree('git://repo@invalid@@@ref');
-- Error: unable to parse OID

SELECT * FROM git_read('git://repo/file@invalid@@@ref');
-- Error: unable to parse OID
```

### Error Types

| Condition | Error Message |
|-----------|---------------|
| Invalid reference | `unable to parse OID` |
| Malformed URI | `GitContextManager: Failed to parse URI '<uri>': <details>` |
| Repository not found | `GitContextManager: Failed to open repository '<path>': <details>` |

## GitContext Lifecycle

### RAII Resource Management

`GitContext` manages `resolved_object` with RAII:

```cpp
{
    auto ctx = GitContextManager::Instance().ProcessGitUri("git://file@HEAD");

    // Use ctx.resolved_object (validated reference)
    git_commit *commit = (git_commit *)ctx.resolved_object;

    // Use ctx.repo_path to open repository in LocalTableFunctionState
    // No manual cleanup of resolved_object needed
} // ctx.resolved_object automatically freed here

// Note: GitContextManager does NOT return or cache repository handles
// Functions must open repositories themselves using ctx.repo_path
```

### Move Semantics

`GitContext` supports move semantics for efficient return:

```cpp
GitContext ProcessGitUri(const string &uri, const string &fallback_ref = "HEAD") {
    // ... processing ...
    return GitContext(repo, obj, repo_path, file_path, final_ref);
    // Efficient move, no copy
}
```

## Implementation Files

### Core Files
- `src/include/git_context_manager.hpp` - Interface and GitContext struct
- `src/git_context_manager.cpp` - Implementation

### Functions Using GitContextManager
- `src/git_log.cpp` - Commit history queries
- `src/git_tree.cpp` - Directory traversal
- `src/git_read.cpp` - File reading
- `src/git_branches.cpp` - Branch operations
- `src/git_tags.cpp` - Tag operations
- `src/git_parents.cpp` - Parent commit relationships

### Tests
- `test/sql/test_git_context_manager.test` - Comprehensive test suite

## Design Decisions

### Why Singleton?

GitContextManager uses a singleton pattern for:
- Consistent validation logic across all function calls
- No need to pass state through DuckDB's function interface
- Single source of truth for URI parsing and path resolution

**Note**: The singleton does NOT cache repositories. It only provides stateless validation.

### Why NOT Cache Repositories Globally?

**Thread-safety concerns**:
- DuckDB table functions can execute in parallel
- Global cache would require mutex locking on every access
- Shared mutable state is error-prone

**Better approach - per-thread caching**:
- Each thread owns its repository handle in `LocalTableFunctionState`
- LATERAL functions cache within their thread (no locks needed)
- RAII ensures proper cleanup when thread completes
- Thread-safe by design

### Why Open Repository Temporarily in ProcessGitUri()?

GitContextManager opens repositories briefly to:
1. Validate that the repository exists
2. Resolve and validate the reference
3. Return validated information to the caller

The repository is immediately closed to:
- Keep GitContextManager stateless
- Avoid thread-safety issues
- Ensure clear ownership (LocalTableFunctionState owns repos)

### Why git_revparse_single() For All References?

`git_revparse_single()` is libgit2's most flexible reference resolver:
- Handles commit hashes, branch names, tags, HEAD
- Supports reflog syntax (`HEAD~1`, `main@{yesterday}`)
- Provides consistent error messages
- Single code path = easier to maintain

## See Also

- [Git URI Format](git_uris.md) - URI parsing and syntax details
- [Extension Architecture](../README.md#architecture) - Overall system design

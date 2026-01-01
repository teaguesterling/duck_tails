# Architecture

Duck Tails is built as a DuckDB extension using libgit2 for git operations.

## Overview

```
┌─────────────────────────────────────────────────────────┐
│                      DuckDB                              │
├─────────────────────────────────────────────────────────┤
│                   Duck Tails Extension                   │
├──────────────┬──────────────┬───────────────────────────┤
│ GitFileSystem│ Table Funcs  │ Scalar Functions          │
│   git://     │ git_log()    │ git_uri()                 │
│              │ git_tree()   │ text_diff()               │
│              │ git_read()   │                           │
├──────────────┴──────────────┴───────────────────────────┤
│                      libgit2                             │
└─────────────────────────────────────────────────────────┘
```

## Core Components

### GitFileSystem

Implements DuckDB's `FileSystem` interface for `git://` URLs.

**Key classes:**

- `GitFileSystem` - Main filesystem implementation
- `GitFileHandle` - Memory-backed file handle for git blobs
- `GitLFSFileHandle` - Streaming handle for LFS files
- `GitPath` - Parser for `git://path@revision` syntax

**Location:** `src/git_filesystem.cpp`, `src/include/git_filesystem.hpp`

```cpp
class GitFileSystem : public FileSystem {
    unique_ptr<FileHandle> OpenFile(const string &path, ...) override;
    vector<OpenFileInfo> Glob(const string &pattern, ...) override;
    bool FileExists(const string &filename, ...) override;
    // ...
};
```

### Table Functions

Each git function is registered as a DuckDB table function:

| File | Functions |
|------|-----------|
| `git_log.cpp` | `git_log`, `git_log_each` |
| `git_tree.cpp` | `git_tree`, `git_tree_each` |
| `git_read.cpp` | `git_read`, `git_read_each` |
| `git_branches.cpp` | `git_branches`, `git_branches_each` |
| `git_tags.cpp` | `git_tags`, `git_tags_each` |
| `git_parents.cpp` | `git_parents`, `git_parents_each` |

### LATERAL Support

The `_each` variants are designed for LATERAL joins. They:

1. Accept table function inputs
2. Return one row per input row
3. Error when called directly (not in LATERAL context)

### Text Diff Engine

Implements text diffing without external dependencies.

**Location:** `src/text_diff.cpp`, `src/include/text_diff.hpp`

**Functions:**
- `text_diff()` / `diff_text()` - Compute unified diff
- `text_diff_lines()` - Parse diff into lines
- `text_diff_stats()` - Compute diff statistics

## Data Flow

### Reading a Git File

```
1. User: SELECT * FROM read_csv('git://data.csv@HEAD')
2. DuckDB calls GitFileSystem::OpenFile()
3. GitPath::Parse() extracts repo/file/revision
4. FindGitRepository() discovers .git directory
5. libgit2 resolves revision to commit
6. libgit2 reads blob content
7. GitFileHandle wraps content in memory
8. DuckDB reads from handle like normal file
```

### Executing a Table Function

```
1. User: SELECT * FROM git_log()
2. DuckDB calls git_log_init()
3. Function opens git repository via libgit2
4. DuckDB calls git_log_func() repeatedly
5. Each call returns chunk of commits
6. libgit2 walks commit history
7. Results returned to user
```

## Repository Discovery

When given a path like `git://src/main.py@HEAD`:

1. Start from `src/main.py`
2. Walk up directory tree
3. Look for `.git` directory
4. Use libgit2's `git_repository_discover()`
5. Handle worktrees and submodules correctly
6. Return repository root path

```cpp
static string FindGitRepository(const string &path) {
    git_buf discovered_path = {0};
    git_repository_discover(&discovered_path, start_path.c_str(), 0, nullptr);
    // ...
}
```

## LFS Handling

LFS pointer files are automatically detected:

```
version https://git-lfs.github.com/spec/v1
oid sha256:abc123...
size 12345678
```

When detected:

1. Parse pointer for OID and size
2. Build local cache path: `.git/lfs/objects/ab/cd/abcd...`
3. Return `GitLFSFileHandle` instead of `GitFileHandle`
4. Stream content from local cache

## Memory Management

- RAII throughout (smart pointers)
- libgit2 objects wrapped with cleanup
- Repository cache to avoid repeated opens
- Content loaded on-demand

```cpp
// Repository cache
unordered_map<string, git_repository*> repo_cache_;

// RAII cleanup in destructor
~GitFileSystem() {
    for (auto &entry : repo_cache_) {
        git_repository_free(entry.second);
    }
    git_libgit2_shutdown();
}
```

## Error Handling

Errors are propagated as DuckDB exceptions:

```cpp
if (error != 0) {
    const git_error *e = git_error_last();
    throw IOException("Failed to open repository: %s", e->message);
}
```

Common error types:
- `IOException` - File/repository access errors
- `InternalException` - Programming errors
- `BinderError` - Function usage errors

## Extension Registration

All components are registered in `duck_tails_extension.cpp`:

```cpp
void DuckTailsExtension::Load(ExtensionLoader &loader) {
    RegisterGitFileSystem(loader);   // git:// protocol
    RegisterGitLogFunction(loader);  // git_log()
    RegisterGitTreeFunction(loader); // git_tree()
    // ...
}
```

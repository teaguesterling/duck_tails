# Code Structure

## Overview

Duck Tails organizes git functionality into separate, focused modules. This document explains the file organization and where to add new features.

## File Organization

### Git Table Functions

Each git table function lives in its own file with a consistent pattern:

```
src/
├── git_log.cpp          - git_log() and git_log_each()
├── git_tree.cpp         - git_tree() and git_tree_each()
├── git_branches.cpp     - git_branches() and git_branches_each()
├── git_tags.cpp         - git_tags() and git_tags_each()
├── git_parents.cpp      - git_parents() and git_parents_each()
└── git_read.cpp         - git_read() and git_read_each()
```

**Why separate files?**
- Easier to navigate (find `git_log` implementation → open `git_log.cpp`)
- Reduced merge conflicts
- Clear ownership of functionality
- Smaller compile units

### Core Infrastructure

```
src/
├── git_context_manager.cpp  - Unified URI processing and repository caching
├── git_filesystem.cpp        - Git filesystem implementation (git:// URIs)
├── git_path.cpp             - Path parsing and normalization
├── git_uri.cpp              - git_uri() helper function
├── git_utils.cpp            - Shared utilities (parameter parsing, etc.)
└── git_functions.cpp        - Registration hub (calls all Register* functions)
```

### Headers

```
src/include/
├── git_context_manager.hpp  - GitContextManager interface
├── git_filesystem.hpp        - Git filesystem and GitPath
├── git_functions.hpp         - All function forward declarations
└── duck_tails_extension.hpp - Extension entry point
```

## Function Pattern: Regular vs _each

Every git table function comes in two variants:

### Regular Function (e.g., `git_log`)

**Purpose**: Query with static parameters known at planning time

**Example**:
```sql
SELECT * FROM git_log();                    -- Default repo
SELECT * FROM git_log('/path/to/repo');     -- Explicit repo
SELECT * FROM git_log('git://file@HEAD');   -- Git URI
```

**Implementation**: Standard DuckDB table function
- `GitLogBind` - Parse parameters, validate repository
- `GitLogInitGlobal` - Initialize global state
- `GitLogFunction` - Generate rows

### _each Function (e.g., `git_log_each`)

**Purpose**: LATERAL join support with dynamic parameters from input rows

**Example**:
```sql
-- Process different repos from a table
SELECT r.name, l.commit_hash
FROM repos r
CROSS JOIN LATERAL git_log_each(r.path) l;

-- Dynamic git URIs from query results
SELECT t.ref, l.message
FROM refs t
CROSS JOIN LATERAL git_log_each('git://@' || t.ref) l;
```

**Implementation**: LATERAL table function (in/out pattern)
- `GitLogEachBind` - Set up schema (no repo validation yet)
- `GitLogLocalInit` - Initialize per-thread state
- `GitLogEachFunction` - Process input rows, generate output rows
  - Uses `in_out_function` pattern
  - Reads repo/URI from input `DataChunk`
  - Outputs results to same `DataChunk`

## File Structure Template

Each git function file follows this structure:

```cpp
// 1. Includes
#include "git_functions.hpp"
#include "git_context_manager.hpp"
// ... other includes

namespace duckdb {

// 2. Bind Data Structures
struct GitLogBindData : public TableFunctionData {
    string repo_path;
    string ref;
    // ... other fields
};

// 3. Local State (if needed)
struct GitLogLocalState : public LocalTableFunctionState {
    // Per-thread state
};

// 4. Regular Function Implementation
unique_ptr<FunctionData> GitLogBind(...) {
    // Parse parameters
    // Validate with GitContextManager
    // Return bind data
}

unique_ptr<GlobalTableFunctionState> GitLogInitGlobal(...) {
    // Initialize global state
}

void GitLogFunction(...) {
    // Generate output rows
}

// 5. _each Function Implementation
unique_ptr<FunctionData> GitLogEachBind(...) {
    // Set up schema only (no validation)
}

unique_ptr<LocalTableFunctionState> GitLogLocalInit(...) {
    // Initialize per-thread state
}

OperatorResultType GitLogEachFunction(...) {
    // Read input rows
    // Process each row
    // Generate output rows
    // Return NEED_MORE_INPUT or FINISHED
}

// 6. Registration Function
void RegisterGitLogFunction(ExtensionLoader &loader) {
    // Register git_log (regular)
    TableFunction git_log_func("git_log", ...);
    loader.RegisterFunction(git_log_func);

    // Register git_log_each (LATERAL)
    TableFunction git_log_each(...);
    git_log_each.in_out_function = GitLogEachFunction;
    loader.RegisterFunction(git_log_each);
}

} // namespace duckdb
```

## Adding a New Git Function

### 1. Create the File

Create `src/git_newfunc.cpp`:

```cpp
#include "git_functions.hpp"
#include "git_context_manager.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// Bind data
struct GitNewFuncBindData : public TableFunctionData {
    string repo_path;
    // ... your fields
};

// Regular function
unique_ptr<FunctionData> GitNewFuncBind(ClientContext &context,
                                         TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types,
                                         vector<string> &names) {
    // 1. Parse parameters
    string repo_path = input.inputs.empty() ? "." : input.inputs[0].GetValue<string>();

    // 2. Validate with GitContextManager
    auto ctx = GitContextManager::Instance().ProcessGitUri(repo_path, "HEAD");

    // 3. Set return schema
    return_types = {LogicalType::VARCHAR, ...};
    names = {"column1", ...};

    // 4. Return bind data
    auto result = make_uniq<GitNewFuncBindData>();
    result->repo_path = ctx.repo_path;
    return std::move(result);
}

void GitNewFuncFunction(ClientContext &context, TableFunctionInput &data_p,
                        DataChunk &output) {
    auto &bind_data = (GitNewFuncBindData &)*data_p.bind_data;

    // Open repository
    git_repository *repo;
    git_repository_open(&repo, bind_data.repo_path.c_str());

    // TODO: Use GitContextManager's cached repo instead!

    // Generate rows
    // ...

    git_repository_free(repo);
}

// _each function (LATERAL)
OperatorResultType GitNewFuncEachFunction(ExecutionContext &context,
                                           TableFunctionInput &data_p,
                                           DataChunk &input,
                                           DataChunk &output) {
    // Read repo_path from input
    // Process
    // Write to output
    // Return NEED_MORE_INPUT or FINISHED
}

void RegisterGitNewFuncFunction(ExtensionLoader &loader) {
    // Register regular function
    TableFunction git_newfunc("git_newfunc", {LogicalType::VARCHAR},
                              GitNewFuncFunction, GitNewFuncBind);
    loader.RegisterFunction(git_newfunc);

    // Register _each variant
    TableFunction git_newfunc_each("git_newfunc_each", {LogicalType::VARCHAR});
    git_newfunc_each.in_out_function = GitNewFuncEachFunction;
    loader.RegisterFunction(git_newfunc_each);
}

} // namespace duckdb
```

### 2. Add Forward Declaration

In `src/include/git_functions.hpp`:

```cpp
// Forward declaration
void RegisterGitNewFuncFunction(ExtensionLoader &loader);
```

### 3. Register in Main Hub

In `src/git_functions.cpp`:

```cpp
void RegisterGitFunctions(ExtensionLoader &loader) {
    RegisterGitLogFunction(loader);
    RegisterGitBranchesFunction(loader);
    // ... other functions
    RegisterGitNewFuncFunction(loader);  // Add this
}
```

### 4. Add to Build

In `CMakeLists.txt`:

```cmake
set(EXTENSION_SOURCES
    src/duck_tails_extension.cpp
    src/git_filesystem.cpp
    # ... other files
    src/git_newfunc.cpp  # Add this
)
```

### 5. Add Tests

Create `test/sql/git_newfunc.test`:

```sql
require duck_tails

# Test basic functionality
query I
SELECT COUNT(*) FROM git_newfunc();
----
(expected count)

# Test LATERAL variant
query I
SELECT COUNT(*) FROM (VALUES ('.'), ('../other-repo')) t(path)
CROSS JOIN LATERAL git_newfunc_each(t.path);
----
(expected count)
```

## Common Patterns

### Using GitContextManager

**Always** use GitContextManager for URI processing:

```cpp
// Good - consistent and cached
auto ctx = GitContextManager::Instance().ProcessGitUri(uri, "HEAD");
git_repository *repo = ctx.repo;
string repo_path = ctx.repo_path;
string file_path = ctx.file_path;

// Bad - manual and inconsistent
GitPath git_path = GitPath::Parse(uri);
git_repository *repo;
git_repository_open(&repo, git_path.repository_path.c_str());
```

### Parameter Parsing

For LATERAL functions, use `ParseLateralGitParams`:

```cpp
#include "git_utils.hpp"

unique_ptr<FunctionData> GitNewFuncEachBind(...) {
    auto params = ParseLateralGitParams(input, 1);  // ref at index 1
    // params.repo_path_or_uri
    // params.ref (defaults to "HEAD")
    return make_uniq<GitNewFuncBindData>(params.ref);
}
```

### Error Handling

Use consistent error patterns:

```cpp
try {
    auto ctx = GitContextManager::Instance().ProcessGitUri(uri);
    // ... use ctx
} catch (const std::exception &e) {
    throw BinderException("git_newfunc: %s", e.what());
}
```

## File Responsibilities

| File | Responsibility |
|------|----------------|
| `git_log.cpp` | Commit history queries |
| `git_tree.cpp` | Directory/file tree traversal |
| `git_branches.cpp` | Branch listing |
| `git_tags.cpp` | Tag listing |
| `git_parents.cpp` | Commit parent relationships |
| `git_read.cpp` | File content reading |
| `git_uri.cpp` | URI construction helper |
| `git_context_manager.cpp` | URI processing, repository caching |
| `git_filesystem.cpp` | VFS implementation for `git://` |
| `git_path.cpp` | Path parsing (GitPath::Parse) |
| `git_utils.cpp` | Shared utilities (ParseLateralGitParams) |
| `git_functions.cpp` | Registration hub |

## What Goes Where?

### Adding a new git table function?
→ Create `src/git_<name>.cpp`

### Adding helper utilities used by multiple functions?
→ Add to `src/git_utils.cpp` and `src/include/git_functions.hpp`

### Modifying URI parsing?
→ Edit `src/git_filesystem.cpp` (GitPath::Parse)

### Improving repository caching?
→ Edit `src/git_context_manager.cpp`

### Adding a new column to all functions?
→ Update each `src/git_*.cpp` file's return schema

### Changing error messages?
→ Update `src/git_context_manager.cpp` (for URI/repo errors) or individual function files

## See Also

- [GitContextManager Documentation](GitContextManager.md) - URI processing and caching
- [Git URI Format](git-uris.md) - URI syntax and parsing
- [Testing Guide](../test/README.md) - How to test your changes

# Duck Tails Phase 2: Intelligent Diff Architecture

## Design Decisions Made

### 1. Function Architecture (Following DuckDB Conventions)

**Table-Valued Functions (File I/O):**
- `read_git_diff(path)` - File against HEAD  
- `read_git_diff(path1, path2)` - Between any two files/git refs
- Returns: `(diff_text, [metadata...])` 

**Scalar Functions (Data Processing):**
- `diff_text(string1, string2)` - Pure text diffing
- Future: `diff_text_extract_changes(diff_text)` for structured parsing

### 2. Smart Path Detection Logic

```cpp
if (both_paths_are_git_urls) {
    // Use libgit2 native diff
} else if (one_path_is_git_url || is_in_git_repo(path)) {
    // Mixed: read via filesystems, then diff_text()
    // Supports: read_git_diff('local.txt', 's3://bucket/file.txt')
} else {
    // Pure "no-index" diff like git --no-index
}
```

### 3. Parameterized Metadata
- `include_metadata := false` → `(diff_text)`
- `include_metadata := true` → `(diff_text, path1, path2, [more...])`

### 4. Text-First Approach
- Store diffs as text strings (memory efficient, proven format)
- Parse to structured data on-demand when needed
- Leverages existing diff standards and tooling

## Implementation Status

### ✅ Completed
- TextDiff data type and basic diff computation
- text_diff() scalar function  
- Basic git:// filesystem integration from Phase 1

### 🚧 Current Task
- Refactor semantic_diff() → diff_text()
- Implement read_git_diff() table function
- Integrate with existing git:// filesystem

### 📋 Next Steps
1. Refactor semantic_diff to diff_text
2. Create read_git_diff table function with file reading logic
3. Test mixed path scenarios (local + git://, S3 + git://, etc.)
4. Add metadata parameter support
5. Implement smart git vs no-index detection

## Key Innovation
Making git diffing a special case of general file diffing, leveraging all of DuckDB's file system integrations (S3, HTTP, local, git://) seamlessly.
# Duck Tails Data Ingestion Documentation

## Overview

Duck Tails does not perform traditional data ingestion or ETL. Instead, it provides **real-time access** to Git repository data through table functions that query Git objects on-demand using libgit2. There is no persistent storage or intermediate database - all data flows directly from Git repositories to SQL results.

## Architecture

```
┌─────────────┐     ┌──────────────┐     ┌─────────────┐
│   SQL Query │────▶│  Duck Tails  │────▶│   libgit2   │
└─────────────┘     │   Extension  │     └─────────────┘
                    └──────────────┘            │
                                                ▼
                                        ┌──────────────┐
                                        │ .git Directory│
                                        │  (Git Objects)│
                                        └──────────────┘
```

## Data Flow Pipeline

### 1. Query Initiation

When a user executes a query like `SELECT * FROM git_tree('.', 'main')`:

1. **DuckDB Parser**: Parses SQL and identifies table function
2. **Function Binding**: Duck Tails bind function validates parameters
3. **Execution Planning**: DuckDB creates execution plan with Duck Tails operator

### 2. Repository Discovery

**Input**: Repository path or current directory
**Process**:
```cpp
GitPath::Parse(repo_path) -> {
    1. Remove git:// prefix if present
    2. Resolve relative paths to absolute
    3. Walk up directory tree looking for .git
    4. Cache discovered repository path
}
```
**Output**: Absolute path to repository root

### 3. Reference Resolution

**Input**: Branch name, tag, commit SHA, or HEAD
**Process**:
```cpp
ResolveReference(ref) -> {
    1. Open repository with libgit2
    2. Try as direct SHA first
    3. Try as branch (refs/heads/ref)
    4. Try as tag (refs/tags/ref)
    5. Try as remote branch (refs/remotes/*/ref)
    6. Parse special syntax (HEAD~2, main^)
}
```
**Output**: Full 40-character commit SHA

### 4. Data Extraction by Function

#### git_tree Data Flow

```
Repository Path + Ref
        ↓
    [Open Repo]
        ↓
    [Resolve to Commit]
        ↓
    [Get Tree from Commit]
        ↓
    [Walk Tree Recursively]
        ↓
    For each entry:
    - Extract path, mode, type
    - Get blob size if file
    - Build full path
    - Emit row
```

**libgit2 calls**:
- `git_repository_open()`
- `git_revparse_single()` 
- `git_commit_tree()`
- `git_tree_walk()`
- `git_blob_lookup()` for sizes

#### git_log Data Flow

```
Repository Path + Ref/Range
        ↓
    [Initialize Revwalk]
        ↓
    [Push/Hide Commits for Range]
        ↓
    [Walk Commit History]
        ↓
    For each commit:
    - Extract all metadata
    - Parse message
    - Get parent SHAs
    - Emit row
```

**libgit2 calls**:
- `git_revwalk_new()`
- `git_revwalk_push()` / `git_revwalk_hide()`
- `git_revwalk_next()`
- `git_commit_lookup()`
- `git_commit_message()`, `git_commit_author()`

#### git_branches Data Flow

```
Repository Path
        ↓
    [Create Branch Iterator]
        ↓
    [Iterate All Branches]
        ↓
    For each branch:
    - Get branch name
    - Resolve to commit
    - Check if HEAD
    - Get upstream
    - Emit row
```

**libgit2 calls**:
- `git_branch_iterator_new()`
- `git_branch_next()`
- `git_branch_name()`
- `git_branch_upstream()`

#### git_tags Data Flow

```
Repository Path
        ↓
    [Iterate References]
        ↓
    [Filter refs/tags/*]
        ↓
    For each tag:
    - Check if annotated
    - Peel to commit
    - Extract tag message
    - Get tagger info
    - Emit row
```

**libgit2 calls**:
- `git_reference_iterator_new()`
- `git_reference_next()`
- `git_tag_lookup()` for annotated
- `git_tag_peel()`

#### git_read Data Flow

```
File Path + Ref
        ↓
    [Resolve Ref to Commit]
        ↓
    [Get Tree from Commit]
        ↓
    [Navigate to File Path]
        ↓
    [Load Blob Content]
        ↓
    - Return raw bytes
    - Attempt UTF-8 decode
    - Emit row
```

**libgit2 calls**:
- `git_tree_entry_bypath()`
- `git_blob_lookup()`
- `git_blob_rawcontent()`
- `git_blob_rawsize()`

#### git_parents Data Flow

```
Commit Hash
        ↓
    [Lookup Commit Object]
        ↓
    [Get Parent Count]
        ↓
    For each parent:
    - Get parent SHA
    - Lookup parent commit
    - Extract metadata
    - Emit row
```

**libgit2 calls**:
- `git_commit_lookup()`
- `git_commit_parentcount()`
- `git_commit_parent()`

### 5. Result Materialization

**DuckDB Integration**:
1. Each row is pushed to DuckDB's DataChunk
2. Type conversion: Git data → DuckDB types
   - SHA strings → VARCHAR
   - Timestamps → TIMESTAMP
   - File content → BLOB/VARCHAR
3. Memory management handled by DuckDB
4. Results streamed (not fully materialized)

## Data Transformation

### String Processing
- **Commit messages**: Split into subject (first line) and body
- **Author strings**: Formatted as "Name <email>"
- **Paths**: Normalized with forward slashes
- **URIs**: Constructed as `git://repo/path@ref`

### Type Conversions
- **Git OID** (20 bytes) → VARCHAR (40 chars hex)
- **git_time** → DuckDB TIMESTAMP
- **File modes** (integer) → VARCHAR (octal string)
- **Binary content** → BLOB
- **UTF-8 content** → VARCHAR

### Special Handling

#### Range Specifications
- `A..B` (two-dot): Commits in B but not in A
  - Implementation: `revwalk.push(B); revwalk.hide(A)`
- `A...B` (three-dot): Commits in A or B but not both
  - Implementation: Find merge-base, exclude it

#### Path Resolution
- Relative paths resolved from repository root
- Symlinks not followed (returned as symlink entries)
- Case sensitivity depends on filesystem

#### Binary Detection
- Files checked for UTF-8 validity
- Binary files have `text` column as NULL
- Content always available in `content` BLOB column

## Performance Optimizations

### Caching Strategy
1. **Repository discovery**: Cached per query
2. **No object caching**: Each query reopens repository
3. **Reference resolution**: Not cached (may change)

### Lazy Evaluation
- Tree walking stops at LIMIT
- Commit history uses streaming
- File content loaded on-demand

### Memory Management
- Large blobs may cause OOM
- No streaming for file content
- DataChunks limited to DuckDB's vector size (2048 rows)

## Error Handling Flow

```
Error Detection
      ↓
[libgit2 error code]
      ↓
[Get error message]
      ↓
[Throw DuckDB exception]
      ↓
Query fails with message
```

Common error paths:
- Repository not found → "Not a git repository"
- Invalid reference → "Reference 'x' not found"
- Permission denied → "Failed to open repository"
- Corrupt objects → "Invalid git object"

## Integration Points

### With DuckDB Readers

Git URIs can be passed to DuckDB's filesystem:
```
git_tree() → git_file_uri → read_csv(uri)
                          → read_json_auto(uri)
                          → read_parquet(uri)
```

The Git filesystem handler intercepts `git://` URIs and:
1. Parses the URI to extract repo, path, and ref
2. Uses same data flow as git_read
3. Returns file content to the reader

### With LATERAL Joins

_each variants enable column references:
```sql
table_with_refs, LATERAL git_tree_each(ref_column)
```

Flow:
1. For each row in left table
2. Extract column value
3. Execute git function with that value
4. Join results

## Monitoring and Debugging

### Performance Metrics
- Repository open time
- Tree walking duration
- Commit history traversal speed
- Memory usage for large files

### Debug Information
- EXPLAIN shows function calls
- libgit2 errors include Git error codes
- Repository path in error messages

### Bottlenecks
1. **Large repositories**: Millions of objects slow
2. **Deep history**: Long commit chains expensive
3. **Big files**: Must fit in memory
4. **Wide trees**: Many files increase overhead

## Data Freshness

- **Real-time**: Always current repository state
- **No staleness**: No caching between queries
- **Consistency**: Single repository snapshot per query
- **Atomicity**: Each function call is independent

## Security Considerations

1. **Read-only**: Cannot modify repositories
2. **Filesystem access**: Respects OS permissions
3. **No network**: Local repositories only
4. **Path traversal**: Prevented by libgit2
5. **Resource limits**: Constrained by DuckDB limits

## Future Ingestion Enhancements

Potential improvements for data flow:
- Streaming large files
- Caching frequently accessed objects
- Parallel tree walking
- Incremental history traversal
- Background repository indexing
- Change data capture from Git hooks
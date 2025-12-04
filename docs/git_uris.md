# Git URI Specification

## Format

Git URIs in Duck Tails follow this format:
```
git://path@revision
```

The `path` component can be:
- A repository path: `git://.@HEAD` or `git://path/to/repo@main`
- A file path: `git://README.md@HEAD` or `git://src/main.cpp@v1.0`
- Both: `git://path/to/repo/src/main.cpp@HEAD`

Repository discovery automatically finds the `.git` directory by walking up from the provided path.

## Components

### Prefix
- `git://` - Required prefix that identifies this as a git URI

### Path
- Combined repository and file path
- Can be absolute or relative
- Repository is discovered by walking up the directory tree
- Examples: `.`, `README.md`, `src/main.cpp`, `/absolute/path/to/repo/file.txt`

### File Path Resolution
After repository discovery, the remaining path becomes the file path:
- `git://README.md@HEAD` → repo: `.` (discovered), file: `README.md`
- `git://src/file.cpp@main` → repo: `.` (discovered), file: `src/file.cpp`
- `git://repo/src/main.cpp@v1.0` → repo: `repo/`, file: `src/main.cpp`

### Revision
- Git reference after the `@` symbol
- Defaults to `HEAD` if omitted
- Types:
  - Commit SHA: `@abc123`
  - Branch: `@main`, `@feature/new-thing`
  - Tag: `@v1.0.0`
  - Relative refs: `@HEAD~1`, `@main^2`, `@HEAD~5`
  - Reflog syntax: `@HEAD@{0}`, `@main@{1.day.ago}`

**Note:** Range syntax (`@v1.0..v2.0` or `@main...feature`) in the revision component is not currently supported in git URIs. This is unrelated to the `..` path security restriction above.

## Repository Discovery

When a git URI is parsed, the system discovers the repository by:
1. Starting from the specified path
2. Walking up the directory tree looking for `.git`
3. Using the first (deepest) repository found

### Examples
- `git://README.md@HEAD` → Discovers repo from current directory
- `git://src/file.cpp@main` → Discovers repo from `src/` upward
- `git://test/tmp/repo/file.md@v1.0` → Finds nested repository at `test/tmp/repo`

## Edge Cases

### Path Normalization
- Leading `./` is stripped for cleaner paths
- Trailing slashes are normalized: `git://repo/` becomes `git://repo`
- **Parent directory (`..`) handling:**
  - ✅ **Repository discovery**: `..` is resolved to absolute paths
    - `git://../other-repo@main` → Finds sibling repository
    - `../other-repo` → Resolves to `/absolute/path/to/other-repo`
  - ❌ **File paths extracted from URIs**: `..` is forbidden for security
    - Example: `git://../../etc/passwd@HEAD` → Rejected
    - Prevents path traversal to escape repository boundaries
    - Applies to file path component AFTER repository discovery

### Non-existent Files
- Files don't need to exist in the working tree
- Only needs to exist in the specified git revision
- Example: Query deleted files from history: `git://deleted-file.txt@old-commit`

### Special Characters
- Spaces and special characters in paths should be escaped or quoted when used in SQL
- Internal handling preserves paths as-is

### Nested Repositories
- Discovers the most specific (deepest) repository first
- `git://outer/inner/file.txt` uses `inner/` repo if it exists, otherwise `outer/`

### Empty Components
- `git://@HEAD` - Invalid (no repo path)
- `git://.@HEAD` - Valid (current directory, no file path)
- `git://./file.txt@` - Valid (defaults to HEAD)
- `git://./file.txt` - Invalid (missing @ separator)

## The git_uri() Function

Constructs a properly formatted git URI from components:

```sql
git_uri(repo_path, file_path, revision) → VARCHAR
```

### Parameters
- `repo_path` (VARCHAR): Repository path (can be `.` for current directory)
- `file_path` (VARCHAR): File path within repository (use empty string `''` for repository root)
- `revision` (VARCHAR): Git revision (commit/branch/tag)

### Examples
```sql
-- Basic usage
SELECT git_uri('.', 'README.md', 'HEAD');
-- Returns: 'git://./README.md@HEAD'

-- With nested repository
SELECT git_uri('test/tmp/main-repo', 'src/main.cpp', 'v1.0');
-- Returns: 'git://test/tmp/main-repo/src/main.cpp@v1.0'

-- Repository root
SELECT git_uri('/abs/path/repo', '', 'main');
-- Returns: 'git:///abs/path/repo@main'
```

## Usage in Duck Tails Functions

### Reading Files
```sql
-- Read file from git
SELECT * FROM read_csv('git://data/sales.csv@HEAD');

-- Using git_read
SELECT * FROM git_read('git://README.md@main');
```

### Tree Operations
```sql
-- git_tree outputs git_uri column
SELECT git_uri FROM git_tree('HEAD');
-- Returns URIs like: 'git://./src/main.cpp@abc123'

-- Using URIs to read specific files
SELECT * FROM git_tree('HEAD') t
JOIN LATERAL git_read_each(t.git_uri) r ON TRUE;
```

### Diff Operations
```sql
-- Compare two versions (takes two separate git URIs)
SELECT * FROM read_git_diff('git://file.txt@v1.0', 'git://file.txt@v2.0');

-- Compare against HEAD (single argument defaults to @HEAD comparison)
SELECT * FROM read_git_diff('git://file.txt@v1.0');
```

## Function Parameter Conventions

Duck Tails git functions accept their first parameter in two forms:

### 1. Filesystem Path (repo_path)
Some functions accept an optional revision as the second parameter:
```sql
-- Functions with optional ref parameter: git_tree, git_parents
SELECT * FROM git_tree('.', 'v1.0.0');
SELECT * FROM git_parents('.', 'main');

-- Functions with single parameter only: git_log, git_branches, git_tags
SELECT * FROM git_log('.');
SELECT * FROM git_branches('path/to/repo');

-- Without revision parameter (defaults to HEAD)
SELECT * FROM git_tree('path/to/repo');
```

### 2. Git URI
When passing a git:// URI, the revision is embedded in the URI:
```sql
-- All functions accept git URIs
SELECT * FROM git_log('git://path/to/repo@main');
SELECT * FROM git_tree('git://.@v1.0.0');
SELECT * FROM git_parents('git://.@feature-branch');

-- Note: Cannot pass revision parameter when using git:// URI
-- This would error: git_tree('git://repo@main', 'other-branch')
```

### Default Revisions
- Filesystem paths without revision parameter: defaults to `HEAD`
- Git URIs without `@revision`: invalid (must include @ separator)
- Empty revision after @: defaults to `HEAD` (e.g., `git://file.txt@`)

## Implementation Notes

- All URI construction should use `git_uri()` function for consistency
- Repository discovery happens at parse time via `GitPath::Parse`
- URIs are normalized to absolute repository paths internally
- The `git_uri` column in git functions provides ready-to-use URIs
- Functions validate that git:// URIs don't conflict with revision parameters

## Schema Standardization

Duck Tails functions follow a consistent schema:

**Key features:**
- All file-based functions return `git_uri` as the first column
- URIs are complete and ready to use with other functions
- Columns follow a standard order: `git_uri`, `repo_path`, `commit_hash`, etc.
- Enhanced metadata enables cross-function compatibility
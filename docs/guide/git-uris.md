# Git URI Syntax

Duck Tails uses a `git://` URI scheme to reference files and repositories at specific revisions.

## Basic Format

```
git://path@revision
```

| Component | Description | Example |
|-----------|-------------|---------|
| `git://` | Required protocol prefix | - |
| `path` | Repository and/or file path | `README.md`, `./src/main.py` |
| `@` | Revision separator | - |
| `revision` | Git reference | `HEAD`, `main`, `v1.0`, `abc1234` |

## Path Component

The path can specify:

### File Only (Repository Auto-Discovered)

```sql
-- File in current repository
'git://README.md@HEAD'
'git://src/main.py@main'
'git://data/sales.csv@v1.0'
```

### Repository Only

```sql
-- Current directory
'git://.@HEAD'

-- Relative path
'git://../other-repo@main'

-- Absolute path
'git:///home/user/projects/myrepo@HEAD'
```

### Repository + File

```sql
-- Explicit repository with file
'git://./src/main.py@HEAD'
'git://../other-repo/config.json@main'
'git:///absolute/path/repo/file.txt@v1.0'
```

## Revision Component

### Branch Names

```sql
'git://file.txt@main'
'git://file.txt@develop'
'git://file.txt@feature/new-feature'
```

### Tag Names

```sql
'git://file.txt@v1.0.0'
'git://file.txt@release-2024'
```

### Commit Hashes

```sql
-- Full hash
'git://file.txt@abc123def456789...'

-- Short hash (minimum 4 characters)
'git://file.txt@abc123'
```

### Relative References

```sql
-- Parent commit
'git://file.txt@HEAD~1'
'git://file.txt@main~3'

-- Second parent (merge commits)
'git://file.txt@HEAD^2'

-- Combining
'git://file.txt@HEAD~2^2'
```

### Reflog Syntax

```sql
-- Reflog by index
'git://file.txt@HEAD@{0}'
'git://file.txt@main@{1}'

-- Reflog by time
'git://file.txt@HEAD@{1.day.ago}'
'git://file.txt@main@{2024-01-15}'
```

## Repository Discovery

When parsing a git URI, Duck Tails automatically discovers the repository:

1. Starts from the specified path
2. Walks up the directory tree looking for `.git`
3. Uses the first (deepest) repository found

### Examples

```sql
-- Discovers repo from current directory
'git://README.md@HEAD'

-- Discovers repo from src/ upward
'git://src/file.cpp@main'

-- Finds nested repository
'git://test/fixtures/repo/file.md@v1.0'
```

## Path Normalization

### Trailing Slashes

Trailing slashes are normalized:

```sql
'git://repo/@HEAD'  -- becomes 'git://repo@HEAD'
```

### Current Directory

```sql
'git://.@HEAD'       -- Current directory, no file
'git://./file.txt@HEAD'  -- Current directory with file
```

### Parent Directory

The `..` path component is supported for repository discovery:

```sql
-- Access sibling repository
'git://../other-repo@main'
'git://../other-repo/file.txt@HEAD'
```

!!! warning "Security Restriction"
    After repository discovery, `..` in file paths is rejected to prevent path traversal attacks.

## Special Cases

### Non-Existent Working Directory Files

Files don't need to exist in the working tree - only in the specified revision:

```sql
-- Query a deleted file from history
SELECT * FROM git_read('git://deleted-file.txt@old-commit');
```

### Special Characters

Paths with special characters should be quoted appropriately in SQL:

```sql
SELECT * FROM git_read('git://path with spaces/file.txt@HEAD');
```

## The `git_uri()` Function

Construct git URIs programmatically:

```sql
SELECT git_uri(repo_path, file_path, revision);
```

### Parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `repo_path` | VARCHAR | Repository path (use `.` for current) |
| `file_path` | VARCHAR | File path within repository (use `''` for root) |
| `revision` | VARCHAR | Git reference |

### Examples

```sql
-- Basic usage
SELECT git_uri('.', 'README.md', 'HEAD');
-- Returns: 'git://./README.md@HEAD'

-- Repository root
SELECT git_uri('/path/repo', '', 'main');
-- Returns: 'git:///path/repo@main'

-- Dynamic URI construction
SELECT git_uri(repo_path, 'config.json', commit_hash)
FROM git_log()
LIMIT 5;
```

## Using Git URIs with DuckDB Functions

Git URIs work with standard DuckDB file-reading functions:

```sql
-- CSV files
SELECT * FROM read_csv('git://data.csv@HEAD');

-- JSON files
SELECT * FROM read_json('git://config.json@main');

-- Parquet files
SELECT * FROM read_parquet('git://data.parquet@v1.0');

-- Text files
SELECT * FROM read_text('git://README.md@HEAD');
```

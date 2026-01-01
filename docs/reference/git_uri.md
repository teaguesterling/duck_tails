# git_uri

Construct a git URI from components.

## Syntax

```sql
git_uri(repo_path, file_path, revision) → VARCHAR
```

## Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `repo_path` | VARCHAR | Yes | Repository path (use `.` for current) |
| `file_path` | VARCHAR | Yes | File path within repository (use `''` for root) |
| `revision` | VARCHAR | Yes | Git reference (commit, branch, tag) |

## Returns

| Type | Description |
|------|-------------|
| VARCHAR | Formatted git:// URI |

## Examples

### Basic Usage

```sql
SELECT git_uri('.', 'README.md', 'HEAD');
-- Returns: 'git://./README.md@HEAD'

SELECT git_uri('.', 'src/main.py', 'main');
-- Returns: 'git://./src/main.py@main'
```

### Repository Root

```sql
SELECT git_uri('/path/to/repo', '', 'HEAD');
-- Returns: 'git:///path/to/repo@HEAD'

SELECT git_uri('.', '', 'v1.0');
-- Returns: 'git://.@v1.0'
```

### With Commit Hash

```sql
SELECT git_uri('.', 'file.txt', 'abc1234');
-- Returns: 'git://./file.txt@abc1234'
```

### Dynamic URI Construction

```sql
-- Build URIs for all Python files
SELECT git_uri('.', file_path, 'HEAD') as uri
FROM git_tree('HEAD')
WHERE file_ext = '.py';
```

### Use with LATERAL Joins

```sql
-- Read file at each commit
SELECT
    l.commit_hash,
    l.author_date,
    r.size_bytes
FROM git_log() l,
     LATERAL git_read_each(
         git_uri('.', 'config.json', l.commit_hash)
     ) r
LIMIT 20;
```

### Cross-Repository Queries

```sql
-- Build URIs for different repositories
SELECT git_uri(repo, 'README.md', 'HEAD') as uri
FROM (VALUES ('.'), ('../lib'), ('../app')) as t(repo);
```

### Combine with git_tree

```sql
-- The git_tree function already provides git_uri column
SELECT
    t.git_uri,              -- Pre-built URI
    git_uri('.', t.file_path, 'main') as main_uri  -- Build custom URI
FROM git_tree('HEAD') t
WHERE t.file_ext = '.md';
```

## Use Cases

### Version Comparison

```sql
-- Compare same file at two versions
SELECT
    git_uri('.', 'data.csv', 'v1.0') as old_version,
    git_uri('.', 'data.csv', 'v2.0') as new_version;
```

### Dynamic File Access

```sql
-- Build URI based on query results
SELECT
    git_uri('.', file_path, commit_hash) as file_uri
FROM (
    SELECT file_path, commit_hash
    FROM git_tree('HEAD')
    WHERE size_bytes > 10000
) large_files;
```

## Notes

- The function handles path normalization automatically
- Empty `file_path` creates a repository-only URI
- The revision is appended with `@` separator
- Use this function when building URIs dynamically in queries
- The `git_tree()` function already provides a `git_uri` column - you don't need to construct it manually for tree results

# git_log

Query commit history from a git repository.

## Syntax

```sql
git_log()
git_log(repo_path)
git_log(git_uri)
```

## Parameters

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `repo_path` | VARCHAR | No | `.` (current directory) | Path to git repository |

## Returns

| Column | Type | Description |
|--------|------|-------------|
| `repo_path` | VARCHAR | Absolute path to the repository |
| `commit_hash` | VARCHAR | Full 40-character commit SHA |
| `author_name` | VARCHAR | Commit author name |
| `author_email` | VARCHAR | Commit author email |
| `committer_name` | VARCHAR | Committer name |
| `committer_email` | VARCHAR | Committer email |
| `author_date` | TIMESTAMP | When the commit was authored |
| `commit_date` | TIMESTAMP | When the commit was committed |
| `message` | VARCHAR | Full commit message |
| `parent_count` | INT32 | Number of parent commits |
| `tree_hash` | VARCHAR | Tree object hash |

## Examples

### Basic Usage

```sql
-- List recent commits (current directory)
SELECT commit_hash, author_name, message
FROM git_log()
LIMIT 10;
```

### With Repository Path

```sql
-- Query specific repository
SELECT * FROM git_log('/path/to/repo');

-- Query relative repository
SELECT * FROM git_log('../other-project');

-- Query git submodule
SELECT * FROM git_log('vendor/duckdb');
```

### Filter by Date

```sql
SELECT commit_hash, author_name, author_date, message
FROM git_log()
WHERE author_date > '2024-01-01'
ORDER BY author_date DESC;
```

### Filter by Author

```sql
SELECT commit_hash, message, author_date
FROM git_log()
WHERE author_name LIKE '%John%'
ORDER BY author_date DESC;
```

### Commit Statistics

```sql
SELECT
    author_name,
    COUNT(*) as commit_count,
    MIN(author_date) as first_commit,
    MAX(author_date) as last_commit
FROM git_log()
GROUP BY author_name
ORDER BY commit_count DESC;
```

### Commits per Month

```sql
SELECT
    date_trunc('month', author_date) as month,
    COUNT(*) as commits
FROM git_log()
GROUP BY month
ORDER BY month DESC;
```

### Find Merge Commits

```sql
SELECT commit_hash, message, author_date
FROM git_log()
WHERE parent_count > 1
ORDER BY author_date DESC;
```

## LATERAL Variant: `git_log_each()`

For use with LATERAL joins:

```sql
-- Query logs from multiple repositories
SELECT r.path, l.*
FROM (VALUES ('.'), ('../other')) as r(path),
     LATERAL git_log_each(r.path) l
LIMIT 20;
```

## Notes

- Commits are returned in reverse chronological order (newest first)
- The function walks the entire commit history by default - use `LIMIT` for large repositories
- Use `git_log('git://path@revision')` to start from a specific revision

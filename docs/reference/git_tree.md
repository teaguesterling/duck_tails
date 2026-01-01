# git_tree

List files and directories in a git commit tree.

## Syntax

```sql
git_tree(revision)
git_tree(repo_path, revision)
git_tree(git_uri)
```

## Parameters

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `repo_path` | VARCHAR | No | `.` (current directory) | Path to git repository |
| `revision` | VARCHAR | Yes | - | Git reference (commit, branch, tag) |

## Returns

| Column | Type | Description |
|--------|------|-------------|
| `git_uri` | VARCHAR | Complete git:// URI for the file |
| `repo_path` | VARCHAR | Absolute path to the repository |
| `commit_hash` | VARCHAR | Full commit hash |
| `tree_hash` | VARCHAR | Tree object hash |
| `file_path` | VARCHAR | Path within the repository |
| `file_ext` | VARCHAR | File extension (e.g., `.py`) |
| `ref` | VARCHAR | Git reference used |
| `blob_hash` | VARCHAR | Blob object hash |
| `commit_date` | TIMESTAMP | Commit timestamp |
| `mode` | INT32 | File mode (Unix permissions) |
| `size_bytes` | INT64 | File size in bytes |
| `kind` | VARCHAR | Entry type: `file` or `directory` |
| `is_text` | BOOLEAN | Whether the file is text |
| `encoding` | VARCHAR | Text encoding (utf8, binary) |

## Examples

### List All Files

```sql
-- List all files at HEAD
SELECT file_path, size_bytes, kind
FROM git_tree('HEAD')
WHERE kind = 'file'
ORDER BY file_path;
```

### Filter by Extension

```sql
-- List all Python files
SELECT file_path, size_bytes
FROM git_tree('HEAD')
WHERE file_ext = '.py'
ORDER BY size_bytes DESC;
```

### List Files at Tag

```sql
SELECT file_path, size_bytes
FROM git_tree('v1.0.0')
WHERE kind = 'file';
```

### List Files at Branch

```sql
SELECT file_path, size_bytes
FROM git_tree('feature-branch')
WHERE kind = 'file';
```

### With Repository Path

```sql
-- Different repository
SELECT file_path FROM git_tree('../other-repo', 'HEAD');

-- Absolute path
SELECT file_path FROM git_tree('/path/to/repo', 'main');
```

### Find Large Files

```sql
SELECT file_path, size_bytes
FROM git_tree('HEAD')
WHERE kind = 'file'
ORDER BY size_bytes DESC
LIMIT 20;
```

### Directory Structure

```sql
-- List top-level directories
SELECT DISTINCT
    split_part(file_path, '/', 1) as top_level,
    COUNT(*) as file_count
FROM git_tree('HEAD')
WHERE kind = 'file'
GROUP BY top_level
ORDER BY file_count DESC;
```

### Compare Trees Between Commits

```sql
-- Files added between v1.0 and v2.0
SELECT file_path
FROM git_tree('v2.0')
WHERE file_path NOT IN (
    SELECT file_path FROM git_tree('v1.0')
);
```

## LATERAL Variant: `git_tree_each()`

For use with LATERAL joins:

```sql
-- List files for each commit
SELECT
    l.commit_hash,
    l.message,
    t.file_path
FROM git_log() l,
     LATERAL git_tree_each('.', l.commit_hash) t
WHERE t.file_ext = '.md'
LIMIT 50;
```

### Read File Contents via LATERAL

```sql
-- Read all Python files
SELECT
    t.file_path,
    r.size_bytes,
    substr(r.text, 1, 100) as preview
FROM git_tree('HEAD') t,
     LATERAL git_read_each(t.git_uri) r
WHERE t.file_ext = '.py';
```

## Notes

- The `git_uri` column can be passed directly to `git_read_each()` or standard DuckDB file functions
- Directories are included in the output with `kind = 'directory'`
- Use `WHERE kind = 'file'` to filter to files only
- The `mode` column contains Unix-style permissions (e.g., 33188 = 0100644 = regular file)

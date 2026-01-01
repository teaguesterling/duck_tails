# git_read

Read file content from a git repository at a specific revision.

## Syntax

```sql
git_read(git_uri)
```

## Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `git_uri` | VARCHAR | Yes | Git URI in format `git://path@revision` |

## Returns

| Column | Type | Description |
|--------|------|-------------|
| `git_uri` | VARCHAR | Complete git:// URI for the file |
| `repo_path` | VARCHAR | Absolute path to the repository |
| `commit_hash` | VARCHAR | Full commit hash |
| `tree_hash` | VARCHAR | Tree object hash |
| `file_path` | VARCHAR | Path within the repository |
| `file_ext` | VARCHAR | File extension |
| `ref` | VARCHAR | Git reference used |
| `blob_hash` | VARCHAR | Blob object hash |
| `mode` | INT32 | File mode |
| `kind` | VARCHAR | Entry type (file) |
| `is_text` | BOOLEAN | Whether content is text |
| `encoding` | VARCHAR | Text encoding (utf8, binary) |
| `size_bytes` | INT64 | File size in bytes |
| `truncated` | BOOLEAN | Whether content was truncated |
| `text` | VARCHAR | File content (for text files) |
| `blob` | BLOB | File content (for binary files) |

## Examples

### Read Text File

```sql
-- Read README from HEAD
SELECT text FROM git_read('git://README.md@HEAD');

-- Get file metadata
SELECT
    file_path,
    size_bytes,
    is_text,
    encoding
FROM git_read('git://src/main.py@HEAD');
```

### Read from Different Revisions

```sql
-- From a branch
SELECT text FROM git_read('git://config.json@develop');

-- From a tag
SELECT text FROM git_read('git://VERSION@v1.0.0');

-- From a specific commit
SELECT text FROM git_read('git://file.txt@abc1234');

-- From relative commit
SELECT text FROM git_read('git://file.txt@HEAD~5');
```

### Read from Different Repositories

```sql
-- Sibling repository
SELECT text FROM git_read('git://../other-repo/README.md@HEAD');

-- Absolute path
SELECT text FROM git_read('git:///home/user/project/file.txt@main');
```

### Read Binary Files

```sql
-- Binary files are in the blob column
SELECT
    file_path,
    length(blob) as size,
    is_text
FROM git_read('git://image.png@HEAD');
```

### Preview File Content

```sql
SELECT
    file_path,
    size_bytes,
    substr(text, 1, 200) as preview
FROM git_read('git://README.md@HEAD');
```

### Check File Existence

```sql
-- This will error if file doesn't exist
SELECT COUNT(*) FROM git_read('git://maybe-exists.txt@HEAD');

-- Use in a subquery to check
SELECT EXISTS (
    SELECT 1 FROM git_read('git://README.md@HEAD')
) as file_exists;
```

## LATERAL Variant: `git_read_each()`

For use with LATERAL joins to read multiple files:

```sql
-- Read all markdown files
SELECT
    t.file_path,
    r.size_bytes,
    substr(r.text, 1, 100) as preview
FROM git_tree('HEAD') t,
     LATERAL git_read_each(t.git_uri) r
WHERE t.file_ext = '.md';
```

### Track File History

```sql
-- Read file at each commit
SELECT
    l.commit_hash,
    l.author_date,
    r.size_bytes
FROM git_log() l,
     LATERAL git_read_each(git_uri('.', 'package.json', l.commit_hash)) r
LIMIT 20;
```

## Using with DuckDB File Functions

The `git://` protocol works with standard DuckDB functions:

```sql
-- Read CSV
SELECT * FROM read_csv('git://data/sales.csv@HEAD');

-- Read JSON
SELECT * FROM read_json('git://config.json@main');

-- Read Parquet
SELECT * FROM read_parquet('git://data/metrics.parquet@v1.0');
```

## Notes

- Text files have content in the `text` column
- Binary files have content in the `blob` column
- The `truncated` column indicates if content was cut off (for very large files)
- Git LFS files are automatically detected and their real content is returned
- Use `git_read_each()` for efficient multi-file reads via LATERAL joins

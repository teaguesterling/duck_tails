# Reading Git Files

Duck Tails provides multiple ways to read files from git repositories.

## Using Standard DuckDB Functions

The `git://` protocol integrates with all DuckDB file-reading functions:

### CSV Files

```sql
-- Read CSV from HEAD
SELECT * FROM read_csv('git://data/sales.csv@HEAD');

-- With options
SELECT * FROM read_csv(
    'git://data/sales.csv@v1.0',
    header = true,
    delim = ','
);

-- Compare versions
SELECT 'current' as version, * FROM read_csv('git://data.csv@HEAD')
UNION ALL
SELECT 'previous' as version, * FROM read_csv('git://data.csv@HEAD~1');
```

### JSON Files

```sql
-- Read JSON file
SELECT * FROM read_json('git://config.json@main');

-- Read JSON array
SELECT * FROM read_json_auto('git://data.json@HEAD');
```

### Parquet Files

```sql
-- Read Parquet from a tagged release
SELECT * FROM read_parquet('git://analytics/data.parquet@v2.0');
```

### Text Files

```sql
-- Read as text
SELECT * FROM read_text('git://README.md@HEAD');
```

## Using `git_read()`

The `git_read()` function provides detailed metadata alongside file content:

```sql
SELECT * FROM git_read('git://README.md@HEAD');
```

### Output Columns

| Column | Type | Description |
|--------|------|-------------|
| `git_uri` | VARCHAR | Complete git URI for the file |
| `repo_path` | VARCHAR | Absolute path to repository |
| `commit_hash` | VARCHAR | Full commit hash |
| `tree_hash` | VARCHAR | Tree object hash |
| `file_path` | VARCHAR | Path within repository |
| `file_ext` | VARCHAR | File extension |
| `ref` | VARCHAR | Git reference used |
| `blob_hash` | VARCHAR | Blob object hash |
| `mode` | INT32 | File mode (permissions) |
| `kind` | VARCHAR | Entry type (file, directory) |
| `is_text` | BOOLEAN | Whether content is text |
| `encoding` | VARCHAR | Text encoding (utf8, binary) |
| `size_bytes` | INT64 | File size in bytes |
| `truncated` | BOOLEAN | Whether content was truncated |
| `text` | VARCHAR | File content (text files) |
| `blob` | BLOB | File content (binary files) |

### Examples

```sql
-- Get file metadata
SELECT
    file_path,
    size_bytes,
    is_text,
    encoding
FROM git_read('git://src/main.py@HEAD');

-- Read text content
SELECT text
FROM git_read('git://README.md@HEAD');

-- Read with size limit
SELECT *
FROM git_read('git://large-file.txt@HEAD')
WHERE size_bytes < 1000000;  -- Skip files over 1MB
```

## Using `git_read_each()` with LATERAL Joins

For reading multiple files efficiently, use `git_read_each()` with LATERAL joins:

```sql
-- Read all Python files at HEAD
SELECT
    t.file_path,
    r.size_bytes,
    substr(r.text, 1, 100) as preview
FROM git_tree('HEAD') t,
     LATERAL git_read_each(t.git_uri) r
WHERE t.file_ext = '.py';
```

See [LATERAL Joins](lateral-joins.md) for more details.

## Reading from Different Sources

### Current Repository

```sql
-- Using default (current directory)
SELECT * FROM git_read('git://file.txt@HEAD');

-- Explicit current directory
SELECT * FROM git_read('git://./file.txt@HEAD');
```

### Relative Repositories

```sql
-- Sibling repository
SELECT * FROM git_read('git://../other-repo/file.txt@HEAD');

-- Nested repository (submodule)
SELECT * FROM git_read('git://vendor/lib/src/main.cpp@HEAD');
```

### Absolute Paths

```sql
SELECT * FROM git_read('git:///home/user/project/README.md@HEAD');
```

## Reading Historical Files

### By Commit

```sql
-- Specific commit
SELECT * FROM git_read('git://file.txt@abc1234');

-- Relative to HEAD
SELECT * FROM git_read('git://file.txt@HEAD~5');
```

### By Branch

```sql
SELECT * FROM git_read('git://config.json@feature-branch');
```

### By Tag

```sql
SELECT * FROM git_read('git://data.csv@v1.0.0');
```

### Deleted Files

Files that no longer exist in the working tree can still be read from history:

```sql
-- Read a file that was deleted
SELECT * FROM git_read('git://old-file.txt@HEAD~10');
```

## Binary Files

Binary files are returned in the `blob` column:

```sql
SELECT
    file_path,
    is_text,
    length(blob) as blob_size
FROM git_read('git://image.png@HEAD');
```

## Error Handling

### File Not Found

```sql
-- Throws error if file doesn't exist at revision
SELECT * FROM git_read('git://nonexistent.txt@HEAD');
-- Error: File 'nonexistent.txt' not found in tree
```

### Invalid Revision

```sql
-- Throws error for invalid revision
SELECT * FROM git_read('git://file.txt@invalid-ref');
-- Error: Failed to resolve revision 'invalid-ref'
```

### Repository Not Found

```sql
-- Throws error if no git repository found
SELECT * FROM git_read('git:///tmp/not-a-repo/file.txt@HEAD');
-- Error: No git repository found for path '/tmp/not-a-repo'
```

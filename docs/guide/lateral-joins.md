# LATERAL Joins

LATERAL joins are a powerful SQL feature that allows you to reference columns from preceding tables in a subquery. Duck Tails provides `_each` variants of all table functions specifically designed for LATERAL join usage.

## Understanding LATERAL Joins

In a regular join, the right side cannot reference columns from the left side. LATERAL changes this:

```sql
-- Regular join (doesn't work - can't reference t.git_uri)
SELECT * FROM git_tree('HEAD') t
JOIN git_read(t.git_uri) r ON TRUE;  -- ERROR!

-- LATERAL join (works!)
SELECT * FROM git_tree('HEAD') t,
     LATERAL git_read_each(t.git_uri) r;
```

## The `_each` Function Pattern

Duck Tails provides paired functions:

| Standard Function | LATERAL Variant | Purpose |
|-------------------|-----------------|---------|
| `git_log()` | `git_log_each()` | Query commit history |
| `git_tree()` | `git_tree_each()` | List tree entries |
| `git_read()` | `git_read_each()` | Read file content |
| `git_branches()` | `git_branches_each()` | List branches |
| `git_tags()` | `git_tags_each()` | List tags |
| `git_parents()` | `git_parents_each()` | List parent commits |

!!! note
    The `_each` variants are LATERAL-only and will error if called directly without a LATERAL context.

## Common Patterns

### Read All Files at a Commit

```sql
SELECT
    t.file_path,
    r.size_bytes,
    r.is_text
FROM git_tree('HEAD') t,
     LATERAL git_read_each(t.git_uri) r
WHERE t.kind = 'file';
```

### Track File Changes Across Commits

```sql
SELECT
    l.commit_hash,
    l.author_date,
    substr(l.message, 1, 50) as message,
    r.size_bytes
FROM git_log() l,
     LATERAL git_read_each(
         git_uri('.', 'README.md', l.commit_hash)
     ) r
WHERE l.author_date > '2024-01-01'
LIMIT 20;
```

### Find Files Changed in Each Commit

```sql
SELECT
    l.commit_hash,
    l.message,
    t.file_path
FROM git_log() l,
     LATERAL git_tree_each('.', l.commit_hash) t
WHERE t.file_ext = '.py'
LIMIT 50;
```

### Cross-Repository Queries

```sql
-- Compare file across repositories
SELECT
    b.branch_name,
    r.text
FROM git_branches() b,
     LATERAL git_read_each(
         git_uri('.', 'VERSION', b.commit_hash)
     ) r
WHERE b.is_remote = false;
```

## Syntax Variations

### Comma Syntax (Implicit LATERAL)

```sql
SELECT * FROM git_tree('HEAD') t,
     LATERAL git_read_each(t.git_uri) r;
```

### Explicit CROSS JOIN LATERAL

```sql
SELECT * FROM git_tree('HEAD') t
CROSS JOIN LATERAL git_read_each(t.git_uri) r;
```

### With ON Clause

```sql
SELECT * FROM git_tree('HEAD') t
JOIN LATERAL git_read_each(t.git_uri) r ON TRUE;
```

## Performance Considerations

### Limit Early

Apply filters and limits before LATERAL joins when possible:

```sql
-- Good: Filter before LATERAL
SELECT * FROM (
    SELECT * FROM git_log()
    WHERE author_date > '2024-01-01'
    LIMIT 100
) l,
LATERAL git_tree_each('.', l.commit_hash) t;

-- Less efficient: All commits processed
SELECT * FROM git_log() l,
     LATERAL git_tree_each('.', l.commit_hash) t
WHERE l.author_date > '2024-01-01'
LIMIT 100;
```

### Use Specific Paths

```sql
-- Good: Read specific file
SELECT * FROM git_log() l,
     LATERAL git_read_each(git_uri('.', 'config.json', l.commit_hash)) r
LIMIT 10;

-- Less efficient: Read all files, filter later
SELECT * FROM git_log() l,
     LATERAL git_tree_each('.', l.commit_hash) t,
     LATERAL git_read_each(t.git_uri) r
WHERE t.file_path = 'config.json'
LIMIT 10;
```

## Advanced Examples

### Find Commits That Modified a Specific File

```sql
WITH file_versions AS (
    SELECT
        l.commit_hash,
        l.author_date,
        l.message,
        r.blob_hash,
        LAG(r.blob_hash) OVER (ORDER BY l.author_date DESC) as prev_blob
    FROM git_log() l,
         LATERAL git_read_each(git_uri('.', 'src/main.py', l.commit_hash)) r
)
SELECT commit_hash, author_date, message
FROM file_versions
WHERE blob_hash != prev_blob OR prev_blob IS NULL
ORDER BY author_date DESC;
```

### Compare File Sizes Across Branches

```sql
SELECT
    b.branch_name,
    SUM(r.size_bytes) as total_size,
    COUNT(*) as file_count
FROM git_branches() b,
     LATERAL git_tree_each('.', b.commit_hash) t,
     LATERAL git_read_each(t.git_uri) r
WHERE t.kind = 'file'
  AND b.is_remote = false
GROUP BY b.branch_name
ORDER BY total_size DESC;
```

### Build Change Log

```sql
SELECT
    l.commit_hash,
    l.author_name,
    l.author_date,
    l.message,
    array_agg(t.file_path) as changed_files
FROM git_log() l,
     LATERAL git_tree_each('.', l.commit_hash) t
WHERE l.author_date > '2024-01-01'
GROUP BY l.commit_hash, l.author_name, l.author_date, l.message
ORDER BY l.author_date DESC
LIMIT 20;
```

## Error Handling

When a LATERAL function fails for one row, it affects only that row:

```sql
-- If README.md doesn't exist in some commits, those rows are skipped
SELECT
    l.commit_hash,
    r.size_bytes
FROM git_log() l,
     LATERAL git_read_each(git_uri('.', 'README.md', l.commit_hash)) r
LIMIT 10;
```

To handle missing files gracefully:

```sql
-- Use LEFT JOIN LATERAL to include rows even when file doesn't exist
SELECT
    l.commit_hash,
    COALESCE(r.size_bytes, 0) as size
FROM git_log() l
LEFT JOIN LATERAL git_read_each(git_uri('.', 'README.md', l.commit_hash)) r ON TRUE
LIMIT 10;
```

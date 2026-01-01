# git_tags

List tags in a git repository.

## Syntax

```sql
git_tags()
git_tags(repo_path)
```

## Parameters

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `repo_path` | VARCHAR | No | `.` (current directory) | Path to git repository |

## Returns

| Column | Type | Description |
|--------|------|-------------|
| `repo_path` | VARCHAR | Absolute path to the repository |
| `tag_name` | VARCHAR | Tag name |
| `commit_hash` | VARCHAR | Commit hash the tag points to |
| `tag_hash` | VARCHAR | Tag object hash (for annotated tags) |
| `tagger_name` | VARCHAR | Name of the tagger (annotated tags) |
| `tagger_date` | TIMESTAMP | When the tag was created |
| `message` | VARCHAR | Tag message (annotated tags) |
| `is_annotated` | BOOLEAN | Whether this is an annotated tag |

## Examples

### List All Tags

```sql
SELECT tag_name, commit_hash, is_annotated
FROM git_tags()
ORDER BY tag_name;
```

### List Annotated Tags Only

```sql
SELECT
    tag_name,
    tagger_name,
    tagger_date,
    message
FROM git_tags()
WHERE is_annotated = true
ORDER BY tagger_date DESC;
```

### Find Tags by Pattern

```sql
-- Semantic version tags
SELECT tag_name, commit_hash
FROM git_tags()
WHERE tag_name LIKE 'v%'
ORDER BY tag_name DESC;
```

### Query Different Repository

```sql
SELECT * FROM git_tags('../other-repo');
SELECT * FROM git_tags('/path/to/repo');
```

### Compare Tagged Versions

```sql
-- Compare file counts between releases
SELECT
    t.tag_name,
    COUNT(*) as file_count
FROM git_tags() t,
     LATERAL git_tree_each('.', t.commit_hash) tr
WHERE t.tag_name LIKE 'v%'
  AND tr.kind = 'file'
GROUP BY t.tag_name
ORDER BY t.tag_name;
```

### Read File at Each Tag

```sql
SELECT
    t.tag_name,
    r.text as version_content
FROM git_tags() t,
     LATERAL git_read_each(git_uri('.', 'VERSION', t.commit_hash)) r
WHERE t.tag_name LIKE 'v%'
ORDER BY t.tag_name DESC;
```

## LATERAL Variant: `git_tags_each()`

For use with LATERAL joins:

```sql
-- List tags from multiple repositories
SELECT r.path, t.tag_name, t.commit_hash
FROM (VALUES ('.'), ('../lib')) as r(path),
     LATERAL git_tags_each(r.path) t;
```

## Notes

- **Lightweight tags** have `is_annotated = false` and empty tagger/message fields
- **Annotated tags** include tagger information and a message
- The `commit_hash` points to the commit the tag references
- The `tag_hash` is the tag object's own hash (same as commit_hash for lightweight tags)
- Tags are not sorted in any particular order - use `ORDER BY` as needed

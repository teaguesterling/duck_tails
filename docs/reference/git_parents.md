# git_parents

Get parent commits for a given commit.

## Syntax

```sql
git_parents(revision)
git_parents(repo_path, revision)
git_parents(git_uri)
```

## Parameters

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `repo_path` | VARCHAR | No | `.` (current directory) | Path to git repository |
| `revision` | VARCHAR | Yes | - | Git reference (commit, branch, tag) |

## Returns

| Column | Type | Description |
|--------|------|-------------|
| `repo_path` | VARCHAR | Absolute path to the repository |
| `commit_hash` | VARCHAR | The child commit hash |
| `parent_hash` | VARCHAR | Parent commit hash |
| `parent_index` | INT32 | Parent index (0-based) |

## Examples

### Get Parents of HEAD

```sql
SELECT parent_hash, parent_index
FROM git_parents('HEAD');
```

### Get Parents of a Specific Commit

```sql
SELECT parent_hash
FROM git_parents('abc1234');
```

### With Repository Path

```sql
SELECT * FROM git_parents('../other-repo', 'HEAD');
SELECT * FROM git_parents('/path/to/repo', 'main');
```

### Find Merge Commits

```sql
-- Commits with multiple parents are merges
SELECT
    l.commit_hash,
    l.message,
    l.parent_count
FROM git_log() l
WHERE l.parent_count > 1
LIMIT 10;
```

### Trace First-Parent History

```sql
-- Follow first parent only (linear history)
WITH RECURSIVE history AS (
    SELECT commit_hash, 0 as depth
    FROM git_log() LIMIT 1

    UNION ALL

    SELECT p.parent_hash, h.depth + 1
    FROM history h,
         LATERAL git_parents_each('.', h.commit_hash) p
    WHERE p.parent_index = 0
      AND h.depth < 10
)
SELECT * FROM history;
```

### Analyze Merge Patterns

```sql
-- Get second parents (merge sources)
SELECT
    l.commit_hash,
    l.message,
    p.parent_hash as merged_from
FROM git_log() l,
     LATERAL git_parents_each('.', l.commit_hash) p
WHERE p.parent_index = 1  -- Second parent
LIMIT 20;
```

## LATERAL Variant: `git_parents_each()`

For use with LATERAL joins:

```sql
-- Get parents for each recent commit
SELECT
    l.commit_hash,
    l.message,
    p.parent_hash,
    p.parent_index
FROM git_log() l,
     LATERAL git_parents_each('.', l.commit_hash) p
LIMIT 50;
```

## Notes

- Regular commits have 1 parent (index 0)
- Merge commits have 2+ parents (indices 0, 1, ...)
- The initial/root commit has 0 parents
- `parent_index = 0` is always the "first parent" (the branch you were on)
- `parent_index = 1` is the "second parent" (the branch being merged in)

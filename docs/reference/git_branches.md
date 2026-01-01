# git_branches

List branches in a git repository.

## Syntax

```sql
git_branches()
git_branches(repo_path)
```

## Parameters

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `repo_path` | VARCHAR | No | `.` (current directory) | Path to git repository |

## Returns

| Column | Type | Description |
|--------|------|-------------|
| `repo_path` | VARCHAR | Absolute path to the repository |
| `branch_name` | VARCHAR | Full branch name |
| `commit_hash` | VARCHAR | Commit hash the branch points to |
| `is_current` | BOOLEAN | Whether this is the checked-out branch |
| `is_remote` | BOOLEAN | Whether this is a remote-tracking branch |

## Examples

### List All Branches

```sql
SELECT branch_name, commit_hash, is_current
FROM git_branches();
```

### List Local Branches Only

```sql
SELECT branch_name, commit_hash
FROM git_branches()
WHERE is_remote = false;
```

### List Remote Branches Only

```sql
SELECT branch_name, commit_hash
FROM git_branches()
WHERE is_remote = true;
```

### Find Current Branch

```sql
SELECT branch_name
FROM git_branches()
WHERE is_current = true;
```

### Query Different Repository

```sql
-- Relative path
SELECT * FROM git_branches('../other-repo');

-- Absolute path
SELECT * FROM git_branches('/path/to/repo');
```

### Compare Branches

```sql
-- Count commits per branch
SELECT
    b.branch_name,
    COUNT(l.commit_hash) as commit_count
FROM git_branches() b,
     LATERAL git_log_each(git_uri('.', '', b.commit_hash)) l
WHERE b.is_remote = false
GROUP BY b.branch_name
ORDER BY commit_count DESC;
```

### Find Branches with Specific File

```sql
SELECT DISTINCT b.branch_name
FROM git_branches() b,
     LATERAL git_tree_each('.', b.commit_hash) t
WHERE t.file_path = 'experimental-feature.py'
  AND b.is_remote = false;
```

## LATERAL Variant: `git_branches_each()`

For use with LATERAL joins:

```sql
-- List branches from multiple repositories
SELECT r.path, b.branch_name
FROM (VALUES ('.'), ('../lib')) as r(path),
     LATERAL git_branches_each(r.path) b
WHERE b.is_remote = false;
```

## Notes

- Remote branches have names like `origin/main`, `origin/feature`
- The `commit_hash` for remote branches may be empty if not fetched
- Use `is_current = true` to find the currently checked-out branch
- Branch names include the full path (e.g., `feature/new-feature`)

# Advanced Queries

Complex queries demonstrating Duck Tails capabilities.

## Commit Analysis

### Find Commits That Changed Specific Files

```sql
WITH file_changes AS (
    SELECT
        l.commit_hash,
        l.author_date,
        l.message,
        r.blob_hash,
        LAG(r.blob_hash) OVER (ORDER BY l.author_date DESC) as prev_blob
    FROM git_log() l,
         LATERAL git_read_each(git_uri('.', 'package.json', l.commit_hash)) r
)
SELECT
    commit_hash,
    author_date,
    message
FROM file_changes
WHERE blob_hash != prev_blob OR prev_blob IS NULL
ORDER BY author_date DESC
LIMIT 20;
```

### Find Large Commits

```sql
SELECT
    l.commit_hash,
    l.author_name,
    l.message,
    COUNT(*) as files_changed,
    SUM(length(r.text)) as total_chars
FROM git_log() l,
     LATERAL git_tree_each('.', l.commit_hash) t,
     LATERAL git_read_each(t.git_uri) r
WHERE t.kind = 'file'
  AND l.author_date > current_date - interval '30 days'
GROUP BY l.commit_hash, l.author_name, l.message
HAVING COUNT(*) > 10
ORDER BY files_changed DESC
LIMIT 10;
```

## Code Pattern Detection

### Find TODO Comments

```sql
SELECT
    t.file_path,
    r.text
FROM git_tree('HEAD') t,
     LATERAL git_read_each(t.git_uri) r
WHERE t.file_ext IN ('.py', '.js', '.ts')
  AND r.text LIKE '%TODO%';
```

### Count Lines of Code by Language

```sql
SELECT
    CASE file_ext
        WHEN '.py' THEN 'Python'
        WHEN '.js' THEN 'JavaScript'
        WHEN '.ts' THEN 'TypeScript'
        WHEN '.sql' THEN 'SQL'
        WHEN '.cpp' THEN 'C++'
        WHEN '.c' THEN 'C'
        ELSE 'Other'
    END as language,
    COUNT(*) as file_count,
    SUM(length(r.text) - length(replace(r.text, chr(10), ''))) as line_count
FROM git_tree('HEAD') t,
     LATERAL git_read_each(t.git_uri) r
WHERE t.kind = 'file'
  AND t.file_ext IN ('.py', '.js', '.ts', '.sql', '.cpp', '.c')
GROUP BY language
ORDER BY line_count DESC;
```

## Dependency Analysis

### Track package.json Changes

```sql
SELECT
    l.commit_hash,
    l.author_date,
    l.message,
    json_extract_string(r.text, '$.version') as version
FROM git_log() l,
     LATERAL git_read_each(git_uri('.', 'package.json', l.commit_hash)) r
ORDER BY l.author_date DESC
LIMIT 50;
```

### Compare Dependencies Between Versions

```sql
WITH v1_deps AS (
    SELECT json_extract_string(r.text, '$.dependencies') as deps
    FROM git_read('git://package.json@v1.0') r
),
v2_deps AS (
    SELECT json_extract_string(r.text, '$.dependencies') as deps
    FROM git_read('git://package.json@v2.0') r
)
SELECT
    v1_deps.deps as v1_dependencies,
    v2_deps.deps as v2_dependencies
FROM v1_deps, v2_deps;
```

## Multi-Repository Queries

### Find File Across Repositories

```sql
SELECT
    l.repo_path,
    t.file_path,
    r.size_bytes
FROM (
    SELECT * FROM git_log('.') LIMIT 1
    UNION ALL
    SELECT * FROM git_log('../other-repo') LIMIT 1
) l,
LATERAL git_tree_each(l.repo_path, l.commit_hash) t,
LATERAL git_read_each(t.git_uri) r
WHERE t.file_path LIKE '%config%'
  AND t.kind = 'file';
```

### Aggregate Statistics Across Projects

```sql
SELECT
    repo_path,
    COUNT(DISTINCT author_name) as contributors,
    COUNT(*) as total_commits,
    MIN(author_date) as first_commit,
    MAX(author_date) as last_commit
FROM (
    SELECT * FROM git_log('.')
    UNION ALL
    SELECT * FROM git_log('../project-a')
    UNION ALL
    SELECT * FROM git_log('../project-b')
)
GROUP BY repo_path
ORDER BY total_commits DESC;
```

## Recursive Queries

### Trace Commit Ancestry

```sql
WITH RECURSIVE ancestry AS (
    -- Start with HEAD
    SELECT
        commit_hash,
        0 as depth,
        commit_hash as path
    FROM git_log()
    LIMIT 1

    UNION ALL

    -- Get parents
    SELECT
        p.parent_hash,
        a.depth + 1,
        a.path || ' -> ' || p.parent_hash
    FROM ancestry a,
         LATERAL git_parents_each('.', a.commit_hash) p
    WHERE a.depth < 5  -- Limit depth
)
SELECT * FROM ancestry
ORDER BY depth, commit_hash;
```

### Find All Branches Containing a Commit

```sql
WITH target_commit AS (
    SELECT 'abc1234' as commit_hash  -- Replace with actual hash
),
branch_ancestry AS (
    SELECT
        b.branch_name,
        l.commit_hash
    FROM git_branches() b,
         LATERAL git_log_each(git_uri('.', '', b.commit_hash)) l
    WHERE b.is_remote = false
)
SELECT DISTINCT branch_name
FROM branch_ancestry, target_commit
WHERE branch_ancestry.commit_hash = target_commit.commit_hash;
```

## Export and Reporting

### Export Commit Log to CSV

```sql
COPY (
    SELECT
        commit_hash,
        author_name,
        author_date,
        message
    FROM git_log()
    WHERE author_date > '2024-01-01'
) TO 'commits-2024.csv' WITH (HEADER);
```

### Generate Change Report

```sql
SELECT
    date_trunc('week', author_date) as week,
    author_name,
    COUNT(*) as commits,
    string_agg(DISTINCT substr(message, 1, 50), '; ') as messages
FROM git_log()
WHERE author_date > current_date - interval '30 days'
GROUP BY week, author_name
ORDER BY week DESC, commits DESC;
```

## Performance Patterns

### Use CTEs for Repeated Data

```sql
-- More efficient: fetch tree once
WITH tree AS (
    SELECT * FROM git_tree('HEAD')
    WHERE kind = 'file'
)
SELECT
    file_ext,
    COUNT(*) as count,
    SUM(size_bytes) as total_size
FROM tree
GROUP BY file_ext

UNION ALL

SELECT
    'TOTAL' as file_ext,
    COUNT(*) as count,
    SUM(size_bytes) as total_size
FROM tree;
```

### Limit Before LATERAL

```sql
-- Efficient: limit commits before reading files
SELECT l.commit_hash, r.size_bytes
FROM (
    SELECT * FROM git_log()
    WHERE author_date > '2024-01-01'
    LIMIT 100
) l,
LATERAL git_read_each(git_uri('.', 'README.md', l.commit_hash)) r;
```

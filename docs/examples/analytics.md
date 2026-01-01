# Repository Analytics

Analyze your git repository with SQL.

## Contributor Statistics

### Most Active Contributors

```sql
SELECT
    author_name,
    COUNT(*) as commit_count,
    MIN(author_date) as first_commit,
    MAX(author_date) as last_commit,
    MAX(author_date) - MIN(author_date) as active_period
FROM git_log()
GROUP BY author_name
ORDER BY commit_count DESC
LIMIT 20;
```

### Commits by Day of Week

```sql
SELECT
    dayname(author_date) as day_of_week,
    COUNT(*) as commits
FROM git_log()
GROUP BY dayname(author_date), dayofweek(author_date)
ORDER BY dayofweek(author_date);
```

### Commits by Hour

```sql
SELECT
    hour(author_date) as hour,
    COUNT(*) as commits
FROM git_log()
GROUP BY hour(author_date)
ORDER BY hour;
```

### Monthly Activity

```sql
SELECT
    date_trunc('month', author_date) as month,
    COUNT(*) as commits,
    COUNT(DISTINCT author_name) as contributors
FROM git_log()
GROUP BY month
ORDER BY month DESC
LIMIT 24;
```

## Code Statistics

### Files by Extension

```sql
SELECT
    file_ext,
    COUNT(*) as file_count,
    SUM(size_bytes) as total_bytes,
    AVG(size_bytes)::INT as avg_size
FROM git_tree('HEAD')
WHERE kind = 'file'
GROUP BY file_ext
ORDER BY total_bytes DESC;
```

### Largest Files

```sql
SELECT
    file_path,
    size_bytes,
    size_bytes / 1024.0 as size_kb
FROM git_tree('HEAD')
WHERE kind = 'file'
ORDER BY size_bytes DESC
LIMIT 20;
```

### Directory Sizes

```sql
SELECT
    split_part(file_path, '/', 1) as directory,
    COUNT(*) as file_count,
    SUM(size_bytes) as total_bytes
FROM git_tree('HEAD')
WHERE kind = 'file'
GROUP BY directory
ORDER BY total_bytes DESC;
```

## Branch Analysis

### Branch Overview

```sql
SELECT
    branch_name,
    is_current,
    is_remote,
    commit_hash
FROM git_branches()
ORDER BY is_current DESC, is_remote, branch_name;
```

### Commits per Branch

```sql
SELECT
    b.branch_name,
    COUNT(*) as commit_count
FROM git_branches() b,
     LATERAL git_log_each(git_uri('.', '', b.commit_hash)) l
WHERE b.is_remote = false
GROUP BY b.branch_name
ORDER BY commit_count DESC;
```

## Tag Analysis

### Release Timeline

```sql
SELECT
    tag_name,
    tagger_date,
    message
FROM git_tags()
WHERE is_annotated = true
ORDER BY tagger_date DESC;
```

### Files Changed Between Releases

```sql
WITH v1_files AS (
    SELECT file_path FROM git_tree('v1.0.0') WHERE kind = 'file'
),
v2_files AS (
    SELECT file_path FROM git_tree('v2.0.0') WHERE kind = 'file'
)
SELECT
    (SELECT COUNT(*) FROM v2_files WHERE file_path NOT IN (SELECT * FROM v1_files)) as files_added,
    (SELECT COUNT(*) FROM v1_files WHERE file_path NOT IN (SELECT * FROM v2_files)) as files_removed,
    (SELECT COUNT(*) FROM v1_files WHERE file_path IN (SELECT * FROM v2_files)) as files_unchanged;
```

## Cross-Repository Analysis

### Compare Multiple Repositories

```sql
SELECT
    repo_path,
    COUNT(*) as commit_count,
    COUNT(DISTINCT author_name) as contributors
FROM (
    SELECT * FROM git_log('.')
    UNION ALL
    SELECT * FROM git_log('../other-project')
)
GROUP BY repo_path;
```

### Find Common Contributors

```sql
WITH repo1 AS (
    SELECT DISTINCT author_name FROM git_log('.')
),
repo2 AS (
    SELECT DISTINCT author_name FROM git_log('../other-project')
)
SELECT author_name
FROM repo1
WHERE author_name IN (SELECT author_name FROM repo2);
```

## Team Insights

### Bus Factor Analysis

```sql
-- Who knows each file (by commit count)
WITH file_authors AS (
    SELECT
        t.file_path,
        l.author_name,
        COUNT(*) as commits
    FROM git_log() l,
         LATERAL git_tree_each('.', l.commit_hash) t
    WHERE t.kind = 'file'
    GROUP BY t.file_path, l.author_name
)
SELECT
    file_path,
    COUNT(DISTINCT author_name) as contributor_count,
    MAX(author_name) FILTER (WHERE commits = (
        SELECT MAX(commits) FROM file_authors f2
        WHERE f2.file_path = file_authors.file_path
    )) as primary_author
FROM file_authors
GROUP BY file_path
HAVING contributor_count = 1  -- Files with only one contributor
ORDER BY file_path
LIMIT 20;
```

### Recent Activity by Author

```sql
SELECT
    author_name,
    COUNT(*) as commits_last_30_days
FROM git_log()
WHERE author_date > current_date - interval '30 days'
GROUP BY author_name
ORDER BY commits_last_30_days DESC;
```

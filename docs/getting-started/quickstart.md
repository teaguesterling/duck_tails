# Quick Start

This guide will get you up and running with Duck Tails in minutes.

## Loading the Extension

```sql
LOAD 'duck_tails';
```

## Your First Queries

### View Commit History

```sql
-- List recent commits (defaults to current directory)
SELECT
    commit_hash,
    author_name,
    substr(message, 1, 50) as message,
    author_date
FROM git_log()
LIMIT 10;
```

### Browse Repository Files

```sql
-- List all files at HEAD
SELECT
    file_path,
    size_bytes,
    kind
FROM git_tree('HEAD')
WHERE kind = 'file'
ORDER BY file_path;
```

### Read a File from Git

```sql
-- Read file content from a specific revision
SELECT * FROM git_read('git://README.md@HEAD');

-- Read and parse a CSV from git history
SELECT * FROM read_csv('git://data/sales.csv@HEAD');

-- Read from a specific commit
SELECT * FROM read_csv('git://data/sales.csv@abc1234');
```

### List Branches and Tags

```sql
-- List all branches
SELECT branch_name, commit_hash, is_current
FROM git_branches();

-- List all tags
SELECT tag_name, commit_hash
FROM git_tags();
```

## Working with Different Repositories

### Current Directory (Default)

```sql
-- All functions default to the current directory
SELECT * FROM git_log();
SELECT * FROM git_tree('HEAD');
```

### Specify a Repository Path

```sql
-- Absolute path
SELECT * FROM git_log('/path/to/repo');

-- Relative path
SELECT * FROM git_log('../other-project');

-- Git submodules
SELECT * FROM git_log('vendor/duckdb');
```

### Using Git URIs

```sql
-- Full git:// URI syntax
SELECT * FROM git_read('git:///path/to/repo/file.txt@HEAD');

-- Relative repository with file
SELECT * FROM git_read('git://./src/main.py@main');
```

## Comparing Versions

### Read Files at Different Revisions

```sql
-- Current version
SELECT * FROM read_csv('git://data.csv@HEAD');

-- Previous version
SELECT * FROM read_csv('git://data.csv@HEAD~1');

-- Tagged version
SELECT * FROM read_csv('git://data.csv@v1.0.0');

-- Branch version
SELECT * FROM read_csv('git://data.csv@feature-branch');
```

### Text Diff

```sql
-- Compare two versions of a file
SELECT * FROM read_git_diff(
    'git://config.json@v1.0',
    'git://config.json@v2.0'
);
```

## Next Steps

- Learn about [Git URI Syntax](../guide/git-uris.md) for advanced file access
- Explore [LATERAL Joins](../guide/lateral-joins.md) for powerful cross-commit queries
- See the [Function Reference](../reference/index.md) for complete API documentation
- Check out [Examples](../examples/analytics.md) for real-world use cases

# Duck Tails

**Git-aware data analysis for DuckDB**

Duck Tails is a DuckDB extension that brings git repository intelligence to your database. Query your git history, access files at any revision, and perform version-aware data analysis - all with SQL.

## Features

### Git Filesystem Access

Read any file from your git repository at any commit, branch, or tag using the `git://` protocol:

```sql
-- Read a CSV file from HEAD
SELECT * FROM read_csv('git://data/sales.csv@HEAD');

-- Compare data between commits
SELECT * FROM read_csv('git://data/sales.csv@HEAD~1');

-- Access files from a specific branch or tag
SELECT * FROM read_csv('git://config.json@feature-branch');
SELECT * FROM read_csv('git://metrics.csv@v1.0.0');
```

### Repository Metadata Queries

Query your git repository metadata directly with SQL:

```sql
-- View commit history
SELECT commit_hash, author_name, message, author_date
FROM git_log();

-- List all branches
SELECT branch_name, commit_hash, is_current
FROM git_branches();

-- Browse repository files at any revision
SELECT file_path, size_bytes, kind
FROM git_tree('HEAD');
```

### Version-Aware Analysis

Perform sophisticated version comparisons and historical analysis:

```sql
-- Compare record counts across versions
WITH current AS (SELECT COUNT(*) as cnt FROM read_csv('git://data.csv@HEAD')),
     previous AS (SELECT COUNT(*) as cnt FROM read_csv('git://data.csv@HEAD~1'))
SELECT current.cnt - previous.cnt AS records_added
FROM current, previous;
```

### Text Diff Analysis

Built-in text diffing with multiple output formats:

```sql
-- Compare two files
SELECT * FROM read_git_diff('git://file.txt@v1.0', 'git://file.txt@v2.0');

-- Get diff statistics
SELECT * FROM text_diff_stats('old content', 'new content');
```

## Quick Start

```sql
-- Load the extension
LOAD 'duck_tails';

-- Query git history (defaults to current directory)
SELECT * FROM git_log() LIMIT 5;

-- Read version-controlled data
SELECT * FROM read_csv('git://data/sales.csv@HEAD');
```

## Platform Support

Duck Tails supports:

- **Linux** (x86_64, aarch64)
- **macOS** (x86_64, Apple Silicon)
- **Windows** (coming soon)

## Getting Help

- [GitHub Issues](https://github.com/teaguesterling/duck_tails/issues) - Bug reports and feature requests
- [GitHub Discussions](https://github.com/teaguesterling/duck_tails/discussions) - Questions and community support

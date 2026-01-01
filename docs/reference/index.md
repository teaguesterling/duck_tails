# Function Reference

Duck Tails provides table functions for querying git repositories and scalar functions for text operations.

## Table Functions

### Repository Metadata

| Function | Description |
|----------|-------------|
| [`git_log()`](git_log.md) | Query commit history |
| [`git_branches()`](git_branches.md) | List repository branches |
| [`git_tags()`](git_tags.md) | List repository tags |
| [`git_parents()`](git_parents.md) | Get parent commits |

### File Access

| Function | Description |
|----------|-------------|
| [`git_tree()`](git_tree.md) | List files in a commit tree |
| [`git_read()`](git_read.md) | Read file content from git |

### Diff Operations

| Function | Description |
|----------|-------------|
| [`read_git_diff()`](diff.md#read_git_diff) | Compare two files |
| [`text_diff()`](diff.md#text_diff) | Compute text diff |
| [`text_diff_lines()`](diff.md#text_diff_lines) | Parse diff into lines |
| [`text_diff_stats()`](diff.md#text_diff_stats) | Get diff statistics |
| [`diff_text()`](diff.md#diff_text) | Alias for `text_diff()` |

## Scalar Functions

| Function | Description |
|----------|-------------|
| [`git_uri()`](git_uri.md) | Construct git URIs |

## LATERAL Variants

All table functions have `_each` variants designed for [LATERAL joins](../guide/lateral-joins.md):

| Standard | LATERAL Variant |
|----------|-----------------|
| `git_log()` | `git_log_each()` |
| `git_tree()` | `git_tree_each()` |
| `git_read()` | `git_read_each()` |
| `git_branches()` | `git_branches_each()` |
| `git_tags()` | `git_tags_each()` |
| `git_parents()` | `git_parents_each()` |

## Common Columns

Most functions return these standard columns:

| Column | Type | Description |
|--------|------|-------------|
| `repo_path` | VARCHAR | Absolute path to the repository |
| `commit_hash` | VARCHAR | Full 40-character commit SHA |

File-related functions also include:

| Column | Type | Description |
|--------|------|-------------|
| `git_uri` | VARCHAR | Complete git:// URI for the item |
| `file_path` | VARCHAR | Path within the repository |
| `file_ext` | VARCHAR | File extension (e.g., `.py`) |

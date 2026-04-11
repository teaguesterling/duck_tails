# git_blame

Return line-level authorship ("blame") for a file at a specific revision.
Analogous to `git blame <rev> -- <path>`, but as a composable DuckDB table.

Two output shapes are available:

- `git_blame` — one row per line of the file, with `line_content`. Best for
  joining against AST line ranges or per-line analysis.
- `git_blame_hunks` — one row per libgit2 blame hunk (contiguous range of
  lines from the same commit). Cheaper if you don't need line content.

Both come in a static form and an `_each` LATERAL variant.

## Syntax

```sql
git_blame(file_path_or_uri)
git_blame(file_path_or_uri, revision := 'HEAD', repo_path := '.')

git_blame_hunks(file_path_or_uri)
git_blame_hunks(file_path_or_uri, revision := 'HEAD', repo_path := '.')
```

## Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| (positional) | VARCHAR | — | Either a `git://` URI that embeds the repo, file, and revision, or a plain file path. |
| `repo_path` | VARCHAR | `.` | Repository path. Ignored when the positional is a `git://` URI. |
| `revision` | VARCHAR | `HEAD` | Revision to blame at (SHA, branch, tag, or `HEAD~N`). |
| `min_line` | BIGINT | 1 | Lower bound for lines to blame (1-indexed, inclusive). |
| `max_line` | BIGINT | last line | Upper bound for lines to blame (inclusive). |
| `ignore_whitespace` | BOOLEAN | `false` | Sets libgit2's `GIT_BLAME_IGNORE_WHITESPACE`. |
| `use_mailmap` | BOOLEAN | `false` | Sets libgit2's `GIT_BLAME_USE_MAILMAP`. |
| `first_parent` | BOOLEAN | `false` | Sets libgit2's `GIT_BLAME_FIRST_PARENT`. |

## Output — `git_blame`

| Column | Type | Description |
|--------|------|-------------|
| `repo_path` | VARCHAR | Absolute repository path |
| `file_path` | VARCHAR | Path within the repository |
| `file_ext` | VARCHAR | File extension |
| `revision` | VARCHAR | Revision requested |
| `line_number` | BIGINT | 1-indexed line in the file at `revision` |
| `line_content` | VARCHAR | Text of that line; NULL for binary files |
| `commit_hash` | VARCHAR | Commit that last touched this line |
| `author_name` | VARCHAR | Author of `commit_hash` |
| `author_email` | VARCHAR | Author email |
| `author_date` | TIMESTAMP | Author timestamp |
| `orig_commit_hash` | VARCHAR | Commit where this line was introduced |
| `orig_path` | VARCHAR | Path in `orig_commit_hash` (same as `file_path` unless libgit2 detected a rename) |
| `orig_line_number` | BIGINT | Line number in `orig_path` at `orig_commit_hash` |
| `boundary` | BOOLEAN | True if the hunk reached the oldest commit boundary |

## Output — `git_blame_hunks`

Same columns as `git_blame` except `line_number`/`line_content`/`orig_line_number`
are replaced with:

| Column | Type | Description |
|--------|------|-------------|
| `start_line` | BIGINT | 1-indexed first line of the hunk |
| `line_count` | BIGINT | Number of lines in the hunk |
| `orig_start_line` | BIGINT | Start line in `orig_path` at `orig_commit_hash` |

## Examples

### Basic blame

```sql
SELECT line_number, author_name, line_content
FROM git_blame('src/auth.py', repo_path := '.');
```

### Using a git:// URI

```sql
SELECT line_number, author_name
FROM git_blame('git://src/auth.py@HEAD');
```

### Restrict to a line range (AST node)

```sql
SELECT line_number, commit_hash, author_name
FROM git_blame('src/auth.py', min_line := 42, max_line := 57);
```

### Hunk view

```sql
SELECT start_line, line_count, commit_hash, author_date
FROM git_blame_hunks('src/auth.py')
ORDER BY start_line;
```

## LATERAL variants

### Blame many files in one query

```sql
SELECT t.file_path, b.line_number, b.author_name
FROM git_tree('HEAD') t,
     LATERAL git_blame_each(t.git_uri) b
WHERE t.file_ext = '.py';
```

### Blame the same file across a commit history

```sql
SELECT l.commit_hash, l.author_date, b.author_name
FROM git_log() l,
     LATERAL git_blame_each('src/auth.py', l.commit_hash) b
WHERE b.line_number = 1
LIMIT 10;
```

### Join against an AST table (pluckit-style)

```sql
SELECT a.node_id, b.author_name, b.author_date
FROM my_ast_nodes a,
     LATERAL git_blame_each(
         a.file_path,
         min_line := a.start_line,
         max_line := a.end_line) b;
```

## Notes

- **Rename tracking across files** is defined by libgit2's
  `GIT_BLAME_TRACK_COPIES_*` flags but those are currently unimplemented in
  libgit2. `orig_path` will almost always equal `file_path` until a future
  libgit2 release. `git_blame` does not expose those flags today.
- **Binary files** return `line_content` as NULL in `git_blame`. Use
  `git_blame_hunks` if you want authorship without line content.
- **Very long lines** (e.g. minified JS) are returned in full in `line_content`.
  Use `min_line`/`max_line` to restrict blame to a manageable range.
- `min_line`/`max_line` are passed directly to libgit2, so restricting a range
  is cheap — the blame computation itself only walks commits that touched the
  requested lines.

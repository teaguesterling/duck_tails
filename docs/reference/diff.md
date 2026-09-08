# Diff Functions

Duck Tails provides several functions for computing and analyzing text differences.

## read_git_diff

Compare two files and return the diff.

### Syntax

```sql
read_git_diff(file1)
read_git_diff(file1, file2)
```

### Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file1` | VARCHAR | Yes | The OLD side (local path or `git://` URI) |
| `file2` | VARCHAR | No | The NEW side (local path or `git://` URI) |

With one argument, that argument is the NEW side and the OLD side is the same
file at HEAD — so the diff reads the way `git diff <file>` does, with `+` lines
being what the working copy adds. The output columns keep the two-argument
meaning: `path1` is the old side (the `git://…@HEAD` URI it was compared
against) and `path2` is the argument as written.

A file that cannot be read raises. It used to come back as a one-row result set
whose `diff_text` began with `Error:`, which made a failure look like data.

### Returns

| Column | Type | Description |
|--------|------|-------------|
| `diff_text` | VARCHAR | Diff output: `+`/`-`/` ` prefixed lines |
| `path1` | VARCHAR | The old side that was compared |
| `path2` | VARCHAR | The new side that was compared |

### Examples

```sql
-- Compare file against HEAD
SELECT * FROM read_git_diff('README.md');

-- Compare two git versions
SELECT * FROM read_git_diff(
    'git://file.txt@v1.0',
    'git://file.txt@v2.0'
);

-- Compare local file with git version
SELECT * FROM read_git_diff(
    'config.json',
    'git://config.json@HEAD'
);

-- Compare two local files
SELECT * FROM read_git_diff('file1.txt', 'file2.txt');
```

---

## text_diff

Compute a unified diff between two text strings.

### Syntax

```sql
text_diff(old_text, new_text) → VARCHAR
```

### Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `old_text` | VARCHAR | Yes | Original text |
| `new_text` | VARCHAR | Yes | Modified text |

### Returns

| Type | Description |
|------|-------------|
| VARCHAR | Unified diff output |

### Examples

```sql
SELECT text_diff('Hello World', 'Hello DuckDB');
-- Returns unified diff showing the change

SELECT text_diff(
    'line1\nline2\nline3',
    'line1\nmodified\nline3'
);
```

---

## diff_text

Alias for `text_diff()`.

```sql
SELECT diff_text('old content', 'new content');
```

---

## text_diff_lines

Parse a diff string into individual lines with metadata.

### Syntax

```sql
text_diff_lines(diff_text) → TABLE
```

### Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `diff_text` | VARCHAR | Yes | A diff string: `text_diff()` output or a unified diff |

Diff headers (`--- a/x`, `+++ b/x`, `diff --git …`, `index …`,
`\ No newline at end of file`) are headers, not content, and are not returned as
rows. A `@@ -a,b +c,d @@` hunk header sets the line numbers that follow it;
without one, numbering starts at 1.

Like every table function, the argument has to be constant at bind time — a
correlated column will not bind.

### Returns

| Column | Type | Description |
|--------|------|-------------|
| `line_type` | VARCHAR | `CONTEXT`, `ADDED`, `REMOVED` or `MODIFIED` |
| `content` | VARCHAR | The line, without its diff prefix |
| `line_number` | BIGINT | The line's number in the file it exists in: the new file for `CONTEXT`/`ADDED`, the old file for `REMOVED` |

### Examples

```sql
SELECT * FROM text_diff_lines(
    text_diff('Hello World', 'Hello DuckDB')
);
```

```sql
-- Count the lines of each kind in a diff
SELECT
    line_type,
    COUNT(*) as line_count
FROM text_diff_lines(
    text_diff('line1' || chr(10) || 'line2', 'line1' || chr(10) || 'changed')
)
GROUP BY line_type;
```

---

## text_diff_stats

Get statistics about a diff.

### Syntax

```sql
text_diff_stats(diff_text) → STRUCT
text_diff_stats(old_text, new_text) → STRUCT
```

### Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `diff_text` | VARCHAR | Yes | A diff string: `text_diff()` output or a unified diff |
| `old_text`, `new_text` | VARCHAR | Yes | Two texts to diff, then count |

Header lines are not counted: `--- a/x` is not a removed line and `+++ b/x` is
not an added one.

### Returns

A single `STRUCT`:

| Field | Type | Description |
|-------|------|-------------|
| `lines_added` | BIGINT | Lines present only in the new text |
| `lines_removed` | BIGINT | Lines present only in the old text |
| `lines_modified` | BIGINT | Lines marked `~` by `text_diff()` |
| `lines_context` | BIGINT | Unchanged lines |

### Examples

```sql
SELECT (text_diff_stats(
    'line1' || chr(10) || 'line2' || chr(10) || 'line3',
    'line1' || chr(10) || 'modified' || chr(10) || 'line3' || chr(10) || 'line4'
)).lines_added;
-- 2

SELECT text_diff_stats(text_diff('a', 'b'));
-- {'lines_added': 1, 'lines_removed': 1, 'lines_modified': 0, 'lines_context': 0}
```

```sql
-- Compare change sizes across commits
SELECT
    l.commit_hash,
    l.message,
    (text_diff_stats(prev.text, curr.text)).lines_added,
    (text_diff_stats(prev.text, curr.text)).lines_removed
FROM git_log() l,
     LATERAL git_read_each(git_uri('.', 'README.md', l.commit_hash)) curr,
     LATERAL git_read_each(git_uri('.', 'README.md', l.commit_hash || '~1')) prev
LIMIT 10;
```

---

## Common Patterns

### Track File Changes Over Time

```sql
WITH file_versions AS (
    SELECT
        l.commit_hash,
        l.author_date,
        r.text,
        LAG(r.text) OVER (ORDER BY l.author_date) as prev_text
    FROM git_log() l,
         LATERAL git_read_each(git_uri('.', 'VERSION', l.commit_hash)) r
    LIMIT 20
)
SELECT
    commit_hash,
    author_date,
    text_diff(COALESCE(prev_text, ''), text) as changes
FROM file_versions
WHERE prev_text IS NOT NULL;
```

### Find Large Changes

```sql
SELECT * FROM (
    SELECT
        l.commit_hash,
        l.message,
        text_diff_stats(prev.text, curr.text) AS s
    FROM git_log() l,
         LATERAL git_read_each(git_uri('.', 'src/main.py', l.commit_hash)) curr,
         LATERAL git_read_each(git_uri('.', 'src/main.py', l.commit_hash || '^')) prev
)
WHERE s.lines_added + s.lines_removed > 50
ORDER BY s.lines_added + s.lines_removed DESC
LIMIT 10;
```

### Analyze Diff Content

```sql
-- Find what was removed. text_diff_lines is a table function, so its argument
-- must be constant at bind time: diff the two versions with read_git_diff and
-- filter its output, rather than feeding a subquery into text_diff_lines.
SELECT line
FROM read_git_diff('git://file.txt@v1.0', 'git://file.txt@v2.0'),
     UNNEST(string_split(diff_text, chr(10))) AS t(line)
WHERE line LIKE '-%';
```

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
| `file1` | VARCHAR | Yes | First file path (local or git://) |
| `file2` | VARCHAR | No | Second file path (defaults to comparing against HEAD) |

### Returns

| Column | Type | Description |
|--------|------|-------------|
| `diff_text` | VARCHAR | Unified diff output |

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
| `diff_text` | VARCHAR | Yes | Unified diff string |

### Returns

| Column | Type | Description |
|--------|------|-------------|
| `line_number` | INT32 | Line number in output |
| `line_type` | VARCHAR | Type: `add`, `remove`, `context`, `header` |
| `content` | VARCHAR | Line content |

### Examples

```sql
SELECT * FROM text_diff_lines(
    text_diff('Hello World', 'Hello DuckDB')
);
```

```sql
-- Analyze changes in a file
SELECT
    line_type,
    COUNT(*) as line_count
FROM text_diff_lines(
    (SELECT diff_text FROM read_git_diff(
        'git://src/main.py@HEAD~1',
        'git://src/main.py@HEAD'
    ))
)
GROUP BY line_type;
```

---

## text_diff_stats

Get statistics about a diff.

### Syntax

```sql
text_diff_stats(old_text, new_text) → TABLE
```

### Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `old_text` | VARCHAR | Yes | Original text |
| `new_text` | VARCHAR | Yes | Modified text |

### Returns

| Column | Type | Description |
|--------|------|-------------|
| `lines_added` | INT32 | Number of lines added |
| `lines_removed` | INT32 | Number of lines removed |
| `lines_changed` | INT32 | Total lines changed |

### Examples

```sql
SELECT * FROM text_diff_stats(
    'line1\nline2\nline3',
    'line1\nmodified\nline3\nline4'
);
-- Returns: lines_added=2, lines_removed=1, lines_changed=3
```

```sql
-- Compare change sizes across commits
SELECT
    l.commit_hash,
    l.message,
    s.lines_added,
    s.lines_removed
FROM git_log() l,
     LATERAL git_read_each(git_uri('.', 'README.md', l.commit_hash)) curr,
     LATERAL git_read_each(git_uri('.', 'README.md', l.commit_hash || '~1')) prev,
     LATERAL text_diff_stats(prev.text, curr.text) s
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
SELECT
    l.commit_hash,
    l.message,
    s.lines_added + s.lines_removed as total_changes
FROM git_log() l,
     LATERAL git_read_each(git_uri('.', 'src/main.py', l.commit_hash)) curr,
     LATERAL git_read_each(git_uri('.', 'src/main.py', l.commit_hash || '^')) prev,
     LATERAL text_diff_stats(prev.text, curr.text) s
WHERE s.lines_added + s.lines_removed > 50
ORDER BY total_changes DESC
LIMIT 10;
```

### Analyze Diff Content

```sql
-- Find what was removed
SELECT content
FROM text_diff_lines(
    text_diff(
        (SELECT text FROM git_read('git://file.txt@v1.0')),
        (SELECT text FROM git_read('git://file.txt@v2.0'))
    )
)
WHERE line_type = 'remove';
```

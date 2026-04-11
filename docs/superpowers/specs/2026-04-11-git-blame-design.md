# git_blame table function — design

**Issue:** [#18](https://github.com/teaguesterling/duck_tails/issues/18)
**Date:** 2026-04-11
**Branch:** `issue/18-git_blame`

## Summary

Add `git_blame` and `git_blame_hunks` table functions (plus `_each` LATERAL
variants) for line-level authorship lookup. Primary consumer is the pluckit
History plugin, which needs to join AST node line ranges against blame output to
answer "who last touched this function?".

`git_blame` returns one row per line with `line_content`; `git_blame_hunks`
returns one row per libgit2 blame hunk (native shape, cheaper). Both share a
single implementation core that wraps libgit2's `git_blame_file`.

## Motivation

`git_log --path` gives file-level history, but pluckit needs line-level. Without
this function, implementing per-line blame requires iterating the whole commit
history and diffing per commit, which is prohibitively expensive.
`libgit2` already exposes `git_blame_file` with native `min_line`/`max_line`
range restriction, so the downstream cost can be made proportional to the AST
node's line range rather than to the whole file.

## Non-goals

- **Rename tracking across files.** libgit2 defines
  `GIT_BLAME_TRACK_COPIES_*` flags but the shipped implementation leaves them
  unimplemented. `orig_path` will still be surfaced (free from the hunk struct)
  but will almost always equal `file_path` until libgit2 gains real rename
  tracking.
- **Committer fields.** Blame is author-centric and adding committer columns now
  would double the signature churn. Cheap to add later if demand appears.
- **Caching / incremental blame.** `git_blame_buffer` exists but is only useful
  for blaming an in-memory modified buffer against a cached committed blame —
  not the shape we want.

## Functions

Four table functions, all registered from a new `src/git_blame.cpp`:

| Function | Shape | Context |
|---|---|---|
| `git_blame` | one row per line | static |
| `git_blame_each` | one row per line | LATERAL |
| `git_blame_hunks` | one row per hunk | static |
| `git_blame_hunks_each` | one row per hunk | LATERAL |

### Signatures — static

```sql
git_blame(file_uri)
git_blame(file_path, repo_path := '...', revision := 'HEAD')
```

Same two forms for `git_blame_hunks`. The first positional argument is either a
`git://` URI (in which case `repo_path` / `revision` are ignored) or a plain
file path (resolved against `repo_path`, default `.`, at `revision`, default
`HEAD`). This mirrors `git_read`'s convention.

### Signatures — LATERAL

```sql
git_blame_each(file_uri)
git_blame_each(file_uri, revision)
```

Same two forms for `git_blame_hunks_each`. Matches `git_read_each`: first
positional LATERAL input is the URI/path, second optional LATERAL input is the
per-row revision override. Revision resolution order per row:

1. Second LATERAL positional (if present and non-NULL) — wins unconditionally.
2. Non-default `@rev` embedded in the input `git://` URI.
3. Bind-time `revision` named parameter (default `HEAD`).

Enables blaming the same file across many commits in one query:

```sql
SELECT l.commit_hash, b.line_number, b.author_name
FROM git_log() l,
     LATERAL git_blame_each('src/auth.py', l.commit_hash) b;
```

### Named parameters (all four functions)

| Name | Type | Default | Effect |
|---|---|---|---|
| `repo_path` | VARCHAR | `.` | Repo path when first positional is a plain path (ignored for `git://` URIs). |
| `revision` | VARCHAR | `HEAD` | Revision to blame (static form only; LATERAL uses second positional). |
| `min_line` | BIGINT | 1 | Lower bound (1-indexed, inclusive). Passed to `git_blame_options.min_line`. |
| `max_line` | BIGINT | end of file | Upper bound (inclusive). Passed to `git_blame_options.max_line`. |
| `ignore_whitespace` | BOOLEAN | false | Sets `GIT_BLAME_IGNORE_WHITESPACE`. |
| `use_mailmap` | BOOLEAN | false | Sets `GIT_BLAME_USE_MAILMAP`. |
| `first_parent` | BOOLEAN | false | Sets `GIT_BLAME_FIRST_PARENT`. |

`min_line` / `max_line` validation: both must be ≥ 1, and if both are set,
`min_line <= max_line`. Violations raise `BinderException`.

## Output schemas

### `git_blame` — one row per line

| Column | Type | Source |
|---|---|---|
| `repo_path` | VARCHAR | resolved absolute repo path |
| `file_path` | VARCHAR | resolved file path within repo |
| `file_ext` | VARCHAR | derived |
| `revision` | VARCHAR | resolved revision string (the one passed, not the SHA) |
| `line_number` | BIGINT | 1-indexed line in file at revision |
| `line_content` | VARCHAR | text of that line (from blob); NULL for binary files |
| `commit_hash` | VARCHAR | `hunk.final_commit_id` |
| `author_name` | VARCHAR | `hunk.final_signature->name` |
| `author_email` | VARCHAR | `hunk.final_signature->email` |
| `author_date` | TIMESTAMP | `hunk.final_signature->when` |
| `orig_commit_hash` | VARCHAR | `hunk.orig_commit_id` |
| `orig_path` | VARCHAR | `hunk.orig_path` |
| `orig_line_number` | BIGINT | `hunk.orig_start_line_number + (line - hunk.final_start_line_number)` |
| `boundary` | BOOLEAN | `hunk.boundary != 0` |

14 columns.

### `git_blame_hunks` — one row per hunk

Same as `git_blame`, with two swaps:

- `line_number` → `start_line` (`hunk.final_start_line_number`)
- `line_content` → `line_count` BIGINT (`hunk.lines_in_hunk`)
- `orig_line_number` → `orig_start_line` (`hunk.orig_start_line_number`)

14 columns total. The full ordered column list is: `repo_path`, `file_path`,
`file_ext`, `revision`, `start_line`, `line_count`, `commit_hash`,
`author_name`, `author_email`, `author_date`, `orig_commit_hash`, `orig_path`,
`orig_start_line`, `boundary`.

## Implementation

### File layout

Single new file `src/git_blame.cpp`, similar shape and size to
`src/git_diff_tree.cpp`. Shared helpers for the two output shapes live inside
the file; no new headers required beyond forward declarations in
`src/include/git_functions.hpp`.

### Data flow

1. **Bind**: parse first positional (URI vs plain path) via existing
   `ParseUnifiedGitParams` / `GitPath::Parse`. Validate `min_line`/`max_line`.
   Store `repo_path`, `file_path`, `revision`, and blame options in bind data.
2. **Init / execute**:
   - Open repo with `git_repository_open`.
   - `git_revparse_single` on `revision` → commit OID → set
     `opts.newest_commit`.
   - `git_blame_file(&blame, repo, file_path, &opts)`.
   - For `git_blame`: load blob at `file_path` from commit tree via
     `git_tree_entry_bypath` + `git_blob_lookup`. Binary check via
     `git_blob_is_binary`. If text, split content into lines by `\n` (strip
     trailing `\r`), storing a `vector<string_view>` indexed by 1-based line
     number. If binary, skip the split and leave `line_content` NULL for all
     emitted rows.
   - For `git_blame_hunks`: no blob load.
   - Iterate `git_blame_get_hunk_count` hunks. Apply `min_line`/`max_line`
     clipping client-side as a safety net (libgit2 already restricts, but hunks
     that cross the boundary are possible in theory).
   - **`git_blame`**: for each hunk, emit `lines_in_hunk` rows, one per line,
     computing `line_number = hunk.final_start_line_number + offset`,
     `orig_line_number = hunk.orig_start_line_number + offset`, and pulling
     `line_content` from the split blob by `line_number`.
   - **`git_blame_hunks`**: emit one row per hunk directly.
3. **LATERAL variants**: follow the `git_read_each` pattern. Runtime input
   chunk column 0 = URI/path, optional column 1 = revision override. Bind-time
   named parameters supply all other options. Per-input-row blame call, buffer
   results in `LocalState.current_rows`, emit up to `STANDARD_VECTOR_SIZE` at a
   time, return `HAVE_MORE_OUTPUT` / `NEED_MORE_INPUT` per the existing
   convention.

### Shared blame core

```cpp
struct GitBlameRow {
  string commit_hash;
  string author_name;
  string author_email;
  timestamp_t author_date;
  string orig_commit_hash;
  string orig_path;
  idx_t orig_line_number;   // resolved per line for git_blame
  idx_t line_number;        // for git_blame; unused for hunks
  idx_t start_line;         // for hunks; unused for git_blame
  idx_t line_count;         // for hunks; unused for git_blame
  string line_content;      // for git_blame; empty for hunks
  bool boundary;
};

// Computes blame hunks and (for per-line mode) loads and splits the blob.
// `per_line` controls whether hunks are expanded into per-line rows and whether
// the blob is loaded for line_content.
static void CollectBlameRows(
    git_repository *repo,
    const string &file_path,
    const string &revision,
    const GitBlameOptions &opts,
    bool per_line,
    vector<GitBlameRow> &rows);
```

Both `git_blame` and `git_blame_hunks` call `CollectBlameRows`; only the
row-to-column mapping differs. The `_each` variants call it once per LATERAL
input row.

### Error handling

All error paths raise `IOException` or `BinderException` to stay consistent
with the rest of the codebase:

- Binder errors: invalid `min_line`/`max_line`, empty/unparseable file URI.
- IO errors (wrapped from libgit2 `git_error_last`): repository open failure,
  revision parse failure, file not found at revision, blame computation
  failure, blob lookup failure.
- Empty file: returns zero rows (not an error).
- Binary file: `git_blame` returns rows with NULL `line_content`; `git_blame_hunks`
  returns rows normally (no blob load attempted).

### Registration

- `src/include/git_functions.hpp`: declare
  `void RegisterGitBlameFunction(ExtensionLoader &loader);`.
- `src/git_functions.cpp`: add `RegisterGitBlameFunction(loader);` to
  `RegisterGitFunctions`.
- `CMakeLists.txt`: add `src/git_blame.cpp` to the extension source list.

## Testing

New test file `test/sql/git_blame.test` using the existing
`test/tmp/main-repo` fixture (2 commits, README.md + src/main.cpp).

Coverage:

1. **Schema**: `DESCRIBE` column count and names for all four functions.
2. **Basic per-line blame**: `git_blame('README.md', repo_path := 'test/tmp/main-repo')`.
   Assert `count(*)` equals file line count; assert `author_name` on known
   lines; assert `line_content` matches expected text for line 1.
3. **URI form**: `git_blame('git://test/tmp/main-repo/README.md@HEAD')` produces
   identical output to the plain-path form.
4. **Revision forms**: `revision := 'HEAD~1'`, branch name, tag, full SHA —
   each returns expected line counts and authors.
5. **`min_line` / `max_line`**: restrict to a 3-line window; assert row count
   equals 3 and `line_number` values are in range.
6. **`min_line > max_line`**: expect `BinderException`.
7. **Option flags**: `ignore_whitespace`, `use_mailmap`, `first_parent` —
   smoke test that each accepts the param and returns results (behavioral
   verification deferred; these are libgit2-native).
8. **`git_blame_hunks`**: assert hunks exist; assert
   `sum(line_count)` equals file line count; assert `start_line` of first hunk
   is 1.
9. **LATERAL `git_blame_each`**: `git_tree(...) → LATERAL git_blame_each(git_uri)`
   over a small tree; assert each file's rows are grouped correctly.
10. **LATERAL with per-row revision**:
    `git_log() → LATERAL git_blame_each('README.md', l.commit_hash)` — verifies
    revision flows through LATERAL.
11. **Error paths**: nonexistent file, nonexistent revision, repository open
    failure.
12. **Empty file**: returns zero rows.
13. **Binary file** (small fixture binary, or create one in setup): `git_blame`
    returns rows with NULL `line_content`; `git_blame_hunks` returns normally.

### Docs

New `docs/reference/git_blame.md` following the format of
`docs/reference/git_read.md`. Covers syntax, named parameters, output schema
for both functions, examples (URI form, explicit form, `min_line`/`max_line`,
LATERAL joining against `git_log`, pluckit-style join against an AST table).

## Risks

- **Binary file handling**: libgit2 will happily blame a binary file. The design
  treats binary as "rows with NULL `line_content`" but the hunk structure may
  not correspond to meaningful "lines". Tests need to verify this doesn't
  crash or produce garbage in `line_content`.
- **Very long lines**: `line_content` as `VARCHAR` has no length cap. If a
  minified JS file has a single 2MB line, that row will be 2MB. Not addressing
  this in v1 — `max_line` is the escape valve. Documented as a caveat.
- **UTF-8 validation**: existing `git_read` has custom UTF-8 validation to
  prevent verification crashes. We need the same treatment when populating
  `line_content`. The blame core should reuse `git_read`'s `IsValidUTF8` helper
  — extract it into `git_utils.hpp` or duplicate until a third caller shows up.
  I'll duplicate for now and note the extraction opportunity.
- **libgit2 memory ownership**: `git_blame_hunk` contents (signatures, orig_path
  strings) are owned by the `git_blame` struct. We must copy all string data
  out before calling `git_blame_free`. The `GitBlameRow` struct uses owning
  `string`s, which covers this correctly as long as we copy *before* the free.

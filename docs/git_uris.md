# Git URI Specification

## Format

The canonical form of a git URI puts the revision in the query component:
```
git://path?ref=revision
```

The `path` component can be:
- A repository path: `git://.?ref=HEAD` or `git://path/to/repo?ref=main`
- A file path: `git://README.md?ref=HEAD` or `git://src/main.cpp?ref=v1.0`
- Both: `git://path/to/repo/src/main.cpp?ref=HEAD`

The `?ref=` part is optional. `git://README.md` names `README.md` at `HEAD`.

Because the revision is not part of the path, the path ends with the file name.
A reader can get the file extension with an ordinary `?`/`#` cut and needs no
knowledge of git.

### Deprecated: `@revision`

The older form `git://path@revision` is still accepted, with no removal date.
It is deprecated: new code should write `?ref=`, and `git_uri()` emits `?ref=`.
`git_canonicalize()` and the filesystem's `CanonicalizePath` rewrite the old
form into the new one (see [Canonical form](#canonical-form)).

Repository discovery automatically finds the `.git` directory by walking up from the provided path.

## Components

### Prefix
- `git://` - Required prefix that identifies this as a git URI

### Path
- Combined repository and file path
- Can be absolute or relative
- Repository is discovered by walking up the directory tree
- Examples: `.`, `README.md`, `src/main.cpp`, `/absolute/path/to/repo/file.txt`

### File Path Resolution
After repository discovery, the remaining path becomes the file path:
- `git://README.md@HEAD` → repo: `.` (discovered), file: `README.md`
- `git://src/file.cpp@main` → repo: `.` (discovered), file: `src/file.cpp`
- `git://repo/src/main.cpp@v1.0` → repo: `repo/`, file: `src/main.cpp`

### Revision
- The value of the `ref` query parameter
- Defaults to `HEAD` if omitted
- Types:
  - Commit SHA: `?ref=abc123`
  - Branch: `?ref=main`, `?ref=feature/new-thing`
  - Tag: `?ref=v1.0.0`
  - Relative refs: `?ref=HEAD~1`, `?ref=main^2`, `?ref=HEAD~5`
  - Reflog syntax: `?ref=HEAD@{0}`, `?ref=main@{1.day.ago}`
  - Pseudo-refs: `?ref=WORKDIR`, `?ref=STAGED`

### Query component rules
- The query component starts at the first `?ref=`. A `?` that is not followed
  by `ref=` stays in the path, where it is a glob wildcard
  (`git://data/file?.csv?ref=HEAD`).
- The value is everything after `?ref=`, taken literally. It is not
  percent-decoded and it is not split on `&`, because git ref names can contain
  both `%` and `&`. `ref` is the only parameter.
- Everything before `?ref=` is the path, taken literally. An `@` there is part
  of a file name: `git://icon@2x.png?ref=HEAD` names `icon@2x.png`.
- An empty value (`git://README.md?ref=`) means the default revision, the same
  as the deprecated `git://README.md@`.
- Do not combine the two forms. In `git://a.md@HEAD?ref=v1.0` the path is the
  file name `a.md@HEAD`, so the URI fails with a file-not-found error unless a
  file with that exact name exists.

### Revision in the deprecated `@` form
- The revision follows the **last** `@` that is not followed by `{`, so reflog
  syntax survives: `git://a.md@HEAD@{0}`.
- A glob after the revision is split off at its first metacharacter:
  `git://repo@HEAD/**/*.csv`.
- A revision that contains `/` is split against the repository: the longest
  prefix that resolves is the ref, and the rest is the path.
- A file name that contains `@` cannot be read with this form, because the
  parser takes that `@` as the separator. Use `?ref=`.

**Note:** Range syntax (`@v1.0..v2.0` or `@main...feature`) in the revision component is not currently supported in git URIs. This is unrelated to the `..` path security restriction above.

## Repository Discovery

When a git URI is parsed, the system discovers the repository by:
1. Starting from the specified path
2. Walking up the directory tree looking for `.git`
3. Using the first (deepest) repository found

### Examples
- `git://README.md@HEAD` → Discovers repo from current directory
- `git://src/file.cpp@main` → Discovers repo from `src/` upward
- `git://test/tmp/repo/file.md@v1.0` → Finds nested repository at `test/tmp/repo`

## Edge Cases

### Path Normalization
- Leading `./` is stripped for cleaner paths
- Trailing slashes are normalized: `git://repo/` becomes `git://repo`
- **Parent directory (`..`) handling:**
  - ✅ **Repository discovery**: `..` is resolved to absolute paths
    - `git://../other-repo@main` → Finds sibling repository
    - `../other-repo` → Resolves to `/absolute/path/to/other-repo`
  - ❌ **File paths extracted from URIs**: `..` is forbidden for security
    - Example: `git://../../etc/passwd@HEAD` → Rejected
    - Prevents path traversal to escape repository boundaries
    - Applies to file path component AFTER repository discovery

### Non-existent Files
- Files don't need to exist in the working tree
- Only needs to exist in the specified git revision
- Example: Query deleted files from history: `git://deleted-file.txt?ref=old-commit`

### Special Characters
- Spaces and special characters in paths should be escaped or quoted when used in SQL
- Internal handling preserves paths as-is

### Nested Repositories
- Discovers the most specific (deepest) repository first
- `git://outer/inner/file.txt` uses `inner/` repo if it exists, otherwise `outer/`

### Empty Components
- `git://?ref=HEAD`, `git://@HEAD` - Valid (an empty path means the current
  directory, with no file path)
- `git://.?ref=HEAD`, `git://.@HEAD` - Valid (current directory, no file path)
- `git://./file.txt?ref=`, `git://./file.txt@` - Valid (default revision)
- `git://./file.txt` - Valid (defaults to HEAD)

## Canonical form

`git_canonicalize(uri)` rewrites a deprecated `@revision` URI into the
`?ref=` form. `GitFileSystem::CanonicalizePath` does the same rewrite, so a
DuckDB extension that calls `FileSystem::CanonicalizePath` on a `git://` path
gets the canonical form without a build-time dependency on duck_tails.

```sql
SELECT git_canonicalize('git://README.md@HEAD');
-- Returns: 'git://README.md?ref=HEAD'

SELECT git_canonicalize('git://repo@HEAD/**/*.csv');
-- Returns: 'git://repo/**/*.csv?ref=HEAD'
```

The rewrite keeps the revision, because for git the revision is part of the
file's identity. The canonical URI opens the same file as the input. These
inputs are returned unchanged:
- a URI that is already canonical, or that has no revision
- a non-`git://` string
- a deprecated URI whose revision contains `/`, when the repository is not
  available to settle where the ref ends

## The git_uri() Function

Constructs a properly formatted git URI from components:

```sql
git_uri(repo_path, file_path, revision) → VARCHAR
```

### Parameters
- `repo_path` (VARCHAR): Repository path (can be `.` for current directory)
- `file_path` (VARCHAR): File path within repository (use empty string `''` for repository root)
- `revision` (VARCHAR): Git revision (commit/branch/tag)

### Examples
```sql
-- Basic usage
SELECT git_uri('.', 'README.md', 'HEAD');
-- Returns: 'git://./README.md?ref=HEAD'

-- With nested repository
SELECT git_uri('test/tmp/main-repo', 'src/main.cpp', 'v1.0');
-- Returns: 'git://test/tmp/main-repo/src/main.cpp?ref=v1.0'

-- Repository root
SELECT git_uri('/abs/path/repo', '', 'main');
-- Returns: 'git:///abs/path/repo?ref=main'
```

## Usage in Duck Tails Functions

### Reading Files
```sql
-- Read file from git
SELECT * FROM read_csv('git://data/sales.csv?ref=HEAD');

-- Using git_read
SELECT * FROM git_read('git://README.md?ref=main');

-- A glob returns one canonical URI per matching file
SELECT filename FROM read_text('git://docs/*.md?ref=v1.0');
```

### Tree Operations
```sql
-- git_tree outputs git_uri column (still in the deprecated @ form)
SELECT git_uri FROM git_tree('HEAD');
-- Returns URIs like: 'git://./src/main.cpp@abc123'

-- Using URIs to read specific files
SELECT * FROM git_tree('HEAD') t
JOIN LATERAL git_read_each(t.git_uri) r ON TRUE;
```

### Diff Operations
```sql
-- Compare two versions (takes two separate git URIs)
SELECT * FROM read_git_diff('git://file.txt?ref=v1.0', 'git://file.txt?ref=v2.0');

-- Compare against HEAD: the single argument is the NEW side, and the OLD side
-- is the same file at HEAD
SELECT * FROM read_git_diff('git://file.txt?ref=v1.0');
```

## Function Parameter Conventions

Duck Tails git functions accept their first parameter in two forms:

### 1. Filesystem Path (repo_path)
Some functions accept an optional revision as the second parameter:
```sql
-- Functions with optional ref parameter: git_tree, git_parents
SELECT * FROM git_tree('.', 'v1.0.0');
SELECT * FROM git_parents('.', 'main');

-- Functions with single parameter only: git_log, git_branches, git_tags
SELECT * FROM git_log('.');
SELECT * FROM git_branches('path/to/repo');

-- Without revision parameter (defaults to HEAD)
SELECT * FROM git_tree('path/to/repo');
```

### 2. Git URI
When passing a git:// URI, the revision is embedded in the URI:
```sql
-- All functions accept git URIs
SELECT * FROM git_log('git://path/to/repo?ref=main');
SELECT * FROM git_tree('git://.?ref=v1.0.0');
SELECT * FROM git_parents('git://.?ref=feature-branch');

-- Note: Cannot pass revision parameter when using git:// URI
-- This would error: git_tree('git://repo?ref=main', 'other-branch')
```

### Default Revisions
- Filesystem paths without revision parameter: defaults to `HEAD`
- Git URIs without a revision: default to `HEAD` (e.g., `git://file.txt`)
- Empty revision: the default (e.g., `git://file.txt?ref=`, `git://file.txt@`)

## Implementation Notes

- All URI construction should use `git_uri()` function for consistency
- Repository discovery happens at parse time via `GitPath::Parse`
- URIs are normalized to absolute repository paths internally
- The `git_uri` column in git functions provides ready-to-use URIs
- Functions validate that git:// URIs don't conflict with revision parameters

## Schema Standardization

Duck Tails functions follow a consistent schema:

**Key features:**
- All file-based functions return `git_uri` as the first column
- URIs are complete and ready to use with other functions
- Columns follow a standard order: `git_uri`, `repo_path`, `commit_hash`, etc.
- Enhanced metadata enables cross-function compatibility
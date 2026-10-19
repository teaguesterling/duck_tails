# Git URI: move the revision into the query component

**Status:** approved in principle (Teague, 2026-10-18). Grammar settled; one open
question below must be answered before implementation.

**Problem owner:** duck_tails. **Blocked consumers:** duckdb_markdown,
sitting_duck, duckdb_panduck.

---

## The problem

`git://<path>@<revision>` puts the revision after the file name, so the file
name's extension is not the end of the string:

```
git://README.md@HEAD
              ^^^^^ the extension is now "md@head"
```

Every consumer that decides anything from a file extension must therefore parse
duck_tails' grammar. Three already do, independently:

| repository | file | what it does |
|---|---|---|
| duckdb_markdown | `src/markdown_reader_files.cpp:52` | strips `?`/`#`, then `@` from the last component |
| sitting_duck | `src/include/ast_file_utils.hpp:55`, `src/ast_file_utils.cpp:307` | strips the scheme, then `@HEAD~3` |
| duckdb_panduck | `src/reader_registry.cpp:55` | `base.find_last_of('@')` — last `@` anywhere, so reflog syntax breaks it |

That grammar is not simple enough to duplicate correctly. `GitPath::Parse`
(`src/git_filesystem.cpp:219-270`) has to:

  - take the **last** `@` that is not followed by `{`, so reflog syntax
    (`@HEAD@{0}`, `@main@{1.day.ago}`) survives
  - tolerate a revision containing `/` (`@feature/foo`, `@refs/heads/main`)
  - split a glob appended *after* the revision (`@HEAD/**/*.csv`), which it can
    only do by finding a glob metacharacter, because slashes are legal in refs

A consumer that reimplements this gets it wrong, or gets it right and then
rots. Both are already visible: duckdb_markdown handles `?`/`#` and a trailing
`@rev` but not a glob suffix, and duckdb_panduck omits the `{` exclusion, so
`git://a.md@HEAD@{0}` already resolves to extension `md@head` there.

## The decision

**The canonical form becomes `git://<path>?ref=<revision>`.**

```
git://README.md?ref=HEAD
git://src/main.cpp?ref=feature/foo
git://docs/a.md?ref=HEAD@{0}
git://**/*.csv?ref=v1.0
```

The revision moves into the RFC 3986 query component. The file name is again
the end of the path, so its extension is readable without knowing anything
about git.

### Why the query component, and not a path prefix

`git://<revision>/<path>` was considered and rejected. Refs contain slashes
(`feature/foo`, `refs/heads/main`), so `git://feature/foo/a.md` cannot be split
into ref and path without asking the repository. The query component has no
such ambiguity: everything before `?` is the path, everything after is
parameters.

### Why this removes the special cases rather than relocating them

`?query` is not duck_tails syntax. It is the URI standard, and consumers
already implement it for their own reasons. duckdb_markdown's recogniser
already contains:

```cpp
// markdown_reader_files.cpp:64
auto query_pos = path.find_first_of("?#");
```

So `git://README.md?ref=HEAD` resolves to `README.md` under code that predates
this design and mentions neither git nor duck_tails.

**The duplication ends by delegation, not by deletion.** `@revision` is
accepted indefinitely (Teague, 2026-10-18), so a caller can always type
`git://README.md@HEAD` straight into a reader. A consumer that simply deletes
its strip therefore regresses on that spelling. What it deletes its strip *for*
is a call to `CanonicalizePath`, which duck_tails owns:

| consumer state | `git://README.md@HEAD` resolves? |
|---|---|
| keeps its own `@` strip | yes — the status quo, one buggy copy per consumer |
| strip deleted, calls `CanonicalizePath` | yes — VFS returns `?ref=`, the consumer's existing `?`/`#` cut finishes |
| strip deleted, no `CanonicalizePath` | **no** — extension reads `md@head` |

So the gain is not "no code anywhere". It is one implementation of the grammar,
in the repository that defines it, instead of three reimplementations of which
two are already wrong. A shared helper or a `git_strip_ref()` scalar function
would centralise the parsing too, but each would make every consumer depend on
duck_tails at build time; `CanonicalizePath` is already in DuckDB's own
interface, so it costs the consumer no new dependency.

## CanonicalizePath is the migration bridge

`FileSystem::CanonicalizePath` (`file_system.hpp:321`) is dispatched by
`VirtualFileSystem` to whichever filesystem claims the path:

```cpp
// duckdb src/common/virtual_file_system.cpp:278
string VirtualFileSystem::CanonicalizePath(const string &path_p, optional_ptr<FileOpener> opener) {
    return FindFileSystem(path_p, opener).CanonicalizePath(path_p, opener);
}
```

`GitFileSystem` overrides it to rewrite the deprecated form into the canonical
one:

```
CanonicalizePath("git://a.md@HEAD")  ->  "git://a.md?ref=HEAD"
```

**The revision is preserved, so canonicalization stays lossless.** This matters
more than it looks. An earlier draft had `CanonicalizePath` *strip* the
revision to yield `a.md`. That is wrong: for git the revision is part of the
file's identity, `git://a.md@HEAD` and `git://a.md@v1.0` are different files,
and a canonical path that cannot be opened again breaks the contract every
other filesystem keeps. Rewriting rather than stripping avoids this entirely.

### CanonicalizePath is an enhancement, not a requirement

Measured on the three DuckDB lines duck_tails and its consumers ship against:

| line | `CanonicalizePath` declared | VFS dispatch |
|---|---|---|
| v1.4.5 (`andium`) | no | no |
| v1.5.6 (`variegata`) | yes | yes |
| 2.0 | yes | yes |

**Whether a consumer may depend on it is per-consumer, not a blanket rule.** An
earlier draft of this spec said consumers must not depend on it at all. That
was wrong on this spec's own evidence: only duckdb_markdown and duckdb_scalarfs
carry an `andium` field, so only those two are constrained by v1.4.5.

| consumer | ships v1.4.5 | may call `CanonicalizePath` |
|---|---|---|
| sitting_duck, duckdb_panduck | no | yes, unconditionally |
| duckdb_markdown, duckdb_scalarfs | yes | behind an `if constexpr` probe, with the `@` strip as the fallback |

A consumer free of v1.4.5 should prefer the call, because it is the only route
that stays correct as the grammar evolves. A consumer carrying v1.4.5 keeps a
local fallback until it drops that line.

A consumer that does call it needs a `try`/`catch`: `FindFileSystem` raises
`File X requires the extension Y to be loaded` when no registered filesystem
claims the path.

## Compatibility

  - `@revision` continues to parse, indefinitely for now, and is documented as
    deprecated rather than removed. Removal is a separate decision with its own
    deprecation window.
  - `git_uri(repo_path, file_path, revision)` emits the **new** form. It is the
    advertised constructor, so it is how most callers will migrate without
    changing anything.
  - Both forms must not be combined. `git://a.md@HEAD?ref=v1.0` is an error,
    not a precedence puzzle.
  - `..` remains forbidden in the file-path component after repository
    discovery. The query component does not relax that.
  - Nested-repository discovery is unchanged: it operates on the path, which is
    now simply the part before `?`.

## Open question, to settle before implementation

**Is a URI with no revision valid?**

`docs/git_uris.md:89` says `git://./file.txt` is "Invalid (missing @
separator)". `GitPath::Parse` disagrees — `git_filesystem.cpp:268-270` falls
through to `result.revision = "HEAD"`. One of these is wrong today.

Under the new grammar a bare path becomes the ordinary case
(`git://README.md`, ref defaulting to `HEAD`), so this cannot stay ambiguous.
Settle it explicitly, then make the doc and the parser agree.

## Consumer-side work, filed separately

  - **duckdb_markdown** — the awkward one. Its descriptor carries `andium`, so
    it ships the v1.4.5 line, where `CanonicalizePath` does not exist. It must
    either keep a minimal `@` fallback behind an `if constexpr` probe, or stop
    supporting decorated git paths on that line. Keep the RFC `?`/`#` strip
    either way. Separate from issue #100, which is about paths with *no*
    extension and is not fixed by this.
  - **sitting_duck** — delete the strip in `ast_file_utils`.
  - **duckdb_panduck** — a one-line table edit, not a deletion. It keys cleaners
    by scheme (`src/reader_registry.cpp:80-84`), so `git://` moves from
    `StripGitRevision` to the `StripUrlQuery` already used for `http`/`https`,
    and `StripGitRevision` is deleted. Its table comment — "schemes whose URIs
    carry decoration AFTER the extension" — is the clearest statement of this
    problem anywhere in the fleet.

    panduck also has a live defect this fixes: `StripGitRevision` takes the last
    `@` with no `{` exclusion, so `git://a.md@HEAD@{0}` yields base `a.md@HEAD`
    and extension `md@head`. Reflog syntax is already broken there.

Each is blocked until `?ref=` is the documented form, and none of them may drop
`@` handling outright — only move it to `CanonicalizePath`.

Measured from the registry descriptors: only **duckdb_markdown** and
**duckdb_scalarfs** carry an `andium` (v1.4.5) field. sitting_duck, panduck and
duck_tails itself do not, so those three can call `CanonicalizePath`
unconditionally with no compatibility probe.

## Related: other schemes, and why they are not this problem

The same "the reader cannot read the extension" symptom has three other causes
in the fleet, each with its own answer. Rulings by Teague, 2026-10-18:

| scheme | answer |
|---|---|
| `pathvariable:`, `pathmacro:` (scalarfs) | **Resolve in the canonicalize phase.** Expanding a variable to its value is lossless and identity-preserving, exactly what `CanonicalizePath` is for. The resolved path carries a real extension. |
| `mcp://` (duckdb_mcp) | **Carry an extension on the resource URL.** There is no mimetype-to-extension map in the fleet, and building one is DuckDB's business, not ours. |
| `data:` (scalarfs) | **Requires an explicit reader.** No content sniffing. libmagic was considered and rejected as too heavy for the benefit. A caller who writes `read_markdown_blocks('data:,...')` has already declared the format. |

The `data:` ruling is what duckdb_markdown#100 must implement: a path with **no**
extension is no evidence either way and should be opened, while a path with a
recognised non-markdown extension (`.png`) should still be refused. That
three-way rule is independent of this design.

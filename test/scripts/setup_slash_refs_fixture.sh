#!/bin/bash
# Generates the slash-bearing-ref fixture used by test/sql/git_slash_refs.test.
#
# Git ref names routinely contain '/': every fully-qualified ref
# (refs/heads/main), the near-universal branch conventions
# (feature/foo, fix/bar) and release tags (release/v1.0). The git://
# URI form embeds the ref after '@', so the parser has to keep those
# slashes instead of cutting the ref at the first one.
#
# The repository is generated (not a committed tarball) so the refs it
# carries are visible in this script, and so a slash-bearing tag can be
# added without rebuilding a binary fixture.
#
# Layout (all on distinct commits so a truncated ref cannot accidentally
# read the right content):
#   main                -> README.md ("main branch content"), data/rows.csv (2 rows)
#   feature/foo         -> README.md ("feature branch content"), data/rows.csv (3 rows),
#                          feature-only.txt
#   fix/deep/nested     -> deep-only.txt  (multi-slash ref)
#   tag release/v1.0    -> lightweight tag on the main commit (slash in a tag)
#   tag release/v2.0    -> ANNOTATED tag on the feature/foo commit (slash + a tag
#                          object that has to be peeled to reach the commit)
#   tag v1.0            -> control tag without a slash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TEMP_DIR="${1:-$PROJECT_ROOT/test/tmp}"
mkdir -p "$TEMP_DIR"

REPO="$TEMP_DIR/slash-refs-repo"

rm -rf "$REPO"
mkdir -p "$REPO"
cd "$REPO"

git init -q
# Name the initial branch explicitly: the default differs across git
# versions and host configuration, and the tests assert on 'main'.
git checkout -q -b main
git config user.email "test@example.com"
git config user.name "Test User"
git config commit.gpgsign false

mkdir -p data

# --- main ------------------------------------------------------------
printf 'main branch content\n' > README.md
printf 'id,name\n1,alpha\n2,beta\n' > data/rows.csv
git add README.md data/rows.csv
git commit -q -m "main: initial commit"

git tag v1.0
git tag release/v1.0

# --- feature/foo -----------------------------------------------------
git checkout -q -b feature/foo
printf 'feature branch content\n' > README.md
printf 'id,name\n1,alpha\n2,beta\n3,gamma\n' > data/rows.csv
printf 'only on feature/foo\n' > feature-only.txt
git add README.md data/rows.csv feature-only.txt
git commit -q -m "feature/foo: add a third row and a branch-only file"

# Annotated tag: resolves to a tag object, not directly to a commit.
git tag -a release/v2.0 -m "release/v2.0"

# --- fix/deep/nested -------------------------------------------------
git checkout -q main
git checkout -q -b fix/deep/nested
printf 'only on fix/deep/nested\n' > deep-only.txt
git add deep-only.txt
git commit -q -m "fix/deep/nested: add a branch-only file"

git checkout -q main

echo "Slash-ref fixture ready at: $REPO"
git --no-pager branch --format='  branch %(refname:short)'
git --no-pager tag --format='  tag %(refname:short)'

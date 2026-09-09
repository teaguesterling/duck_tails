#!/bin/bash
# Generates the path-scoping fixture used by test/sql/path_scope_and_classification.test.
#
# Three of the defects in #55 need a repository whose answer visibly CHANGES when a
# path is honoured, and a sibling directory whose name shares a prefix with it:
#
#   src/          tracked + modified + untracked      -- the scope a caller asks for
#   src_backup/   tracked + modified + untracked      -- shares the prefix "src";
#                                                        a raw StartsWith filter
#                                                        pulls it in as well
#   docs/         tracked + modified + untracked      -- outside both scopes
#   latin1.txt    valid text, NOT valid UTF-8         -- git_read and git_tree
#   docs/latin1_untracked.txt                            classified these
#                                                        differently
#
# Every directory carries a change of each kind, so "the path was dropped"
# (whole-repo answer), "the path over-matched" (src_backup pulled in) and "the
# path was honoured" (src only) are three distinct row counts rather than
# variations on the same one.
#
# Generated rather than committed as a tarball so the layout stays readable, and
# because the workdir/untracked state a status test needs cannot be carried in a
# git object anyway.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TEMP_DIR="${1:-$PROJECT_ROOT/test/tmp}"
mkdir -p "$TEMP_DIR"

REPO="$TEMP_DIR/path-scope-repo"

rm -rf "$REPO"
mkdir -p "$REPO"
cd "$REPO"

git init -q
# Name the initial branch explicitly: the default differs across git versions
# and host configuration, and the tests assert on 'main'.
git checkout -q -b main
git config user.email "test@example.com"
git config user.name "Test User"
git config commit.gpgsign false

mkdir -p src src_backup docs

# --- commit 1: the base tree -----------------------------------------
printf 'src one\n'        > src/one.txt
printf 'backup one\n'     > src_backup/one.txt
printf 'docs one\n'       > docs/one.txt
# "caf<0xE9>" -- Latin-1, no NUL byte. libgit2's binary heuristic (NUL in the
# first 8000 bytes) calls this text; UTF-8 validation does not.
printf 'caf\351\n'        > latin1.txt
git add .
git commit -q -m "base tree"

# --- commit 2: one change in each directory --------------------------
printf 'src one changed\n'    > src/one.txt
printf 'backup one changed\n' > src_backup/one.txt
printf 'docs one changed\n'   > docs/one.txt
git add .
git commit -q -m "change one file in each directory"

# --- a second branch, for the git_log(repo, ref) overload (#52) ------
git checkout -q -b develop
printf 'only on develop\n' > develop-only.txt
git add develop-only.txt
git commit -q -m "develop: a commit that is not on main"
git checkout -q main

# --- working tree: modified tracked files ----------------------------
printf 'src one modified in workdir\n'    > src/one.txt
printf 'backup one modified in workdir\n' > src_backup/one.txt
printf 'docs one modified in workdir\n'   > docs/one.txt

# --- working tree: untracked files -----------------------------------
printf 'untracked under src\n'        > src/untracked.txt
printf 'untracked under src_backup\n' > src_backup/untracked.txt
printf 'untracked under docs\n'       > docs/untracked.txt
# The untracked counterpart of latin1.txt: git_tree classifies untracked files
# from disk rather than from a blob, so that path needs its own Latin-1 case.
printf 'caf\351\n'                    > docs/latin1_untracked.txt

echo "Path-scope fixture ready at: $REPO"
git --no-pager status --short | sed 's/^/  /'

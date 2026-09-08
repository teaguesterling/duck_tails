#!/bin/bash
# Generates the staged-glob fixture used by test/sql/git_staged_glob.test.
#
# The @STAGED / @INDEX branch of GitFileSystem::Glob enumerates the git index.
# It needs a repository whose index holds strictly more (and different) files
# than HEAD, so an assertion cannot pass by accidentally reading the commit
# tree, and whose staged paths live at more than one depth, so per-component
# glob matching ('**' crawls, '*' does not cross '/') is actually exercised.
#
# Generated rather than committed as a tarball because a *staged* state is the
# point: tar preserves the index file, but a checked-in fixture whose whole
# value is "these paths are in the index and not in HEAD" is far easier to read
# as a script than as a binary blob.
#
# Layout:
#   HEAD   -> README.md ("committed"), src/main.py, utf8.txt        (3 entries)
#   index  -> README.md (modified, staged), app.js, data/rows.csv,
#             notes.txt, src/main.py, src/util.py, utf8.txt        (7 entries)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TEMP_DIR="${1:-$PROJECT_ROOT/test/tmp}"
mkdir -p "$TEMP_DIR"

REPO="$TEMP_DIR/staged-glob-repo"

rm -rf "$REPO"
mkdir -p "$REPO"
cd "$REPO"

git init -q
# Name the initial branch explicitly: the default differs across git versions
# and host configuration.
git checkout -q -b main
git config user.email "test@example.com"
git config user.name "Test User"
git config commit.gpgsign false

mkdir -p src data

# --- the commit: three files -----------------------------------------
printf 'committed\n' > README.md
printf 'print("main")\n' > src/main.py
# A file with a multi-byte UTF-8 character, for git_read's max_bytes boundary:
# 'caf\xc3\xa9\n' is 6 bytes, so a 4-byte budget lands inside the two-byte 'e-acute'.
printf 'caf\xc3\xa9\n' > utf8.txt
git add README.md src/main.py utf8.txt
git commit -q -m "initial commit"

# --- the index: seven files, four of them never committed ------------
printf 'staged\n' > README.md
printf 'console.log("app");\n' > app.js
printf 'id,name\n1,alpha\n2,beta\n' > data/rows.csv
printf 'staged notes\n' > notes.txt
printf 'print("util")\n' > src/util.py
git add README.md app.js data/rows.csv notes.txt src/util.py

echo "Staged-glob fixture ready at: $REPO"
git --no-pager ls-files --cached | sed 's/^/  index /'

#!/bin/bash
# Generates the LFS local-cache fixture used by test/sql/git_lfs_local_cache.test.
#
# duck_tails serves an LFS-tracked file from the local object store
# (.git/lfs/objects/ab/cd/<oid>) and has no LFS Batch API client, so an object
# that was never pulled cannot be produced. The two cases have to be told
# apart, and neither may ever hand the caller the pointer text as if it were
# the file's content:
#
#   present.dat  - pointer whose 64-hex object IS materialized locally; reading
#                  it must return the object's content, not the pointer.
#   missing.dat  - pointer whose object is NOT in the local store; reading it
#                  must raise an error that names the object and says how to
#                  get it, rather than returning pointer text or an empty file.
#
# Generated rather than committed so no part of it depends on git-lfs being
# installed, on a remote being reachable, or on this repository's own
# .gitattributes.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TEMP_DIR="${1:-$PROJECT_ROOT/test/tmp}"
mkdir -p "$TEMP_DIR"

REPO="$TEMP_DIR/lfs-cache-repo"

rm -rf "$REPO"
mkdir -p "$REPO"
cd "$REPO"

git init -q
git checkout -q -b main
git config user.email "test@example.com"
git config user.name "Test User"
git config commit.gpgsign false
# Do not let a host git-lfs smudge/clean filter rewrite these pointers.
git config filter.lfs.smudge "cat"
git config filter.lfs.clean "cat"

GIT_DIR_ABS="$(cd "$REPO/.git" && pwd)"

write_pointer() {
    # $1 = filename, $2 = oid, $3 = size
    cat > "$1" <<EOF
version https://git-lfs.github.com/spec/v1
oid sha256:$2
size $3
EOF
}

# --- present.dat: object materialized in the local store -------------
PRESENT_CONTENT="lfs object served from the local cache"
PRESENT_SIZE=$(printf '%s' "$PRESENT_CONTENT" | wc -c | tr -d ' ')
PRESENT_OID=$(printf '%s' "$PRESENT_CONTENT" | sha256sum | awk '{print $1}')
write_pointer "present.dat" "$PRESENT_OID" "$PRESENT_SIZE"
OBJ_DIR="$GIT_DIR_ABS/lfs/objects/${PRESENT_OID:0:2}/${PRESENT_OID:2:2}"
mkdir -p "$OBJ_DIR"
printf '%s' "$PRESENT_CONTENT" > "$OBJ_DIR/$PRESENT_OID"

# --- missing.dat: well-formed oid, nothing in the local store --------
MISSING_CONTENT="this object is deliberately never materialized"
MISSING_SIZE=$(printf '%s' "$MISSING_CONTENT" | wc -c | tr -d ' ')
MISSING_OID=$(printf '%s' "$MISSING_CONTENT" | sha256sum | awk '{print $1}')
write_pointer "missing.dat" "$MISSING_OID" "$MISSING_SIZE"

git add present.dat missing.dat
git commit -q -m "LFS local-cache fixture"

echo "LFS local-cache fixture ready at: $REPO"
echo "  present.dat oid=$PRESENT_OID (object materialized locally)"
echo "  missing.dat oid=$MISSING_OID (object absent)"

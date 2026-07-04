#!/bin/bash
# Generates the LFS OID path-traversal security fixture used by
# test/sql/duck_tails_lfs_oid_traversal.test.
#
# It builds a small git repo containing:
#   1. leak.dat        - a valid-looking LFS pointer whose `oid` is a
#                        directory-traversal string that (pre-fix) resolves
#                        OUT of .git/lfs/objects/ to a test-created canary file.
#   2. malformed.dat   - an LFS pointer with a non-hex, wrong-length oid.
#   3. valid.dat       - a well-formed LFS pointer whose 64-hex object exists
#                        locally (positive control: must still resolve/read).
#
# The canary is a TEST-CREATED temp file (never a real system path such as
# /etc/passwd). Pre-fix, reading leak.dat leaks SECRET-CANARY; post-fix it is
# a clean IOException. The traversal string is computed empirically with
# `realpath -m` and the script FAILS LOUDLY if it cannot construct a string
# that resolves to the canary -- so the test can never silently pass.

set -euo pipefail

# Resolve the test/tmp directory (same location the other fixtures extract to).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"   # test/
PROJECT_ROOT="$(cd "$PROJECT_ROOT/.." && pwd)" # repo root
TEMP_DIR="${1:-$PROJECT_ROOT/test/tmp}"
mkdir -p "$TEMP_DIR"

REPO="$TEMP_DIR/lfs_oid_traversal_repo"
CANARY="$TEMP_DIR/lfs_canary_SECRET.txt"
CANARY_NAME="lfs_canary_SECRET.txt"
CANARY_CONTENT="SECRET-CANARY-DO-NOT-LEAK"

# Fresh state.
rm -rf "$REPO"
printf '%s' "$CANARY_CONTENT" > "$CANARY"
CANARY_SIZE=$(printf '%s' "$CANARY_CONTENT" | wc -c | tr -d ' ')
CANARY_REAL="$(realpath -m "$CANARY")"

mkdir -p "$REPO"
cd "$REPO"
git init -q
git config user.email "test@example.com"
git config user.name "Test User"
# Do not let a host git-lfs smudge filter interfere.
git config filter.lfs.smudge "cat"
git config filter.lfs.clean "cat"

GIT_DIR_ABS="$(cd "$REPO/.git" && pwd)"

# Replicate the EXACT concatenation performed by
# GitLFSFileHandle::BuildLFSObjectPath:
#   git_repository_path(repo) + "lfs/objects/" + oid[0:2] + "/" + oid[2:4] + "/" + oid
# git_repository_path() returns "<repo>/.git/" (trailing slash).
build_lfs_path() {
    local oid="$1"
    local d1="${oid:0:2}"
    local d2="${oid:2:2}"
    printf '%s' "$GIT_DIR_ABS/lfs/objects/$d1/$d2/$oid"
}

# Find the smallest number of ../ segments whose resulting path resolves to the
# canary. Search a generous range; fail loudly if none match.
TRAVERSAL_OID=""
for n in $(seq 1 16); do
    prefix=""
    for _ in $(seq 1 "$n"); do prefix="../$prefix"; done
    candidate="${prefix}${CANARY_NAME}"
    resolved="$(realpath -m "$(build_lfs_path "$candidate")")"
    if [ "$resolved" = "$CANARY_REAL" ]; then
        TRAVERSAL_OID="$candidate"
        break
    fi
done

if [ -z "$TRAVERSAL_OID" ]; then
    echo "FATAL: could not construct a traversal OID resolving to the canary." >&2
    echo "       .git dir: $GIT_DIR_ABS" >&2
    echo "       canary:   $CANARY_REAL" >&2
    exit 1
fi
echo "Traversal OID resolves to canary: $TRAVERSAL_OID -> $CANARY_REAL"

write_pointer() {
    # $1 = filename, $2 = oid, $3 = size
    cat > "$1" <<EOF
version https://git-lfs.github.com/spec/v1
oid sha256:$2
size $3
EOF
}

# 1) Malicious traversal pointer. size = canary size so read_text returns the
#    full leaked content pre-fix.
write_pointer "leak.dat" "$TRAVERSAL_OID" "$CANARY_SIZE"

# 2) Malformed (non-hex, wrong length) pointer.
write_pointer "malformed.dat" "not-a-valid-hex-oid" "50"

# 3) Valid pointer whose object exists locally (positive control).
VALID_CONTENT="valid-lfs-object-content"
VALID_SIZE=$(printf '%s' "$VALID_CONTENT" | wc -c | tr -d ' ')
VALID_OID=$(printf '%s' "$VALID_CONTENT" | sha256sum | awk '{print $1}')
write_pointer "valid.dat" "$VALID_OID" "$VALID_SIZE"
# Materialize the local LFS object it points to.
OBJ_DIR="$GIT_DIR_ABS/lfs/objects/${VALID_OID:0:2}/${VALID_OID:2:2}"
mkdir -p "$OBJ_DIR"
printf '%s' "$VALID_CONTENT" > "$OBJ_DIR/$VALID_OID"

git add leak.dat malformed.dat valid.dat
git commit -q -m "LFS OID traversal security fixture"

echo "LFS OID traversal fixture ready at: $REPO"
echo "  leak.dat      oid=$TRAVERSAL_OID (traversal -> canary)"
echo "  malformed.dat oid=not-a-valid-hex-oid"
echo "  valid.dat     oid=$VALID_OID (object materialized locally)"

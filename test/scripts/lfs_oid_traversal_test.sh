#!/bin/bash
# Security regression test for the LFS pointer OID path-traversal fix
# (GHSA-4577-v6fp-qc6f). It is driven as a shell test rather than a
# sqllogictest because the realistic exploit is a victim running DuckDB *inside*
# a cloned untrusted repository (cwd == the malicious repo): the LFS object
# resolution in GitFileSystem::OpenFile then reads the attacker's OID.
#
# SAFE by construction: the "secret" is a TEST-CREATED temp canary file, never a
# real system path such as /etc/passwd. The malicious OID is a ../ traversal
# string, computed at fixture-build time, that resolves to that canary.
#
#   Pre-fix : reading git://leak.dat@HEAD returns SECRET-CANARY (arbitrary read).
#   Post-fix: the OID is rejected with a clean IOException; the canary is not
#             read; a well-formed LFS pointer still resolves and reads.
#
# Usage: test/scripts/lfs_oid_traversal_test.sh [path/to/duckdb] [path/to/duck_tails.duckdb_extension]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

DUCKDB_BIN="${1:-$REPO_ROOT/build/release/duckdb}"
EXT_PATH="${2:-$REPO_ROOT/build/release/extension/duck_tails/duck_tails.duckdb_extension}"

if [ ! -x "$DUCKDB_BIN" ]; then
    echo "SKIP: duckdb binary not found at $DUCKDB_BIN (build the extension first)" >&2
    exit 2
fi
if [ ! -f "$EXT_PATH" ]; then
    echo "SKIP: duck_tails extension not found at $EXT_PATH" >&2
    exit 2
fi

# Build a fresh fixture repo + canary under test/tmp.
TMP_DIR="$REPO_ROOT/test/tmp"
bash "$SCRIPT_DIR/setup_lfs_oid_traversal_fixture.sh" "$TMP_DIR" >/dev/null
REPO="$TMP_DIR/lfs_oid_traversal_repo"

run_sql() {
    # Run a query from cwd == the fixture repo (the realistic exploit posture).
    ( cd "$REPO" && "$DUCKDB_BIN" -unsigned -batch -noheader -list \
        -c "LOAD '$EXT_PATH'; $1" ) 2>&1
}

fail=0
check() {
    local name="$1" cond="$2"
    if [ "$cond" = "ok" ]; then
        echo "PASS: $name"
    else
        echo "FAIL: $name"
        fail=1
    fi
}

echo "== 1. traversal OID must be rejected and canary must NOT leak =="
out="$(run_sql "SELECT content FROM read_text('git://leak.dat@HEAD');")"
echo "  output: $out"
if echo "$out" | grep -q "SECRET-CANARY"; then
    check "traversal OID does not leak canary" "bad"
elif echo "$out" | grep -q "64 lowercase hex"; then
    check "traversal OID rejected with OID-validation error" "ok"
else
    echo "  (unexpected output)"
    check "traversal OID rejected cleanly" "bad"
fi

echo "== 2. malformed (non-hex/short) OID -> clean OID-validation error =="
out="$(run_sql "SELECT content FROM read_text('git://malformed.dat@HEAD');")"
echo "  output: $out"
if echo "$out" | grep -q "64 lowercase hex"; then
    check "malformed OID rejected with OID-validation error" "ok"
else
    check "malformed OID rejected with OID-validation error" "bad"
fi

echo "== 3. positive control: a valid LFS pointer still resolves and reads =="
out="$(run_sql "SELECT content FROM read_text('git://valid.dat@HEAD');")"
echo "  output: $out"
if echo "$out" | grep -q "valid-lfs-object-content"; then
    check "valid LFS object still resolves" "ok"
else
    check "valid LFS object still resolves" "bad"
fi

if [ "$fail" -eq 0 ]; then
    echo "ALL CHECKS PASSED"
    exit 0
else
    echo "ONE OR MORE CHECKS FAILED"
    exit 1
fi

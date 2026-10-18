PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=duck_tails
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Test fixture setup (optional, uses - prefix to ignore if missing)
-include test/Makefile.inc
.PHONY: test-require
# A TEST RUN THAT SKIPPED REPORTS SUCCESS. `require duck_tails` in a .test file means
# "skip unless available", so when the extension cannot be loaded EVERY file skips and
# the harness still prints "all tests passed" with exit 0. That is how 680 tests across
# this fleet went unrun behind 12 green registry checks in October 2026
# (duckdb/community-extensions#3086: the recorded extension version is truncated to 32
# bytes while the engine compares the untruncated value, so `require` finds nothing).
#
# `--require <name>` makes a MISSING require a hard failure instead -- see
# sqllogic_test_runner.cpp, where IsRequired(param) turns it into
# parser.Fail("require %s: FAILED"). It is SCOPED to the named requirement, so files
# gated on anything else (notwindows) still skip normally. That scoping is why this
# is used rather than run_tests.py's --fail-require-skip, which collapses every skip
# reason into one string and so cannot tell an unloadable extension from a deliberate
# platform gate -- and which is a no-op as shipped anyway, searching stdout while the
# summary goes to stderr (duckdb/duckdb#27413, with a verified fix).
#
# THE NAME IS WRITTEN OUT, not $(EXT_NAME), because the two are not always the same:
# sitting_duck sets EXT_NAME=duckdb_ast while its tests say `require sitting_duck`, so
# $(EXT_NAME) there would match no token and make this guard a silent no-op -- the exact
# failure class it exists to catch.
#
# WATCHED IT FAIL BOTH WAYS on DuckDB 2.0 (b15358c371d) before being trusted, by moving
# the built extension aside (measured on duck_block_utils):
#     make test-require, present : exit 0  "2 skipped, 813 assertions in 16 cases"
#     make test-require, hidden  : exit 2  "require duck_block_utils: FAILED"
#     make test      , hidden    : exit 0  "ran tests: 0 passed, 18 skipped"  <-- the bug
# The present case's legitimate skips passing is the proof the scoping works.
#
# INERT ON THE v1.5.6 LINE, where `require` cannot fail: hiding the artifact changes
# nothing (measured on markdown and yaml). Harmless there, load-bearing on 2.0.
# Report the ASSERTION count this prints, never `make test`'s file count.
test-require:
	./build/release/test/unittest --require duck_tails "$(TESTS_BASE_DIRECTORY)*"

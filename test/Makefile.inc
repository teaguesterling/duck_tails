# Test fixture setup for duck_tails extension tests
# Include this from the main Makefile with: -include test/Makefile.inc

.PHONY: test_setup

# SKIP_TESTS is set by extension-ci-tools for the pass that does not run the
# tests. On linux the test phase invokes `make test_<type>` TWICE -- once inside
# the container, then again on the host with LINUX_CI_IN_DOCKER=0, where the
# Makefile sets SKIP_TESTS=1 and runs nothing. Fixture setup must follow the
# tests: the container pass creates test/tmp as root, and a second setup from the
# host (uid 1001) cannot write into it and fails the job.
test_setup:
	@if [ "$(SKIP_TESTS)" = "1" ]; then \
		echo "Fixture setup skipped: tests are skipped in this run."; \
	else \
		bash test/fixtures/setup_fixtures.sh setup; \
	fi

# Fixture setup has to run wherever the tests run, and extension-ci-tools has
# used two different target names for that:
#
#   v1.5-variegata (the stable job): test_<type> wraps test_<type>_internal
#   main (the duckdb-main canary):   test_<type> runs the tests itself; the
#                                    _internal targets no longer exist
#
# Hooking only the _internal names meant the canary never ran this, which is why
# its fixture setup had to be carried by a post_build_command in the workflow --
# a step marked "revert before merge" in #32 that had quietly become
# load-bearing. Hook both names so each path sets its own fixtures up, and the
# workflow needs no test-specific scaffolding.
test_release_internal: test_setup
test_debug_internal: test_setup
test_reldebug_internal: test_setup
test_release: test_setup
test_debug: test_setup
test_reldebug: test_setup

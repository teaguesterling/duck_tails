# Test fixture setup for duck_tails extension tests
# Include this from the main Makefile with: -include test/Makefile.inc

.PHONY: test_setup

# libgit2 ownership vs. the unittest binary's HOME sandbox.
#
# The CI image declares `git config --global --add safe.directory "*"` -- i.e.
# in /root/.gitconfig, under $HOME. duckdb main's unittest binary (test/unittest.cpp)
# setenv()s HOME to a fresh, empty per-run sandbox unless --keep-home is passed, so
# libgit2 stops finding that declaration. It then falls back to its ownership check
# for every repository the extension opens, and nothing we open is owned by root:
# the workspace is a bind mount owned by the host runner (uid 1001) and `tar -x` run
# as root restores the uid recorded in the fixture tarballs (uid 501). Every
# git_repository_open returns GIT_EOWNER, so every git:// path fails discovery and
# essentially the whole suite fails -- reported as only "6 failed" because
# run_tests.py counts failed BATCHES, not failed tests.
#
# duckdb v1.5.4's unittest never touches HOME, which is why the stable pipeline is
# unaffected. libgit2 resolves the SYSTEM config (/etc/gitconfig) without consulting
# $HOME, so re-declaring safe.directory there survives the sandbox. Root-only: a
# no-op outside the CI container, where the files are already owned by the caller.
#
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
		if [ "$$(id -u 2>/dev/null || echo 1)" = "0" ]; then \
			git config --file /etc/gitconfig --replace-all safe.directory '*' 2>/dev/null \
				&& echo "safe.directory='*' declared in /etc/gitconfig (survives the unittest HOME sandbox)" \
				|| echo "warning: could not declare safe.directory in /etc/gitconfig"; \
		fi; \
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

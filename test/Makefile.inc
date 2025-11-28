# Test fixture setup for duck_tails extension tests
# Include this from the main Makefile with: -include test/Makefile.inc

.PHONY: test_setup

test_setup:
	@bash test/fixtures/setup_fixtures.sh setup

# Add fixture setup as dependency to test targets (defined in duckdb_extension.Makefile)
test_release_internal: test_setup
test_debug_internal: test_setup
test_reldebug_internal: test_setup

PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=duck_tails
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Set up test fixtures before running tests
.PHONY: test_setup
test_setup:
	@echo "Setting up test fixtures..."
	@bash test/fixtures/setup_fixtures.sh setup

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Add fixture setup as dependency to test targets
test_release_internal: test_setup
test_debug_internal: test_setup
test_reldebug_internal: test_setup

# Custom test target that ensures we don't use cached extensions
# Delete any cached duck_tails extension and run tests with fresh build
.PHONY: test_fresh
test_fresh: release test_setup
	@echo "Removing cached duck_tails extensions..."
	@rm -f ~/.duckdb/extensions/*/duck_tails.duckdb_extension
	@rm -f ~/.duckdb/extensions/*/*/duck_tails.duckdb_extension
	@echo "Running tests with fresh extension build..."
	./build/release/test/unittest "$(PROJ_DIR)test/sql/*.test"
#!/bin/bash
# Script to run duck_tails tests with the extension automatically loaded

# Set the build directory (can be overridden by environment variable)
BUILD_DIR=${DUCK_TAILS_BUILD_DIR:-"$(pwd)/build/release"}

# Set the extension directory for DuckDB to find our extension
export DUCK_TAILS_EXTENSION_DIR="$BUILD_DIR/extension"

# Function to run a test with duck_tails loaded
run_test() {
    local test_file="$1"
    echo "Running test: $test_file"
    
    # Create a temporary test file with the LOAD statement prepended
    temp_test="/tmp/duck_tails_test_$$.test"
    
    # Add the LOAD statements at the beginning
    cat > "$temp_test" << EOF
# Auto-generated: Load duck_tails extension
statement ok
SET extension_directory='$DUCK_TAILS_EXTENSION_DIR';

statement ok  
LOAD duck_tails;

EOF
    
    # Append the original test content
    cat "$test_file" >> "$temp_test"
    
    # Run the test
    "$BUILD_DIR/test/unittest" "$temp_test"
    
    # Clean up
    rm -f "$temp_test"
}

# Run all tests or specific test
if [ $# -eq 0 ]; then
    # Run all duck_tails tests
    for test in test/sql/*.test; do
        run_test "$test"
    done
else
    # Run specific test
    run_test "$1"
fi
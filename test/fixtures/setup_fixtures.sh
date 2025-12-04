#!/bin/bash
# Test fixture extraction and management system for Duck Tails git repository tests.
# This script extracts compressed git repository fixtures to /tmp for testing,
# ensuring predictable and isolated test environments.

set -e

FIXTURES_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEMP_DIR=""
EXTRACTED_REPOS_FILE=""

# Function to print usage
usage() {
    echo "Usage: $0 [COMMAND]"
    echo "Commands:"
    echo "  setup    - Extract fixtures to test/tmp directory"
    echo "  cleanup  - Clean up extracted fixtures"
    echo "  validate - Validate extracted fixtures"
    echo "  list     - List available fixtures"
    echo "  path [repo-name] - Get path to extracted repository"
}

# Function to setup fixtures
setup_fixtures() {
    # Default to test/tmp relative to project root (where tests expect fixtures)
    # The project root is two directories up from this script
    local PROJECT_ROOT="$(cd "$FIXTURES_DIR/../.." && pwd)"
    TEMP_DIR="$PROJECT_ROOT/test/tmp"
    mkdir -p "$TEMP_DIR"
    
    echo "Setting up test fixtures in: $TEMP_DIR"
    
    # Store temp dir location for other commands
    EXTRACTED_REPOS_FILE="$TEMP_DIR/.extracted_repos"
    echo "TEMP_DIR=$TEMP_DIR" > "$EXTRACTED_REPOS_FILE"
    
    # Define fixtures with their expected properties (file|branches|tags|description)
    local fixtures=(
        "main-repo.tar.gz|main,develop,feature/test|v1.0.0,v2.0.0,beta-1|Main test repository"
        "empty-repo.tar.gz|main||Minimal repository for edge cases"
        "large-repo.tar.gz|main,branch-1,branch-2,branch-3,branch-4,branch-5,branch-6,branch-7,branch-8,branch-9,branch-10|tag-1,tag-2,tag-3,tag-4,tag-5|Large repository for performance testing"
        "special-chars-repo.tar.gz|main,feature/test-123,bugfix/issue-456|v1.0.0-beta,v1.0.0-rc.1|Repository with special characters"
    )
    
    # Extract each fixture
    for fixture_info in "${fixtures[@]}"; do
        IFS='|' read -r fixture_file expected_branches expected_tags description <<< "$fixture_info"
        fixture_path="$FIXTURES_DIR/$fixture_file"
        
        if [ ! -f "$fixture_path" ]; then
            echo "Error: Fixture not found: $fixture_path"
            exit 1
        fi
        
        echo "Extracting $fixture_file..."
        cd "$TEMP_DIR"
        tar -xzf "$fixture_path"
        
        # Store repository info
        repo_name="${fixture_file%.tar.gz}"
        repo_path="$TEMP_DIR/$repo_name"
        
        echo "$repo_name|$repo_path|$expected_branches|$expected_tags|$description" >> "$EXTRACTED_REPOS_FILE"
    done
    
    echo "Fixtures extracted successfully to: $TEMP_DIR"
    echo "Run '$0 validate' to verify fixture integrity"
}

# Function to get the temp directory from stored state
get_temp_dir() {
    EXTRACTED_REPOS_FILE=""
    
    # Check if environment variable is set
    if [ -n "$DUCK_TAILS_TEST_FIXTURES_DIR" ] && [ -f "$DUCK_TAILS_TEST_FIXTURES_DIR/.extracted_repos" ]; then
        EXTRACTED_REPOS_FILE="$DUCK_TAILS_TEST_FIXTURES_DIR/.extracted_repos"
    else
        # Look for temp directories
        for dir in $(find /tmp -name "duck_tails_test_*" -type d 2>/dev/null) $(find /var/folders -name "duck_tails_test_*" -type d 2>/dev/null); do
            if [ -d "$dir" ] && [ -f "$dir/.extracted_repos" ]; then
                EXTRACTED_REPOS_FILE="$dir/.extracted_repos"
                break
            fi
        done
    fi
    
    if [ -z "$EXTRACTED_REPOS_FILE" ]; then
        echo "Error: No extracted fixtures found. Run '$0 setup' first."
        exit 1
    fi
    
    TEMP_DIR=$(grep "TEMP_DIR=" "$EXTRACTED_REPOS_FILE" | cut -d'=' -f2)
}

# Function to validate fixtures
validate_fixtures() {
    get_temp_dir
    
    echo "Validating fixtures in: $TEMP_DIR"
    
    while IFS='|' read -r repo_name repo_path expected_branches expected_tags description; do
        [[ "$repo_name" =~ ^TEMP_DIR= ]] && continue  # Skip the temp dir line
        
        echo "Validating $repo_name..."
        
        if [ ! -d "$repo_path" ]; then
            echo "  ERROR: Repository directory not found: $repo_path"
            continue
        fi
        
        cd "$repo_path"
        
        # Check if it's a valid git repository
        if ! git rev-parse --git-dir >/dev/null 2>&1; then
            echo "  ERROR: Not a valid git repository"
            continue
        fi
        
        # Check branches
        if [ -n "$expected_branches" ]; then
            IFS=',' read -ra EXPECTED_BRANCHES <<< "$expected_branches"
            actual_branches=$(git branch -a | sed 's/^[* ] //' | sed 's/remotes\/origin\///' | sort -u | grep -v '^HEAD$' || true)
            
            for expected_branch in "${EXPECTED_BRANCHES[@]}"; do
                if ! echo "$actual_branches" | grep -q "^$expected_branch$"; then
                    echo "  WARNING: Expected branch '$expected_branch' not found"
                fi
            done
        fi
        
        # Check tags
        if [ -n "$expected_tags" ]; then
            IFS=',' read -ra EXPECTED_TAGS <<< "$expected_tags"
            actual_tags=$(git tag | sort)
            
            for expected_tag in "${EXPECTED_TAGS[@]}"; do
                if ! echo "$actual_tags" | grep -q "^$expected_tag$"; then
                    echo "  WARNING: Expected tag '$expected_tag' not found"
                fi
            done
        fi
        
        echo "  OK: $description"
        
    done < "$EXTRACTED_REPOS_FILE"
    
    echo "Validation complete"
}

# Function to cleanup fixtures
cleanup_fixtures() {
    get_temp_dir
    
    if [ -d "$TEMP_DIR" ]; then
        echo "Cleaning up fixtures from: $TEMP_DIR"
        rm -rf "$TEMP_DIR"
        echo "Cleanup complete"
    else
        echo "No fixtures to clean up"
    fi
}

# Function to list fixtures
list_fixtures() {
    echo "Available fixtures:"
    for fixture in "$FIXTURES_DIR"/*.tar.gz; do
        if [ -f "$fixture" ]; then
            basename "$fixture"
        fi
    done
}

# Function to get repository path
get_repo_path() {
    local repo_name="$1"
    
    if [ -z "$repo_name" ]; then
        echo "Error: Repository name required"
        echo "Usage: $0 path <repo-name>"
        exit 1
    fi
    
    get_temp_dir
    
    while IFS='|' read -r name path expected_branches expected_tags description; do
        [[ "$name" =~ ^TEMP_DIR= ]] && continue
        
        if [ "$name" = "$repo_name" ]; then
            echo "$path"
            return 0
        fi
    done < "$EXTRACTED_REPOS_FILE"
    
    echo "Error: Repository '$repo_name' not found"
    echo "Available repositories:"
    while IFS='|' read -r name path expected_branches expected_tags description; do
        [[ "$name" =~ ^TEMP_DIR= ]] && continue
        echo "  $name"
    done < "$EXTRACTED_REPOS_FILE"
    exit 1
}

# Main command processing
case "${1:-}" in
    setup)
        setup_fixtures
        ;;
    validate)
        validate_fixtures
        ;;
    cleanup)
        cleanup_fixtures
        ;;
    list)
        list_fixtures
        ;;
    path)
        get_repo_path "$2"
        ;;
    "")
        usage
        ;;
    *)
        echo "Error: Unknown command '$1'"
        usage
        exit 1
        ;;
esac
#!/bin/bash
# Setup script to create the test/lfs_test_repo for LFS testing
# This creates a local git repository with LFS files for testing purposes
# WITHOUT requiring git-lfs to be installed

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_REPO_DIR="$SCRIPT_DIR/lfs_test_repo"

# Function to create an LFS object manually
# Args: $1 = content, $2 = oid (hash)
create_lfs_object() {
    local content="$1"
    local oid="$2"

    # Extract first 2 and next 2 chars for directory structure
    local dir1="${oid:0:2}"
    local dir2="${oid:2:2}"

    # Create directory structure: .git/lfs/objects/ab/cd/
    local lfs_dir=".git/lfs/objects/$dir1/$dir2"
    mkdir -p "$lfs_dir"

    # Write the actual content (not the pointer) to the LFS object
    echo -n "$content" > "$lfs_dir/$oid"

    echo "  Created LFS object: $lfs_dir/$oid"
}

# Function to calculate SHA256 hash of content
calc_sha256() {
    echo -n "$1" | sha256sum | awk '{print $1}'
}

# Remove existing repo if it exists
if [ -d "$TEST_REPO_DIR" ]; then
    echo "Removing existing test repository..."
    rm -rf "$TEST_REPO_DIR"
fi

# Create and initialize the test repository
echo "Creating test repository at $TEST_REPO_DIR..."
mkdir -p "$TEST_REPO_DIR"
cd "$TEST_REPO_DIR"

git init
git config user.email "test@example.com"
git config user.name "Test User"

echo "Setting up manual LFS structure (no git-lfs required)..."

# Commit 1: Add LFS test data
echo "Creating commit 1: Add LFS test data..."

# Define the actual CSV content
TEST_DATA_CONTENT="id,name,amount
1,Alice,100
2,Bob,200
3,Charlie,300"

# Calculate hash and size
TEST_DATA_OID=$(calc_sha256 "$TEST_DATA_CONTENT")
TEST_DATA_SIZE=$(echo -n "$TEST_DATA_CONTENT" | wc -c)

echo "  test_data.csv: oid=$TEST_DATA_OID, size=$TEST_DATA_SIZE"

# Create the LFS pointer file
cat > test_data.csv << EOF
version https://git-lfs.github.com/spec/v1
oid sha256:$TEST_DATA_OID
size $TEST_DATA_SIZE
EOF

# Create the actual LFS object
create_lfs_object "$TEST_DATA_CONTENT" "$TEST_DATA_OID"

git add test_data.csv
git commit -m "Add LFS test data"

# Commit 2: Add missing LFS test (LFS pointer with invalid/missing object)
echo "Creating commit 2: Add missing LFS test (intentionally no LFS object)..."

# Use all zeros hash - we intentionally DON'T create the object
MISSING_OID="0000000000000000000000000000000000000000000000000000000000000000"

cat > missing_lfs.csv << EOF
version https://git-lfs.github.com/spec/v1
oid sha256:$MISSING_OID
size 50
EOF

echo "  missing_lfs.csv: pointer created, but LFS object intentionally omitted"

git add missing_lfs.csv
git commit -m "Add missing LFS test"

# Commit 3: Add regular CSV test file (not LFS)
echo "Creating commit 3: Add regular CSV test file (non-LFS)..."
cat > regular_file.csv << 'EOF'
name,value
Regular,123
File,456
EOF
git add regular_file.csv
git commit -m "Add regular CSV test file"

echo ""
echo "✅ Test repository setup complete!"
echo "Repository location: $TEST_REPO_DIR"
echo ""
echo "Commits:"
git log --oneline
echo ""
echo "LFS objects created:"
find .git/lfs/objects -type f 2>/dev/null | while read f; do
    size=$(wc -c < "$f")
    echo "  $f ($size bytes)"
done
echo ""
echo "Files in working directory:"
ls -lh *.csv

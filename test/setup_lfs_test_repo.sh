#!/bin/bash
# Setup script to create the test/lfs_test_repo for LFS testing
# This creates a local git repository with LFS files for testing purposes

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_REPO_DIR="$SCRIPT_DIR/lfs_test_repo"

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

# Initialize Git LFS (if available)
if command -v git-lfs &> /dev/null; then
    git lfs install --local
    git lfs track "*.csv"
    git add .gitattributes
    git commit -m "Initialize LFS tracking"
else
    echo "Warning: git-lfs not found. LFS pointer files will be created manually."
fi

# Commit 1: Add LFS test data
echo "Creating commit 1: Add LFS test data..."
cat > test_data.csv << 'EOF'
version https://git-lfs.github.com/spec/v1
oid sha256:4d7a214614ab2935c943f9e0ff69d22eadbb8f32b1258daaa5e2ca24d17e2393
size 87
EOF
git add test_data.csv
git commit -m "Add LFS test data"

# Commit 2: Add missing LFS test (LFS pointer with invalid/missing object)
echo "Creating commit 2: Add missing LFS test..."
cat > missing_lfs.csv << 'EOF'
version https://git-lfs.github.com/spec/v1
oid sha256:0000000000000000000000000000000000000000000000000000000000000000
size 50
EOF
git add missing_lfs.csv
git commit -m "Add missing LFS test"

# Commit 3: Add regular CSV test file (not LFS)
echo "Creating commit 3: Add regular CSV test file..."
cat > regular_file.csv << 'EOF'
name,value
Regular,123
File,456
EOF
git add regular_file.csv
git commit -m "Add regular CSV test file"

echo "Test repository setup complete!"
echo "Repository location: $TEST_REPO_DIR"
git log --oneline

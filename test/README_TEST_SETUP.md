# Test Repository Setup

## LFS Test Repository

The `test/lfs_test_repo` directory is a Git repository used for testing Git LFS functionality. It is not tracked in the main repository and must be created locally before running LFS-related tests.

### How It Works

The setup script creates a complete LFS test environment **without requiring git-lfs to be installed**. It manually creates:

1. **LFS pointer files** - Small text files that reference the actual data by SHA256 hash
2. **LFS objects** - The actual file content stored in `.git/lfs/objects/{hash[0:2]}/{hash[2:4]}/{hash}`
3. **Git commits** - Three commits with different file types for comprehensive testing

This approach allows testing LFS functionality in any environment, including CI systems where git-lfs may not be available.

### Setup

To create the test repository, run:

```bash
cd test
./setup_lfs_test_repo.sh
```

This script will:
1. Create a new Git repository at `test/lfs_test_repo`
2. Add test files including:
   - `test_data.csv` - An LFS pointer file with corresponding LFS object (50 bytes)
   - `missing_lfs.csv` - An LFS pointer with intentionally missing object (for error testing)
   - `regular_file.csv` - A regular CSV file (not LFS)
3. Create 3 commits to provide a history for testing
4. Manually create the LFS object directory structure and files

### What Gets Created

```
test/lfs_test_repo/
├── .git/
│   └── lfs/
│       └── objects/
│           └── 41/
│               └── 01/
│                   └── 41018de2...  (actual CSV data, 50 bytes)
├── test_data.csv      (LFS pointer file)
├── missing_lfs.csv    (LFS pointer, but no object)
└── regular_file.csv   (regular file)
```

### CI/CD Integration

For CI/CD pipelines, this setup script should be run before executing tests that depend on the LFS test repository. Add a step like:

```yaml
- name: Setup LFS test repository
  run: |
    cd test
    ./setup_lfs_test_repo.sh
```

**No git-lfs installation required!** The script works in any environment with basic bash/git.

### Why Not a Submodule?

The test repository is intentionally not tracked as a Git submodule because:
1. It's a local test fixture without a remote origin
2. It needs to be recreated for each test environment
3. The specific commit structure is important for version history tests
4. LFS objects in `.git/lfs/objects/` are not part of the Git repository and can't be pushed to a remote without git-lfs

### Why Manual LFS Objects?

By manually creating the LFS objects, we can:
- Test LFS functionality without requiring git-lfs installation
- Work in CI environments that don't support git-lfs
- Ensure deterministic test data (exact hashes and content)
- Test error conditions (missing LFS objects)

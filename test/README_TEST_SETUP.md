# Test Repository Setup

## LFS Test Repository

The `test/lfs_test_repo` directory is a Git repository used for testing Git LFS functionality. It is not tracked in the main repository and must be created locally before running LFS-related tests.

### Setup

To create the test repository, run:

```bash
cd test
./setup_lfs_test_repo.sh
```

This script will:
1. Create a new Git repository at `test/lfs_test_repo`
2. Add test files including:
   - `test_data.csv` - A valid LFS pointer file
   - `missing_lfs.csv` - An LFS pointer with a missing object (for error testing)
   - `regular_file.csv` - A regular CSV file (not LFS)
3. Create 3 commits to provide a history for testing

### CI/CD Integration

For CI/CD pipelines, this setup script should be run before executing tests that depend on the LFS test repository. Add a step like:

```yaml
- name: Setup LFS test repository
  run: |
    cd test
    ./setup_lfs_test_repo.sh
```

### Why Not a Submodule?

The test repository is intentionally not tracked as a Git submodule because:
1. It's a local test fixture without a remote origin
2. It needs to be recreated for each test environment
3. The specific commit structure is important for version history tests

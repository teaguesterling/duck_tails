# Duck Tails Testing Strategy

## Centralized Extension Loading

To avoid repeating the `LOAD duck_tails` statement in every test file, we have several approaches:

### 1. Using `__BUILD_DIRECTORY__` Placeholder (Recommended for CI)

In test files that need to use the freshly built extension:

```sql
# Set extension directory to build output
statement ok
SET extension_directory='__BUILD_DIRECTORY__/extension';

statement ok
LOAD duck_tails;
```

The `__BUILD_DIRECTORY__` placeholder is automatically replaced by the test runner with the actual build path.

### 2. Using CMake Post-Build Hooks

Our `CMakeLists.txt` automatically creates a symlink in the expected location:
- Build output: `extension/duck_tails/duck_tails.duckdb_extension`
- Symlink created at: `extension/v1.3.2/osx_arm64/duck_tails.duckdb_extension`

This allows tests to find the extension using standard DuckDB search paths.

### 3. Using Make Targets

```bash
# Run tests with fresh extension (removes cached versions first)
make test_fresh

# Standard test run
make test
```

### 4. Test-Specific Loading

For tests that need specific control:

```sql
# Load directly from build directory
statement ok
LOAD '__BUILD_DIRECTORY__/extension/duck_tails/duck_tails.duckdb_extension';
```

## Avoiding Cached Extensions

DuckDB caches extensions in `~/.duckdb/extensions/v{version}/{platform}/`. To ensure tests use the fresh build:

1. Delete cached extension: `rm -f ~/.duckdb/extensions/*/duck_tails.duckdb_extension`
2. Use `SET extension_directory` to point to build directory
3. Use the `make test_fresh` target which handles this automatically

## Test File Organization

- Regular tests: Can use simple `LOAD duck_tails` if cache is managed
- Debug tests: Should use `SET extension_directory='__BUILD_DIRECTORY__/extension'` to ensure fresh build
- CI tests: Should always use explicit paths to avoid cache issues

## Environment Variables

While DuckDB supports `LOCAL_EXTENSION_REPO` for known extensions, custom extensions like duck_tails need explicit loading. Future improvements could include:

1. Adding duck_tails to DuckDB's extension registry
2. Creating a custom test harness that pre-loads the extension
3. Using test fixtures that handle setup/teardown

## Current Limitations

DuckDB's SQLLogicTest framework doesn't support:
- Include files or test templates
- Global setup/teardown hooks
- Dynamic test generation

Therefore, each test file needs to handle its own extension loading, but we can minimize repetition using the strategies above.
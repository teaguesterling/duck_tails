# Contributing

Thank you for your interest in contributing to Duck Tails!

## Development Setup

### Prerequisites

- **Git** with submodule support
- **C++ compiler** (GCC 11+, Clang 14+, or MSVC 2019+)
- **CMake** 3.15+
- **vcpkg** package manager
- **Python 3** (for format checks)

### Clone the Repository

```bash
git clone --recursive https://github.com/teaguesterling/duck_tails.git
cd duck_tails
```

If you already cloned without `--recursive`:

```bash
git submodule update --init --recursive
```

### Build

```bash
# Release build
make release

# Debug build (with symbols)
make debug

# Clean and rebuild
make clean && make release
```

### Run Tests

```bash
# Run all tests
make test

# Expected: All tests passed (736 assertions in 46 test cases)
```

## Code Quality

### Format Check

```bash
# Check formatting
make format-check

# Auto-fix formatting
make format-fix
```

### Tidy Check

```bash
# Requires libgit2 and clang-tidy
make tidy-check
```

## Project Structure

```
duck_tails/
├── src/                    # Source files
│   ├── include/            # Header files
│   ├── git_*.cpp           # Git functions
│   ├── text_diff.cpp       # Diff functions
│   └── duck_tails_extension.cpp
├── test/
│   ├── sql/                # SQL test files
│   └── fixtures/           # Test data
├── duckdb/                 # DuckDB submodule
├── extension-ci-tools/     # CI configuration
├── docs/                   # Documentation
└── vcpkg.json              # Dependencies
```

## Adding a New Function

### 1. Create the Implementation

```cpp
// src/git_myfunction.cpp
#include "duckdb.hpp"

namespace duckdb {

void RegisterMyFunction(ExtensionLoader &loader) {
    auto &db = loader.GetDatabaseInstance();

    auto my_function = TableFunction("git_myfunction", {...});
    ExtensionUtil::RegisterFunction(db, my_function);
}

} // namespace duckdb
```

### 2. Register the Function

Add to `src/duck_tails_extension.cpp`:

```cpp
void DuckTailsExtension::Load(ExtensionLoader &loader) {
    // ... existing registrations
    RegisterMyFunction(loader);
}
```

### 3. Add Tests

Create `test/sql/git_myfunction.test`:

```sql
# name: test/sql/git_myfunction.test
# group: [duck_tails]

require duck_tails

statement ok
SELECT * FROM git_myfunction() LIMIT 1;
```

### 4. Add Documentation

Add to `docs/reference/git_myfunction.md` and update `mkdocs.yml`.

## Testing Guidelines

### Test File Format

Tests use DuckDB's sqllogictest format:

```sql
# name: test/sql/feature.test
# group: [duck_tails]

require duck_tails

# Statement that should succeed
statement ok
SELECT 1;

# Query with expected result
query I
SELECT 42;
----
42

# Statement that should fail
statement error
SELECT * FROM nonexistent_table;
```

### Test Fixtures

Test fixtures are in `test/fixtures/`:

```bash
# Extract fixtures before running tests
test/fixtures/setup_fixtures.sh

# Validate fixtures
test/fixtures/setup_fixtures.sh validate
```

## Pull Request Process

1. **Fork** the repository
2. **Create a branch** for your feature
3. **Write tests** for new functionality
4. **Ensure tests pass**: `make test`
5. **Check formatting**: `make format-check`
6. **Submit PR** with clear description

## Code Style

- Use DuckDB's code style (tab indentation)
- Run `make format-fix` before committing
- Use RAII for resource management
- Include clear error messages

## Reporting Issues

When reporting bugs, please include:

- DuckDB version
- Duck Tails version/commit
- Operating system
- Minimal reproduction case
- Error message (full text)

## Questions?

- Open a [GitHub Discussion](https://github.com/teaguesterling/duck_tails/discussions)
- Check existing [Issues](https://github.com/teaguesterling/duck_tails/issues)

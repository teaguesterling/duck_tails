# Installation

## Prerequisites

- **DuckDB** v1.3.2 or later
- **vcpkg** package manager (for building from source)
- **libgit2** (automatically installed via vcpkg)

## Building from Source

### Clone the Repository

```bash
git clone --recursive https://github.com/teaguesterling/duck_tails.git
cd duck_tails
```

The `--recursive` flag ensures the DuckDB submodule is also cloned.

### Build

```bash
# Build in release mode
make release

# Or just 'make' for default release build
make
```

### Verify Installation

```bash
# Run the test suite
make test

# Expected output: All tests passed (736 assertions in 46 test cases)
```

### Load the Extension

```bash
# Start DuckDB with the extension
./build/release/duckdb -c "LOAD 'duck_tails';"
```

## Build Options

### Debug Build

```bash
make debug
```

### Clean Build

```bash
make clean
make release
```

## Dependencies

Duck Tails uses vcpkg for dependency management. The following dependencies are automatically installed:

| Dependency | Purpose |
|------------|---------|
| libgit2 | Git repository access |
| OpenSSL | Cryptographic operations for git |

## Troubleshooting

### vcpkg Not Found

Ensure vcpkg is installed and the `VCPKG_ROOT` environment variable is set:

```bash
export VCPKG_ROOT=/path/to/vcpkg
```

### libgit2 Build Errors

If libgit2 fails to build, ensure you have the required system dependencies:

```bash
# Ubuntu/Debian
sudo apt-get install libssl-dev libssh2-1-dev

# macOS
brew install openssl libssh2

# Fedora
sudo dnf install openssl-devel libssh2-devel
```

### Extension Load Errors

If the extension fails to load, verify:

1. DuckDB version compatibility (v1.3.2+)
2. Extension was built successfully
3. Extension file exists at `build/release/duck_tails.duckdb_extension`

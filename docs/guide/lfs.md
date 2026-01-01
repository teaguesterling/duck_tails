# Git LFS Support

Duck Tails automatically detects and handles Git Large File Storage (LFS) files.

## How It Works

When you read a file through Duck Tails, it automatically:

1. Detects if the file is an LFS pointer
2. Locates the actual content in the local LFS cache
3. Streams the real file content

This is transparent - you don't need to do anything special.

## LFS Pointer Detection

LFS pointer files are small text files with a specific format:

```
version https://git-lfs.github.com/spec/v1
oid sha256:abc123...
size 12345678
```

Duck Tails recognizes these automatically and fetches the real content.

## Reading LFS Files

### Standard Usage

```sql
-- LFS files work just like regular files
SELECT * FROM read_csv('git://data/large-dataset.csv@HEAD');

-- Or with git_read
SELECT * FROM git_read('git://data/large-file.bin@HEAD');
```

### Checking LFS Status

```sql
-- The git_tree output shows file sizes
SELECT
    file_path,
    size_bytes,
    kind
FROM git_tree('HEAD')
WHERE file_path LIKE 'data/%'
ORDER BY size_bytes DESC;
```

## Local Cache Requirements

Duck Tails currently reads LFS files from the local cache only. Before querying LFS files, ensure they are downloaded:

```bash
# Download all LFS files
git lfs pull

# Download specific files
git lfs pull --include="data/*.csv"
```

### Cache Location

LFS objects are stored in:

```
.git/lfs/objects/<oid-prefix>/<oid>
```

For example:
```
.git/lfs/objects/ab/cd/abcd1234567890...
```

## Streaming Architecture

LFS files are streamed rather than loaded entirely into memory:

- Large files don't exhaust memory
- Seek operations are supported
- Progress tracking is available

## Example Queries

### Read Large CSV

```sql
-- Works with large LFS-tracked CSV files
SELECT
    COUNT(*) as row_count,
    SUM(amount) as total
FROM read_csv('git://data/transactions.csv@HEAD');
```

### Compare LFS File Versions

```sql
-- Compare record counts across versions
WITH v1 AS (
    SELECT COUNT(*) as cnt FROM read_csv('git://data/metrics.csv@v1.0')
),
v2 AS (
    SELECT COUNT(*) as cnt FROM read_csv('git://data/metrics.csv@v2.0')
)
SELECT v2.cnt - v1.cnt as records_added
FROM v1, v2;
```

### Query Historical LFS Data

```sql
-- Read LFS file from a specific commit
SELECT * FROM read_parquet('git://analytics/data.parquet@abc1234')
LIMIT 1000;
```

## Limitations

### Remote LFS Not Yet Supported

Currently, LFS files must be present in the local cache. Remote LFS fetching via the Batch API is planned for a future release.

If an LFS file isn't in the local cache:

```sql
SELECT * FROM git_read('git://large-file.bin@HEAD');
-- Error: Remote LFS not yet implemented. Run 'git lfs pull' to download LFS objects locally.
```

### Solution

```bash
# Fetch the LFS files first
git lfs pull
```

## Troubleshooting

### File Not Found in LFS Cache

```bash
# Check if file is tracked by LFS
git lfs ls-files

# Pull LFS files
git lfs pull

# Pull specific file
git lfs pull --include="path/to/file"
```

### Verify LFS Installation

```bash
# Check LFS is installed
git lfs version

# Check LFS tracking rules
cat .gitattributes
```

### Check Cache Contents

```bash
# List LFS objects in cache
ls -la .git/lfs/objects/
```

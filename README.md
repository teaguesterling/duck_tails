# Duck Tails 🦆

**Smart Development Intelligence for DuckDB**

Duck Tails is a DuckDB extension that brings git-aware data analysis capabilities to your database. Query your git history, access files at any revision, and perform version-aware data analysis - all with SQL.

## ✨ Current Features (Phase 1)

### 🗂️ Git Filesystem
Access any file in your git repository at any commit, branch, or tag using the `git://` protocol:

```sql
-- Read a CSV file from the current HEAD
SELECT * FROM read_csv('git://data/sales.csv@HEAD');

-- Compare data between commits
SELECT * FROM read_csv('git://data/sales.csv@HEAD~1');

-- Access files from a specific branch
SELECT * FROM read_csv('git://config.json@feature-branch');

-- Load data from a tagged release
SELECT * FROM read_csv('git://metrics.csv@v1.0.0');
```

### 📊 Git Table Functions
Query your git repository metadata directly:

```sql
-- View commit history
SELECT commit_hash, author_name, message, author_date 
FROM git_log('.');

-- List all branches
SELECT branch_name, commit_hash, is_current 
FROM git_branches('.');

-- Show all tags
SELECT tag_name, commit_hash, tagger_date 
FROM git_tags('.');
```

### 🔄 Version-Aware Analysis
Perform sophisticated version comparisons and historical analysis:

```sql
-- Compare record counts across versions
WITH current AS (SELECT COUNT(*) as cnt FROM read_csv('git://data.csv@HEAD')),
     previous AS (SELECT COUNT(*) as cnt FROM read_csv('git://data.csv@HEAD~1'))
SELECT current.cnt - previous.cnt AS records_added
FROM current, previous;

-- Analyze changes over time
SELECT 
    c.commit_hash,
    c.author_date,
    (SELECT COUNT(*) FROM read_csv('git://metrics.csv@' || c.commit_hash)) as metric_count
FROM git_log('.') c
WHERE c.author_date > '2024-01-01'
ORDER BY c.author_date;
```

## 🚀 Quick Start

### Prerequisites
- DuckDB v1.3.2+
- vcpkg package manager
- libgit2 (automatically installed via vcpkg)

### Building
```bash
# Clone and build
git clone <repository-url>
cd duck_tails
make

# Load the extension
./build/release/duckdb -c "LOAD 'duck_tails';"
```

### Basic Usage
```sql
-- Load the extension
LOAD 'duck_tails';

-- Query git history
SELECT * FROM git_log('.') LIMIT 5;

-- Access version-controlled data
SELECT * FROM read_csv('git://test/data/sales.csv@HEAD');
```

## 📋 Examples

### Historical Data Analysis
```sql
-- Compare sales data between releases
SELECT 
    'v1.0' as version,
    SUM(amount) as total_sales
FROM read_csv('git://sales.csv@v1.0')
UNION ALL
SELECT 
    'v2.0' as version,
    SUM(amount) as total_sales  
FROM read_csv('git://sales.csv@v2.0');
```

### Repository Analytics
```sql
-- Most active contributors
SELECT 
    author_name,
    COUNT(*) as commit_count,
    MIN(author_date) as first_commit,
    MAX(author_date) as latest_commit
FROM git_log('.')
GROUP BY author_name
ORDER BY commit_count DESC;
```

### Configuration Drift Detection
```sql
-- Compare configuration files across branches
SELECT 
    'main' as branch,
    * 
FROM read_json('git://config.json@main')
UNION ALL
SELECT 
    'develop' as branch,
    *
FROM read_json('git://config.json@develop');
```

## 🏗️ Architecture

Duck Tails implements a custom DuckDB FileSystem that intercepts `git://` URLs and translates them into libgit2 operations:

- **GitFileSystem**: Handles git:// protocol registration and file access
- **GitFileHandle**: Memory-backed file handles for git blob content  
- **GitPath**: Parser for git://path@revision syntax
- **Git Table Functions**: Direct repository metadata access

## 🛣️ Roadmap

### Phase 2: Text Diff Intelligence
- `TextDiff` and `DiffInfo` data types
- Native diff computation and analysis
- Conflict detection and resolution helpers

### Phase 3: Semantic Code Intelligence  
- AST-aware diff analysis
- Function and class change tracking
- Intelligent merge conflict resolution

### Phase 4: Development Workflow Integration
- Pull request analytics
- Code review intelligence
- Development velocity metrics

## 🤝 Contributing

Duck Tails is built with modern C++, DuckDB's extension framework, and libgit2. Key technologies:

- **DuckDB Extension API**: FileSystem and table function registration
- **libgit2**: Git repository access and blob content loading
- **vcpkg**: Dependency management for cross-platform builds
- **RAII**: Smart pointer usage throughout for memory safety

## 📜 License

[License details to be added]

## 🙏 Acknowledgments

Built with ❤️ using:
- [DuckDB](https://duckdb.org/) - Fast analytical database
- [libgit2](https://libgit2.org/) - Portable git implementation
- [vcpkg](https://vcpkg.io/) - C++ package manager

---

*Duck Tails: Where data analysis meets version control* 🦆✨
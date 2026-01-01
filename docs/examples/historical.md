# Historical Data Analysis

Query and compare data across git history.

## Version Comparison

### Compare Record Counts

```sql
WITH current AS (
    SELECT COUNT(*) as cnt FROM read_csv('git://data/sales.csv@HEAD')
),
previous AS (
    SELECT COUNT(*) as cnt FROM read_csv('git://data/sales.csv@HEAD~1')
)
SELECT
    current.cnt as current_count,
    previous.cnt as previous_count,
    current.cnt - previous.cnt as records_added
FROM current, previous;
```

### Compare Aggregates Across Versions

```sql
SELECT
    'v1.0' as version,
    SUM(amount) as total_sales,
    AVG(amount) as avg_sale
FROM read_csv('git://data/sales.csv@v1.0')
UNION ALL
SELECT
    'v2.0' as version,
    SUM(amount) as total_sales,
    AVG(amount) as avg_sale
FROM read_csv('git://data/sales.csv@v2.0');
```

### Track Metric Over Time

```sql
SELECT
    l.commit_hash,
    l.author_date,
    (SELECT COUNT(*) FROM read_csv(git_uri('.', 'data/records.csv', l.commit_hash))) as record_count
FROM git_log() l
WHERE l.author_date > '2024-01-01'
ORDER BY l.author_date;
```

## Schema Evolution

### Compare Column Changes

```sql
SELECT
    'v1.0' as version,
    column_name,
    column_type
FROM (
    SELECT * FROM read_csv('git://data.csv@v1.0') LIMIT 0
)
UNION ALL
SELECT
    'v2.0' as version,
    column_name,
    column_type
FROM (
    SELECT * FROM read_csv('git://data.csv@v2.0') LIMIT 0
);
```

### Find When Column Was Added

```sql
WITH tagged_versions AS (
    SELECT tag_name, commit_hash
    FROM git_tags()
    WHERE tag_name LIKE 'v%'
)
SELECT
    t.tag_name,
    (SELECT COUNT(*) FROM (
        SELECT * FROM read_csv(git_uri('.', 'data.csv', t.commit_hash)) LIMIT 0
    ) WHERE column_name = 'new_column') > 0 as has_column
FROM tagged_versions t
ORDER BY t.tag_name;
```

## Configuration Drift

### Compare Configuration Files

```sql
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

### Track Configuration Changes

```sql
SELECT
    l.commit_hash,
    l.author_date,
    l.message,
    r.text as config_content
FROM git_log() l,
     LATERAL git_read_each(git_uri('.', 'config.json', l.commit_hash)) r
ORDER BY l.author_date DESC
LIMIT 20;
```

## Data Quality Monitoring

### Compare Data Quality Metrics

```sql
WITH current AS (
    SELECT
        COUNT(*) as total_rows,
        COUNT(*) FILTER (WHERE amount IS NULL) as null_amounts,
        COUNT(DISTINCT customer_id) as unique_customers
    FROM read_csv('git://data/orders.csv@HEAD')
),
previous AS (
    SELECT
        COUNT(*) as total_rows,
        COUNT(*) FILTER (WHERE amount IS NULL) as null_amounts,
        COUNT(DISTINCT customer_id) as unique_customers
    FROM read_csv('git://data/orders.csv@HEAD~1')
)
SELECT
    current.total_rows - previous.total_rows as rows_delta,
    current.null_amounts - previous.null_amounts as null_delta,
    current.unique_customers - previous.unique_customers as customers_delta
FROM current, previous;
```

### Find When Data Issue Was Introduced

```sql
SELECT
    l.commit_hash,
    l.author_date,
    l.message,
    COUNT(*) FILTER (WHERE amount < 0) as negative_amounts
FROM git_log() l,
     LATERAL (
         SELECT amount
         FROM read_csv(git_uri('.', 'data/transactions.csv', l.commit_hash))
     ) d
GROUP BY l.commit_hash, l.author_date, l.message
HAVING COUNT(*) FILTER (WHERE amount < 0) > 0
ORDER BY l.author_date DESC;
```

## Time-Series Analysis

### Build Historical Dataset

```sql
CREATE TABLE historical_metrics AS
SELECT
    l.author_date::DATE as snapshot_date,
    (SELECT SUM(value) FROM read_csv(git_uri('.', 'metrics.csv', l.commit_hash))) as total_value
FROM git_log() l
WHERE l.author_date > '2024-01-01'
ORDER BY l.author_date;
```

### Rolling Comparison

```sql
WITH daily_metrics AS (
    SELECT
        l.author_date::DATE as date,
        (SELECT COUNT(*) FROM read_csv(git_uri('.', 'data.csv', l.commit_hash))) as count
    FROM git_log() l
    WHERE l.author_date > current_date - interval '30 days'
)
SELECT
    date,
    count,
    LAG(count) OVER (ORDER BY date) as prev_count,
    count - LAG(count) OVER (ORDER BY date) as daily_change
FROM daily_metrics
ORDER BY date;
```

## Disaster Recovery

### Find Last Known Good Version

```sql
SELECT
    l.commit_hash,
    l.author_date,
    l.message
FROM git_log() l,
     LATERAL (
         SELECT COUNT(*) as cnt
         FROM read_csv(git_uri('.', 'data.csv', l.commit_hash))
     ) d
WHERE d.cnt > 1000  -- Expected minimum record count
ORDER BY l.author_date DESC
LIMIT 1;
```

### Restore Deleted Data

```sql
-- Find when file was deleted
SELECT
    l.commit_hash,
    l.author_date,
    l.message
FROM git_log() l
WHERE EXISTS (
    SELECT 1 FROM git_tree_each('.', l.commit_hash || '^') t
    WHERE t.file_path = 'deleted-data.csv'
)
AND NOT EXISTS (
    SELECT 1 FROM git_tree_each('.', l.commit_hash) t
    WHERE t.file_path = 'deleted-data.csv'
)
LIMIT 1;

-- Read the deleted file from before deletion
SELECT * FROM read_csv('git://deleted-data.csv@<commit-before-deletion>');
```

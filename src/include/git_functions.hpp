#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include <git2.h>
#include "git_path.hpp"

namespace duckdb {

// Git log table function - bind data (read-only, shared across threads)
struct GitLogFunctionData : public TableFunctionData {
    explicit GitLogFunctionData(const string &repo_path, const string &resolved_repo_path);
    explicit GitLogFunctionData(const string &ref);  // For LATERAL functions

    string repo_path;           // Original input path
    string resolved_repo_path;  // Absolute path to repository
    string ref;                 // Reference for LATERAL functions
    string file_path;           // File path for filtering (from git URI)
};

void GitLogFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
unique_ptr<FunctionData> GitLogBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<FunctionData> GitLogEachBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitLogInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Git log row structure for LATERAL processing
struct GitLogRow {
    string repo_path;
    string commit_hash;
    string author_name;
    string author_email;
    string committer_name;
    string committer_email;
    timestamp_t author_date;
    timestamp_t commit_date;
    string message;
    uint32_t parent_count;
    string tree_hash;

    // Constructor to ensure proper initialization
    GitLogRow() :
        repo_path(""), commit_hash(""), author_name(""), author_email(""),
        committer_name(""), committer_email(""),
        author_date(timestamp_t(0)), commit_date(timestamp_t(0)),
        message(""), parent_count(0), tree_hash("") {}
};

// Local state for git_log (per-thread resources)
struct GitLogLocalState : public LocalTableFunctionState {
    git_repository *repo = nullptr;
    git_revwalk *walker = nullptr;
    bool initialized = false;

    // For LATERAL functions: cache repository for repeated paths
    string cached_repo_path;
    git_repository *cached_repo = nullptr;

    // LATERAL processing state
    vector<GitLogRow> current_rows;
    idx_t current_input_row = 0;
    idx_t current_output_row = 0;
    bool initialized_row = false;

    ~GitLogLocalState() {
        if (walker) {
            git_revwalk_free(walker);
        }
        if (repo) {
            git_repository_free(repo);
        }
        if (cached_repo) {
            git_repository_free(cached_repo);
        }
    }
};

// Local init for git_log_each
unique_ptr<LocalTableFunctionState> GitLogLocalInit(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state);

// Git branches row structure for LATERAL processing
struct GitBranchesRow {
    string repo_path;
    string branch_name;
    string commit_hash;
    bool is_current;
    bool is_remote;

    // Constructor to ensure proper initialization
    GitBranchesRow() :
        repo_path(""), branch_name(""), commit_hash(""),
        is_current(false), is_remote(false) {}
};

// Git branches table function - bind data (read-only, shared across threads)
struct GitBranchesFunctionData : public TableFunctionData {
    explicit GitBranchesFunctionData(const string &repo_path, const string &resolved_repo_path);
    explicit GitBranchesFunctionData(const string &ref);  // For LATERAL functions

    string repo_path;           // Original input path
    string resolved_repo_path;  // Absolute path to repository
    string ref;                 // Reference for LATERAL functions
};

// Local state for git_branches (per-thread resources)
struct GitBranchesLocalState : public LocalTableFunctionState {
    git_repository *repo = nullptr;
    git_branch_iterator *iterator = nullptr;
    bool initialized = false;

    // For LATERAL functions: cache repository for repeated paths
    string cached_repo_path;
    git_repository *cached_repo = nullptr;

    // LATERAL processing state
    vector<GitBranchesRow> current_rows;
    idx_t current_input_row = 0;
    idx_t current_output_row = 0;
    bool initialized_row = false;

    ~GitBranchesLocalState() {
        if (iterator) {
            git_branch_iterator_free(iterator);
        }
        if (repo) {
            git_repository_free(repo);
        }
        if (cached_repo) {
            git_repository_free(cached_repo);
        }
    }
};

// Local init for git_branches_each
unique_ptr<LocalTableFunctionState> GitBranchesLocalInit(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state);

void GitBranchesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
unique_ptr<FunctionData> GitBranchesBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<FunctionData> GitBranchesEachBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitBranchesInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Git tags row structure for LATERAL processing
struct GitTagsRow {
    string repo_path;
    string tag_name;
    string commit_hash;
    string tag_hash;
    string tagger_name;
    timestamp_t tagger_date;
    string message;
    bool is_annotated;

    // Constructor to ensure proper initialization
    GitTagsRow() :
        repo_path(""), tag_name(""), commit_hash(""), tag_hash(""),
        tagger_name(""), tagger_date(timestamp_t(0)), message(""), is_annotated(false) {}
};

// Git tags table function - bind data (read-only, shared across threads)
struct GitTagsFunctionData : public TableFunctionData {
    explicit GitTagsFunctionData(const string &repo_path, const string &resolved_repo_path);
    explicit GitTagsFunctionData(const string &ref);  // For LATERAL functions

    string repo_path;           // Original input path
    string resolved_repo_path;  // Absolute path to repository
    string ref;                 // Reference for LATERAL functions
};

// Local state for git_tags (per-thread resources)
struct GitTagsLocalState : public LocalTableFunctionState {
    git_repository *repo = nullptr;
    vector<string> tag_names;
    idx_t current_index = 0;
    bool initialized = false;

    // For LATERAL functions: cache repository for repeated paths
    string cached_repo_path;
    git_repository *cached_repo = nullptr;

    // LATERAL processing state
    vector<GitTagsRow> current_rows;
    idx_t current_input_row = 0;
    idx_t current_output_row = 0;
    bool initialized_row = false;

    ~GitTagsLocalState() {
        if (repo) {
            git_repository_free(repo);
        }
        if (cached_repo) {
            git_repository_free(cached_repo);
        }
    }
};

// Local init for git_tags_each
unique_ptr<LocalTableFunctionState> GitTagsLocalInit(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state);

void GitTagsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
unique_ptr<FunctionData> GitTagsBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<FunctionData> GitTagsEachBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitTagsInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Git tree operation modes
enum class GitTreeMode {
    SINGLE,    // Single commit (static or dynamic)
    RANGE      // Commit range (e.g., HEAD~10..HEAD)
};

// Git tree table function
struct GitTreeFunctionData : public TableFunctionData {
    explicit GitTreeFunctionData(const string &ref, const string &repo_path);
    explicit GitTreeFunctionData(const string &range, const string &repo_path, bool is_range);
    explicit GitTreeFunctionData(const string &ref, const string &repo_path, const string &requested_path);
    explicit GitTreeFunctionData(const string &range, const string &repo_path, bool is_range, const string &requested_path);

    GitTreeMode mode;
    string ref;                    // For single commit mode
    string commit_range;           // For range mode
    string repo_path;
    string requested_path;         // Requested subpath within repo (may be empty)
    vector<struct GitTreeRow> rows; // Read-only after bind (thread-safe)
    bool is_dynamic;               // True if parameter comes from LATERAL
};

struct GitTreeRow {
    string git_uri;           // Renamed from git_file_uri - complete git:// URI
    string repo_path;         // NEW - repository filesystem path
    string commit_hash;       // Git commit hash
    string tree_hash;         // NEW - Git tree hash containing the file
    string file_path;         // File path within repository (extracted from URI)
    string file_ext;          // File extension (e.g., .js, .cpp, .md)
    string ref;              // Git reference (SHA/branch/tag)
    string blob_hash;         // Git blob hash of file content
    timestamp_t commit_date;  // Commit timestamp
    int32_t mode;            // File mode
    int64_t size_bytes;      // Renamed from size - file size in bytes
    string kind;             // NEW - object kind (blob, tree, etc.)
    bool is_text;            // NEW - whether content is text
    string encoding;         // NEW - text encoding (utf8, binary)
    // REMOVED: string path; // Redundant with file_path
};

// Local state for git_tree (per-thread iteration state)
struct GitTreeLocalState : public LocalTableFunctionState {
    idx_t current_index = 0;  // Per-thread iteration through bind_data.rows

    // LATERAL processing state
    vector<GitTreeRow> current_rows;
    idx_t current_input_row = 0;
    idx_t current_output_row = 0;
    bool initialized_row = false;
};

// Local init for git_tree_each
unique_ptr<LocalTableFunctionState> GitTreeLocalInit(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state);

void GitTreeFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
unique_ptr<FunctionData> GitTreeBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<FunctionData> GitTreeEachBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitTreeInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Git parents table function
struct GitParentsFunctionData : public TableFunctionData {
    explicit GitParentsFunctionData(const string &ref, const string &repo_path, bool all_refs);
    explicit GitParentsFunctionData(const vector<string> &commits, const string &repo_path, bool all_refs);

    string ref;                    // For single commit mode
    vector<string> commits;        // For array mode
    string repo_path;
    bool all_refs;
    bool is_array_mode;
    vector<struct GitParentsRow> rows; // Read-only after bind (thread-safe)
};

struct GitParentsRow {
    string commit_hash;
    string parent_hash;
    int32_t parent_index;
};

void GitParentsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);

// Local state for git_parents (per-thread iteration state)
struct GitParentsLocalState : public LocalTableFunctionState {
    idx_t current_index = 0;  // Per-thread iteration through bind_data.rows

    // LATERAL processing state
    vector<GitParentsRow> current_rows;
    idx_t current_input_row = 0;
    idx_t current_output_row = 0;
    bool initialized_row = false;
    string current_repo_path;  // Store the repo path for the current row being processed
};

// Bind data for git_parents_each (simpler than regular git_parents)
struct GitParentsEachBindData : public TableFunctionData {
    string repo_path;  // Bind-time repository path
    string ref;        // Fallback ref for GitContextManager
};

// Function declarations for git_parents_each
unique_ptr<FunctionData> GitParentsEachBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<LocalTableFunctionState> GitParentsLocalInit(ExecutionContext &context, TableFunctionInitInput &input,
                                                       GlobalTableFunctionState *global_state);
OperatorResultType GitParentsEachFunction(ExecutionContext &context, TableFunctionInput &data_p,
                                         DataChunk &input, DataChunk &output);
unique_ptr<FunctionData> GitParentsBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitParentsInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Registration functions
void RegisterGitLogFunction(ExtensionLoader &loader);
void RegisterGitBranchesFunction(ExtensionLoader &loader);
void RegisterGitTagsFunction(ExtensionLoader &loader);
void RegisterGitTreeFunction(ExtensionLoader &loader);
void RegisterGitParentsFunction(ExtensionLoader &loader);
void RegisterGitReadFunction(ExtensionLoader &loader);
void RegisterGitUriFunction(ExtensionLoader &loader);
void RegisterGitFunctions(ExtensionLoader &loader);

} // namespace duckdb

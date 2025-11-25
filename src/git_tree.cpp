#include "duckdb.hpp"
#include "git_functions.hpp"
#include "git_filesystem.hpp"
#include "git_path.hpp"
#include "git_context_manager.hpp"
#include "git_utils.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/common/exception.hpp"

#include <git2.h>
#include <algorithm>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Helper Functions for git_tree
//===--------------------------------------------------------------------===//

// Schema definition for git_tree
static void DefineGitTreeSchema(vector<LogicalType> &return_types, vector<string> &names) {
    return_types = {
        LogicalType::VARCHAR,    // git_uri (complete git:// URI)
        LogicalType::VARCHAR,    // repo_path (repository filesystem path)
        LogicalType::VARCHAR,    // commit_hash (git commit hash)
        LogicalType::VARCHAR,    // tree_hash (git tree hash containing the file)
        LogicalType::VARCHAR,    // file_path (file path within repository)
        LogicalType::VARCHAR,    // file_ext (file extension)
        LogicalType::VARCHAR,    // ref (git reference)
        LogicalType::VARCHAR,    // blob_hash (git blob hash of file content)
        LogicalType::TIMESTAMP,  // commit_date (commit timestamp)
        LogicalType::INTEGER,    // mode (file mode)
        LogicalType::BIGINT,     // size_bytes (file size in bytes)
        LogicalType::VARCHAR,    // kind (object kind: blob, tree, etc.)
        LogicalType::BOOLEAN,    // is_text (whether content is text)
        LogicalType::VARCHAR     // encoding (text encoding: utf8, binary)
    };
    names = {"git_uri", "repo_path", "commit_hash", "tree_hash", "file_path", "file_ext", 
             "ref", "blob_hash", "commit_date", "mode", "size_bytes", "kind", "is_text", "encoding"};
}

// Helper to get schema return types
static vector<LogicalType> GetGitTreeSchema() {
    vector<LogicalType> return_types;
    vector<string> names;
    DefineGitTreeSchema(return_types, names);
    return return_types;
}

// Helper to get schema column names
static vector<string> GetGitTreeColumnNames() {
    vector<LogicalType> return_types;
    vector<string> names;
    DefineGitTreeSchema(return_types, names);
    return names;
}

// Output helper for git_tree rows
static void OutputGitTreeRow(DataChunk &output, const GitTreeRow &row, idx_t row_idx) {
    output.SetValue(0, row_idx, Value(row.git_uri));
    output.SetValue(1, row_idx, Value(row.repo_path));
    output.SetValue(2, row_idx, Value(row.commit_hash));
    output.SetValue(3, row_idx, Value(row.tree_hash));
    output.SetValue(4, row_idx, Value(row.file_path));
    output.SetValue(5, row_idx, Value(row.file_ext));
    output.SetValue(6, row_idx, Value(row.ref));
    if (row.kind == "file") { 
        output.SetValue(7, row_idx, Value(row.blob_hash)); 
    } else { 
        output.SetValue(7, row_idx, Value()); // NULL for non-file entries
    }
    output.SetValue(8, row_idx, Value::TIMESTAMP(row.commit_date));
    output.SetValue(9, row_idx, Value::INTEGER(row.mode));
    output.SetValue(10, row_idx, Value::BIGINT(row.size_bytes));
    output.SetValue(11, row_idx, Value(row.kind));
    output.SetValue(12, row_idx, Value::BOOLEAN(row.is_text));
    output.SetValue(13, row_idx, Value(row.encoding));
}

//===--------------------------------------------------------------------===//
// GitTreeFunctionData Constructor Implementations
//===--------------------------------------------------------------------===//

GitTreeFunctionData::GitTreeFunctionData(const string &ref, const string &repo_path)
    : mode(GitTreeMode::SINGLE), ref(ref), repo_path(repo_path), is_dynamic(false) {
}

GitTreeFunctionData::GitTreeFunctionData(const string &range, const string &repo_path, bool is_range)
    : mode(GitTreeMode::RANGE), commit_range(range), repo_path(repo_path), is_dynamic(false) {
}

GitTreeFunctionData::GitTreeFunctionData(const string &ref, const string &repo_path, const string &requested_path)
    : mode(GitTreeMode::SINGLE), ref(ref), repo_path(repo_path), requested_path(requested_path), is_dynamic(false) {
}

GitTreeFunctionData::GitTreeFunctionData(const string &range, const string &repo_path, bool is_range, const string &requested_path)
    : mode(GitTreeMode::RANGE), commit_range(range), repo_path(repo_path), requested_path(requested_path), is_dynamic(false) {
}

//===--------------------------------------------------------------------===//
// Helper Functions (copied from git_functions.cpp)
//===--------------------------------------------------------------------===//

static string oid_to_hex(const git_oid *oid) {
    char hex[GIT_OID_HEXSZ + 1];
    git_oid_tostr(hex, sizeof(hex), oid);
    return string(hex);
}

static string BuildGitFileUri(const string &repo_path, const string &file_path, const string &commit_hash) {
    if (file_path.empty()) {
        return "git://" + repo_path + "@" + commit_hash;
    }
    return "git://" + repo_path + "/" + file_path + "@" + commit_hash;
}

static string ExtractFileExtension(const string &path) {
    size_t dot_pos = path.find_last_of('.');
    if (dot_pos == string::npos || dot_pos == path.length() - 1) {
        return "";
    }
    return path.substr(dot_pos);
}

// NormalizeRepoPathSpec is now defined in git_path.cpp

// Helper functions for emitting different row types
static inline void EmitTreeRow(vector<GitTreeRow> &out, const string &repo_path, const string &commit_hash,
                               const string &containing_tree_hash, const string &path, timestamp_t commit_date, int32_t mode) {
    GitTreeRow row;
    row.git_uri = BuildGitFileUri(repo_path, path, commit_hash);
    row.repo_path = repo_path;
    row.commit_hash = commit_hash;
    row.tree_hash = containing_tree_hash;
    row.file_path = path;
    row.file_ext = ExtractFileExtension(path);
    row.ref = commit_hash;
    row.blob_hash = "0000000000000000000000000000000000000000";
    row.commit_date = commit_date;
    row.mode = mode;
    row.size_bytes = 0;
    row.kind = "tree";
    row.is_text = false;
    row.encoding = "unknown";
    out.push_back(std::move(row));
}

static inline void EmitSubmoduleRow(vector<GitTreeRow> &out, const string &repo_path, const string &commit_hash,
                                    const string &containing_tree_hash, const string &path, timestamp_t commit_date, int32_t mode) {
    GitTreeRow row;
    row.git_uri = BuildGitFileUri(repo_path, path, commit_hash);
    row.repo_path = repo_path;
    row.commit_hash = commit_hash;
    row.tree_hash = containing_tree_hash;
    row.file_path = path;
    row.file_ext = ExtractFileExtension(path);
    row.ref = commit_hash;
    row.blob_hash = string();
    row.commit_date = commit_date;
    row.mode = mode;
    row.size_bytes = 0;
    row.kind = "submodule";
    row.is_text = false;
    row.encoding = "unknown";
    out.push_back(std::move(row));
}

static inline void EmitFileRow(vector<GitTreeRow> &out, const string &repo_path, const string &commit_hash,
                               const string &containing_tree_hash, const string &path, timestamp_t commit_date,
                               int32_t mode, git_repository *repo, const git_oid *blob_oid) {
    int64_t size_bytes = 0;
    bool is_text = false;
    string encoding = "unknown";
    git_blob *blob = nullptr;
    if (blob_oid && git_blob_lookup(&blob, repo, blob_oid) == 0 && blob) {
        size_bytes = static_cast<int64_t>(git_blob_rawsize(blob));
        is_text = !git_blob_is_binary(blob);
        encoding = is_text ? "utf8" : "binary";
        git_blob_free(blob);
    }
    GitTreeRow row;
    row.git_uri = BuildGitFileUri(repo_path, path, commit_hash);
    row.repo_path = repo_path;
    row.commit_hash = commit_hash;
    row.tree_hash = containing_tree_hash;
    row.file_path = path;
    row.file_ext = ExtractFileExtension(path);
    row.ref = commit_hash;
    row.blob_hash = blob_oid ? oid_to_hex(blob_oid) : string();
    row.commit_date = commit_date;
    row.mode = mode;
    row.size_bytes = size_bytes;
    row.kind = "file";
    row.is_text = is_text;
    row.encoding = encoding;
    out.push_back(std::move(row));
}

//===--------------------------------------------------------------------===//
// Tree Traversal Function (copied from git_functions.cpp)
//===--------------------------------------------------------------------===//

static void traverse_tree(git_repository *repo, git_tree *tree, const string &base, vector<GitTreeRow> &out, 
                         const string &commit_hash, timestamp_t commit_date, const string &repo_path) {
    const size_t count = git_tree_entrycount(tree);
    for (size_t i = 0; i < count; ++i) {
        const git_tree_entry *entry = git_tree_entry_byindex(tree, i);
        const char *name = git_tree_entry_name(entry);
        git_object_t type = git_tree_entry_type(entry);
        const git_oid *oid = git_tree_entry_id(entry);
        int32_t mode = git_tree_entry_filemode(entry);
        
        string path = base.empty() ? string(name) : base + "/" + string(name);
        
        if (type == GIT_OBJECT_BLOB) {
            string kind;
            switch (mode) {
                case GIT_FILEMODE_BLOB:
                case GIT_FILEMODE_BLOB_EXECUTABLE:
                    kind = "file";
                    break;
                case GIT_FILEMODE_LINK:
                    kind = "file";  // Changed from "symlink" to "file"
                    break;
                default:
                    kind = "file";
                    break;
            }
            
            // Get the tree containing this entry
            string tree_hash = oid_to_hex(git_tree_id(tree));
            
            // Create file row using the EmitFileRow helper
            EmitFileRow(out, repo_path, commit_hash, tree_hash, path, commit_date, mode, repo, oid);
            
        } else if (type == GIT_OBJECT_TREE) {
            string containing = oid_to_hex(git_tree_id(tree));
            EmitTreeRow(out, repo_path, commit_hash, containing, path, commit_date, mode);
            git_tree *subtree = nullptr;
            if (git_tree_lookup(&subtree, repo, oid) == 0 && subtree) {
                traverse_tree(repo, subtree, path, out, commit_hash, commit_date, repo_path);
                git_tree_free(subtree);
            }
        } else if (type == GIT_OBJECT_COMMIT) {
            string containing = oid_to_hex(git_tree_id(tree));
            EmitSubmoduleRow(out, repo_path, commit_hash, containing, path, commit_date, mode);
        }
    }
}

//===--------------------------------------------------------------------===//
// ProcessSingleCommit Function (copied from git_functions.cpp)
//===--------------------------------------------------------------------===//

static void ProcessSingleCommit(git_repository *repo, const string &ref, const string &repo_path, const string &requested_path,
                               vector<GitTreeRow> &rows) {
    git_object *obj = nullptr;
    
    // Use the validated ref from GitContextManager
    if (git_revparse_single(&obj, repo, ref.c_str()) != 0) {
        throw IOException("Unable to parse ref '%s': %s", ref, "unable to parse OID");
    }
    
    git_commit *commit = nullptr;
    if (git_object_peel((git_object **)&commit, obj, GIT_OBJECT_COMMIT) != 0) {
        git_object_free(obj);
        throw BinderException("git_tree: failed to get commit for ref '%s' in repository '%s'", ref, repo_path);
    }
    
    string commit_hash = oid_to_hex(git_commit_id(commit));
    timestamp_t commit_date = Timestamp::FromEpochSeconds(git_commit_time(commit));
    
    git_tree *tree = nullptr;
    if (git_commit_tree(&tree, commit) != 0) {
        git_commit_free(commit);
        git_object_free(obj);
        throw BinderException("git_tree: failed to get tree for commit '%s' in repository '%s'", commit_hash, repo_path);
    }
    
    // Path filtering logic
    if (requested_path.empty()) {
        traverse_tree(repo, tree, "", rows, commit_hash, commit_date, repo_path);
    } else {
        string norm = NormalizeRepoPathSpec(requested_path);
        if (norm.empty()) {
            traverse_tree(repo, tree, "", rows, commit_hash, commit_date, repo_path);
        } else {
            git_tree_entry *path_entry = nullptr;
            int error = git_tree_entry_bypath(&path_entry, tree, norm.c_str());
            if (error == 0 && path_entry) {
                git_object_t etype = git_tree_entry_type(path_entry);
                int32_t mode = git_tree_entry_filemode(path_entry);
                const git_oid *eoid = git_tree_entry_id(path_entry);
                string parent_tree_hash = oid_to_hex(git_tree_id(tree));
                if (etype == GIT_OBJECT_TREE) {
                    EmitTreeRow(rows, repo_path, commit_hash, parent_tree_hash, norm, commit_date, mode);
                    git_tree *subtree = nullptr;
                    if (git_tree_lookup(&subtree, repo, eoid) == 0 && subtree) {
                        traverse_tree(repo, subtree, norm, rows, commit_hash, commit_date, repo_path);
                        git_tree_free(subtree);
                    }
                } else if (etype == GIT_OBJECT_BLOB) {
                    EmitFileRow(rows, repo_path, commit_hash, parent_tree_hash, norm, commit_date, mode, repo, eoid);
                } else if (etype == GIT_OBJECT_COMMIT) {
                    EmitSubmoduleRow(rows, repo_path, commit_hash, parent_tree_hash, norm, commit_date, mode);
                }
                git_tree_entry_free(path_entry);
            }
        }
    }
    
    git_tree_free(tree);
    git_commit_free(commit);
    git_object_free(obj);
}

//===--------------------------------------------------------------------===//
// Git Tree Bind Functions (copied from git_functions.cpp)
//===--------------------------------------------------------------------===//

unique_ptr<FunctionData> GitTreeBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
    
    // Set return schema first
    return_types = GetGitTreeSchema();
    names = GetGitTreeColumnNames();
    
    // Parse parameters using unified parameter parsing 
    auto params = ParseUnifiedGitParams(input, 1);
    
    // Use GitContextManager to process the URI and validate the reference
    string fallback_ref = params.ref.empty() ? "HEAD" : params.ref;
    
    try {
        auto ctx = GitContextManager::Instance().ProcessGitUri(params.repo_path_or_uri, fallback_ref);
        
        // Check if ref is a commit range (contains '..')
        if (ctx.final_ref.find("..") != string::npos) {
            return make_uniq<GitTreeFunctionData>(ctx.final_ref, ctx.repo_path, true, ctx.file_path);
        } else {
            return make_uniq<GitTreeFunctionData>(ctx.final_ref, ctx.repo_path, ctx.file_path);
        }
        
    } catch (const std::exception &e) {
        throw BinderException("git_tree: %s", e.what());
    }
}

unique_ptr<FunctionData> GitTreeEachBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
    
    // Set return schema
    return_types = GetGitTreeSchema();
    names = GetGitTreeColumnNames();
    
    // Parse parameters for LATERAL functions - ref defaults to "HEAD" 
    auto params = ParseLateralGitParams(input, 1);
    
    // For LATERAL functions, we create minimal bind data with the ref from parameters
    // The actual repository path will come from runtime DataChunk input
    return make_uniq<GitTreeFunctionData>(params.ref, ".");  // Use 2-parameter constructor
}

//===--------------------------------------------------------------------===//
// Git Tree Table Functions (copied from git_functions.cpp)
//===--------------------------------------------------------------------===//

unique_ptr<GlobalTableFunctionState> GitTreeInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind_data = const_cast<GitTreeFunctionData&>(input.bind_data->Cast<GitTreeFunctionData>());
    
    // Only process for non-dynamic (regular table function) mode
    if (!bind_data.is_dynamic && bind_data.mode == GitTreeMode::SINGLE) {
        // Open repository
        git_repository *repo = nullptr;
        int error = git_repository_open(&repo, bind_data.repo_path.c_str());
        if (error != 0) {
            const git_error *git_err = git_error_last();
            string error_msg = git_err ? git_err->message : "Unknown git error";
            throw BinderException("git_tree: failed to open repository '%s': %s", bind_data.repo_path, error_msg);
        }
        
        try {
            ProcessSingleCommit(repo, bind_data.ref, bind_data.repo_path, bind_data.requested_path, bind_data.rows);
        } catch (...) {
            git_repository_free(repo);
            throw;
        }
        
        git_repository_free(repo);
    } else if (!bind_data.is_dynamic && bind_data.mode == GitTreeMode::RANGE) {
        // Handle range mode - this would need more complex implementation
        throw BinderException("git_tree: Range mode not yet implemented");
    }
    
    return make_uniq<GlobalTableFunctionState>();
}

void GitTreeFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitTreeFunctionData>();
    auto &local_state = data_p.local_state->Cast<GitTreeLocalState>();

    idx_t output_count = 0;
    const idx_t max_output = STANDARD_VECTOR_SIZE;

    // Process rows from bind_data.rows using per-thread iteration state
    while (local_state.current_index < bind_data.rows.size() && output_count < max_output) {
        auto &row = bind_data.rows[local_state.current_index];
        OutputGitTreeRow(output, row, output_count);
        local_state.current_index++;
        output_count++;
    }

    output.SetCardinality(output_count);
}

//===--------------------------------------------------------------------===//
// Git Tree Each (LATERAL) Functions (copied from git_functions.cpp)
//===--------------------------------------------------------------------===//

unique_ptr<LocalTableFunctionState> GitTreeLocalInit(ExecutionContext &context,
                                                    TableFunctionInitInput &input, GlobalTableFunctionState *global_state) {
    // Create local state for LATERAL processing
    return make_uniq<GitTreeLocalState>();
}

static OperatorResultType GitTreeEachFunction(ExecutionContext &context, TableFunctionInput &data_p,
                                            DataChunk &input, DataChunk &output) {
    auto &state = data_p.local_state->Cast<GitTreeLocalState>();
    auto &bind_data = data_p.bind_data->Cast<GitTreeFunctionData>();
    
    string resolved_repo_path;
    
    // Process input rows one at a time with proper state management (following git_log_each pattern)
    while (true) {
        // Check if we need to initialize for the current input row
        if (!state.initialized_row) {
            // initialize for the current input row
            if (state.current_input_row >= input.size()) {
                // ran out of rows
                state.current_input_row = 0;
                state.initialized_row = false;
                return OperatorResultType::NEED_MORE_INPUT;
            }
            
            // LATERAL function: extract repo_path_or_uri from input DataChunk
            input.Flatten();
            
            // Check if input has columns and data
            if (input.ColumnCount() == 0) {
                // Skip problematic row and continue
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            // Check if the input is null
            if (FlatVector::IsNull(input.data[0], state.current_input_row)) {
                // Move to next input row if null
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            // Extract repo_path_or_uri from input DataChunk - direct string_t access
            auto data = FlatVector::GetData<string_t>(input.data[0]);
            if (!data) {
                // Skip problematic row and continue
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            // Get the string_t directly and convert to string
            auto string_t_value = data[state.current_input_row];
            string repo_path_or_uri(string_t_value.GetData(), string_t_value.GetSize());
            
            if (repo_path_or_uri.empty()) {
                // Skip problematic row and continue
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            // Apply unified parameter processing at runtime
            // Use GitContextManager for unified git URI processing and reference validation
            string resolved_file_path, final_ref;
            
            try {
                // GitContextManager handles both git:// URIs and filesystem paths
                // It also validates references and throws consistent "unable to parse OID" errors
                // Use bind_data.ref as fallback (defaults to "HEAD" from ParseLateralGitParams)
                auto ctx = GitContextManager::Instance().ProcessGitUri(repo_path_or_uri, bind_data.ref);
                resolved_repo_path = ctx.repo_path;
                resolved_file_path = ctx.file_path;
                final_ref = ctx.final_ref;
                
                // Note: ctx.repo and ctx.resolved_object are managed by GitContextManager
                // No need to free them here - GitContextManager handles caching and cleanup
            } catch (const std::exception &e) {
                // GitContextManager provides consistent error messages - skip problematic row and continue
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            // Process the git tree using the resolved parameters
            state.current_rows.clear();
            
            // Process this git tree request
            git_repository *repo = nullptr;
            int error = git_repository_open(&repo, resolved_repo_path.c_str());
            if (error != 0) {
                // Skip failed repositories in LATERAL context - just move to next input
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            try {
                ProcessSingleCommit(repo, final_ref, resolved_repo_path, resolved_file_path, state.current_rows);
            } catch (...) {
                git_repository_free(repo);
                // Skip failed processing in LATERAL context - just move to next input
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            git_repository_free(repo);
            
            state.initialized_row = true;
            state.current_output_row = 0;
        }
        
        // Output rows for current input
        idx_t output_count = 0;
        while (output_count < STANDARD_VECTOR_SIZE && 
               state.current_output_row < state.current_rows.size()) {
            
            auto &row = state.current_rows[state.current_output_row];
            OutputGitTreeRow(output, row, output_count);
            
            output_count++;
            state.current_output_row++;
        }
        
        output.SetCardinality(output_count);
        
        // Check if we're done with current input row
        if (state.current_output_row >= state.current_rows.size()) {
            state.current_input_row++;
            state.initialized_row = false;
        }
        
        return OperatorResultType::HAVE_MORE_OUTPUT;
    }
}

//===--------------------------------------------------------------------===//
// Git Tree Registration (copied from git_functions.cpp)
//===--------------------------------------------------------------------===//

void RegisterGitTreeFunction(ExtensionLoader &loader) {
    TableFunctionSet git_tree_set("git_tree");

    // Single parameter: git_tree(repo_path_or_uri)
    TableFunction git_tree_single({LogicalType::VARCHAR}, GitTreeFunction, GitTreeBind, GitTreeInitGlobal);
    git_tree_single.init_local = GitTreeLocalInit;
    git_tree_single.named_parameters["array"] = LogicalType::LIST(LogicalType::VARCHAR);
    git_tree_set.AddFunction(git_tree_single);

    // Two parameters: git_tree(repo_path_or_uri, ref)
    TableFunction git_tree_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, GitTreeFunction, GitTreeBind, GitTreeInitGlobal);
    git_tree_two.init_local = GitTreeLocalInit;
    git_tree_two.named_parameters["array"] = LogicalType::LIST(LogicalType::VARCHAR);
    git_tree_set.AddFunction(git_tree_two);

    // Array parameter: git_tree(array=['commit1', 'commit2'])
    TableFunction git_tree_array({LogicalType::LIST(LogicalType::VARCHAR)}, GitTreeFunction, GitTreeBind, GitTreeInitGlobal);
    git_tree_array.init_local = GitTreeLocalInit;
    git_tree_array.named_parameters["array"] = LogicalType::LIST(LogicalType::VARCHAR);
    git_tree_set.AddFunction(git_tree_array);

    // Zero parameters: git_tree() (uses current directory, HEAD)
    TableFunction git_tree_zero({}, GitTreeFunction, GitTreeBind, GitTreeInitGlobal);
    git_tree_zero.init_local = GitTreeLocalInit;
    git_tree_zero.named_parameters["array"] = LogicalType::LIST(LogicalType::VARCHAR);
    git_tree_set.AddFunction(git_tree_zero);

    loader.RegisterFunction( git_tree_set);
    
    // LATERAL git_tree_each function (commit ref comes from LATERAL context) - ONLY for dynamic input
    TableFunctionSet git_tree_each_set("git_tree_each");
    
    // Single parameter version
    TableFunction git_tree_each_single({LogicalType::VARCHAR}, nullptr, GitTreeEachBind, nullptr, GitTreeLocalInit);
    git_tree_each_single.in_out_function = GitTreeEachFunction;
    git_tree_each_single.named_parameters["array"] = LogicalType::LIST(LogicalType::VARCHAR);
    git_tree_each_set.AddFunction(git_tree_each_single);
    
    // Two parameter version  
    TableFunction git_tree_each_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, GitTreeEachBind, nullptr, GitTreeLocalInit);
    git_tree_each_two.in_out_function = GitTreeEachFunction;
    git_tree_each_two.named_parameters["array"] = LogicalType::LIST(LogicalType::VARCHAR);
    git_tree_each_set.AddFunction(git_tree_each_two);
    
    loader.RegisterFunction( git_tree_each_set);
}

} // namespace duckdb
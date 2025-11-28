#include "git_functions.hpp"
#include "git_filesystem.hpp"
#include "git_utils.hpp"
#include "git_context_manager.hpp"
#include "git_history.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Git Log Function
//===--------------------------------------------------------------------===//

GitLogFunctionData::GitLogFunctionData(const string &repo_path, const string &resolved_repo_path)
    : repo_path(repo_path), resolved_repo_path(resolved_repo_path) {
}

GitLogFunctionData::GitLogFunctionData(const string &ref)
    : repo_path(""), resolved_repo_path(""), ref(ref) {
}

unique_ptr<FunctionData> GitLogBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<string> &names) {
    
    // Use unified parameter parsing to support both git:// URIs and filesystem paths
    auto params = ParseUnifiedGitParams(input, 1);  // ref parameter at index 1 (optional)
    
    // Define return schema with repo_path as first column
    return_types = {
        LogicalType::VARCHAR,    // repo_path
        LogicalType::VARCHAR,    // commit_hash
        LogicalType::VARCHAR,    // author_name  
        LogicalType::VARCHAR,    // author_email
        LogicalType::VARCHAR,    // committer_name
        LogicalType::VARCHAR,    // committer_email
        LogicalType::TIMESTAMP,  // author_date
        LogicalType::TIMESTAMP,  // commit_date
        LogicalType::VARCHAR,    // message
        LogicalType::INTEGER,    // parent_count
        LogicalType::VARCHAR     // tree_hash
    };
    
    names = {
        "repo_path", "commit_hash", "author_name", "author_email", "committer_name", "committer_email",
        "author_date", "commit_date", "message", "parent_count", "tree_hash"
    };
    
    try {
        auto ctx = GitContextManager::Instance().ProcessGitUri(params.repo_path_or_uri, params.ref);
        auto result = make_uniq<GitLogFunctionData>(params.repo_path_or_uri, ctx.repo_path);
        result->file_path = ctx.file_path;
        return std::move(result);
    } catch (const std::exception &e) {
        throw BinderException("git_log: %s", e.what());
    }
}

unique_ptr<GlobalTableFunctionState> GitLogInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<GlobalTableFunctionState>();
}

unique_ptr<LocalTableFunctionState> GitLogLocalInit(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state) {
    return make_uniq<GitLogLocalState>();
}

void GitLogFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitLogFunctionData>();
    auto &local_state = data_p.local_state->Cast<GitLogLocalState>();

    if (!local_state.initialized) {
        // Open repository in this thread's local state
        int error = git_repository_open(&local_state.repo, bind_data.resolved_repo_path.c_str());
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to open git repository '%s': %s",
                            bind_data.repo_path, e ? e->message : "Unknown error");
        }

        // Create revwalk
        error = git_revwalk_new(&local_state.walker, local_state.repo);
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to create revwalk: %s", e ? e->message : "Unknown error");
        }

        // Push HEAD
        error = git_revwalk_push_head(local_state.walker);
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to push HEAD: %s", e ? e->message : "Unknown error");
        }

        local_state.initialized = true;
    }

    idx_t count = 0;
    git_oid oid;

    while (count < STANDARD_VECTOR_SIZE && git_revwalk_next(&oid, local_state.walker) == 0) {
        git_commit *commit = nullptr;
        int error = git_commit_lookup(&commit, local_state.repo, &oid);
        if (error != 0) {
            continue; // Skip invalid commits
        }

        // File path filtering: if a specific file is requested, only include commits that modified it
        if (!bind_data.file_path.empty()) {
            if (!FileChangedInCommit(local_state.repo, commit, bind_data.file_path)) {
                git_commit_free(commit);
                continue;
            }
        }

        // Set repo_path as first column
        output.SetValue(0, count, Value(bind_data.repo_path));

        // Get commit hash
        char hash_str[GIT_OID_HEXSZ + 1];
        git_oid_tostr(hash_str, sizeof(hash_str), &oid);
        output.SetValue(1, count, Value(hash_str));

        // Get author info
        const git_signature *author = git_commit_author(commit);
        output.SetValue(2, count, Value(author->name ? author->name : ""));
        output.SetValue(3, count, Value(author->email ? author->email : ""));

        // Get committer info
        const git_signature *committer = git_commit_committer(commit);
        output.SetValue(4, count, Value(committer->name ? committer->name : ""));
        output.SetValue(5, count, Value(committer->email ? committer->email : ""));

        // Get timestamps
        timestamp_t author_ts = Timestamp::FromEpochSeconds(author->when.time);
        timestamp_t commit_ts = Timestamp::FromEpochSeconds(committer->when.time);
        output.SetValue(6, count, Value::TIMESTAMP(author_ts));
        output.SetValue(7, count, Value::TIMESTAMP(commit_ts));

        // Get commit message
        const char *message = git_commit_message(commit);
        output.SetValue(8, count, Value(message ? message : ""));

        // Get parent count
        unsigned int parent_count = git_commit_parentcount(commit);
        output.SetValue(9, count, Value::INTEGER(parent_count));

        // Get tree hash
        const git_oid *tree_oid = git_commit_tree_id(commit);
        char tree_hash[GIT_OID_HEXSZ + 1];
        git_oid_tostr(tree_hash, sizeof(tree_hash), tree_oid);
        output.SetValue(10, count, Value(tree_hash));

        git_commit_free(commit);
        count++;
    }

    output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// LATERAL Functions Support
//===--------------------------------------------------------------------===//

// Helper function for LATERAL git_log_each processing - processes entire git log for a repository path
// Uses cached repository from local_state for performance
static void ProcessLogCommitForInOut(git_repository *repo, const string &resolved_repo_path, const string &ref,
                                     vector<GitLogRow> &rows, const string &file_path = "") {
    // repo is from the per-thread cache in GitLogLocalState

    // Create revwalk
    git_revwalk *walker = nullptr;
    int error = git_revwalk_new(&walker, repo);
    if (error != 0) {
        const git_error *e = git_error_last();
        throw IOException("Failed to create revwalk: %s", e ? e->message : "Unknown error");
    }

    // Push the specified ref (already validated by GitContextManager)
    git_object *ref_obj = nullptr;
    error = git_revparse_single(&ref_obj, repo, ref.c_str());
    if (error != 0) {
        git_revwalk_free(walker);
        const git_error *e = git_error_last();
        throw IOException("Unable to parse ref '%s': %s", ref, e ? e->message : "unable to parse OID");
    }

    const git_oid *ref_oid = git_object_id(ref_obj);
    error = git_revwalk_push(walker, ref_oid);
    git_object_free(ref_obj);
    if (error != 0) {
        git_revwalk_free(walker);
        const git_error *e = git_error_last();
        throw IOException("Failed to push ref '%s': %s", ref, e ? e->message : "Unknown error");
    }

    // Walk commits
    git_oid commit_oid;
    while (git_revwalk_next(&commit_oid, walker) == 0) {
        git_commit *commit = nullptr;
        error = git_commit_lookup(&commit, repo, &commit_oid);
        if (error != 0) {
            continue; // Skip invalid commits
        }

        GitLogRow row;
        row.repo_path = resolved_repo_path;

        // Get commit hash
        char hash_str[GIT_OID_HEXSZ + 1];
        git_oid_tostr(hash_str, sizeof(hash_str), &commit_oid);
        row.commit_hash = hash_str;

        // Get author info
        const git_signature *author = git_commit_author(commit);
        row.author_name = author->name ? author->name : "";
        row.author_email = author->email ? author->email : "";

        // Get committer info
        const git_signature *committer = git_commit_committer(commit);
        row.committer_name = committer->name ? committer->name : "";
        row.committer_email = committer->email ? committer->email : "";

        // Get timestamps
        row.author_date = Timestamp::FromEpochSeconds(author->when.time);
        row.commit_date = Timestamp::FromEpochSeconds(committer->when.time);

        // Get commit message
        const char *message = git_commit_message(commit);
        row.message = message ? message : "";

        // Get parent count
        row.parent_count = git_commit_parentcount(commit);

        // Get tree hash
        const git_oid *tree_oid = git_commit_tree_id(commit);
        char tree_hash[GIT_OID_HEXSZ + 1];
        git_oid_tostr(tree_hash, sizeof(tree_hash), tree_oid);
        row.tree_hash = tree_hash;

        // File path filtering: if a specific file is requested, only include commits that modified it
        if (!file_path.empty()) {
            if (!FileChangedInCommit(repo, commit, file_path)) {
                git_commit_free(commit);
                continue;
            }
        }

        rows.push_back(row);
        git_commit_free(commit);
    }

    git_revwalk_free(walker);
    // Note: Do NOT free repo here - it's cached in local_state
}

// LATERAL git_log_each function - processes dynamic commit refs
static OperatorResultType GitLogEachFunction(ExecutionContext &context, TableFunctionInput &data_p, 
                                           DataChunk &input, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitLogFunctionData>();
    auto &state = data_p.local_state->Cast<GitLogLocalState>();
    
    // Declare resolved_repo_path at function scope so it's accessible in output loop
    string resolved_repo_path;
    
    while (true) {
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
                throw BinderException("git_log_each: no input columns available");
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
                throw BinderException("git_log_each: no string data in input column");
            }
            
            // Get the string_t directly and convert to string
            auto string_t_value = data[state.current_input_row];
            string repo_path_or_uri(string_t_value.GetData(), string_t_value.GetSize());
            
            if (repo_path_or_uri.empty()) {
                throw BinderException("git_log_each: received empty repo_path_or_uri from input");
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
            } catch (const std::exception &e) {
                // GitContextManager provides consistent error messages
                throw BinderException("git_log_each: %s", e.what());
            }

            // Check if we can reuse cached repository (optimization for LATERAL joins)
            if (state.cached_repo_path != resolved_repo_path) {
                // Different repo - close old one if exists and open new one
                if (state.cached_repo) {
                    git_repository_free(state.cached_repo);
                    state.cached_repo = nullptr;
                }

                int error = git_repository_open(&state.cached_repo, resolved_repo_path.c_str());
                if (error != 0) {
                    const git_error *e = git_error_last();
                    throw IOException("Failed to open repository '%s': %s",
                                    resolved_repo_path, e ? e->message : "Unknown error");
                }

                state.cached_repo_path = resolved_repo_path;
            }

            // Process the git log using the cached repository
            state.current_rows.clear();

            ProcessLogCommitForInOut(state.cached_repo, resolved_repo_path, final_ref, state.current_rows, resolved_file_path);
            
            state.initialized_row = true;
            state.current_output_row = 0;
        }
        
        // Output rows for current input
        idx_t output_count = 0;
        while (output_count < STANDARD_VECTOR_SIZE && 
               state.current_output_row < state.current_rows.size()) {
            
            auto &row = state.current_rows[state.current_output_row];
            
            // Fill output row with git log data
            output.SetValue(0, output_count, Value(resolved_repo_path));
            output.SetValue(1, output_count, Value(row.commit_hash));
            output.SetValue(2, output_count, Value(row.author_name));  
            output.SetValue(3, output_count, Value(row.author_email));
            output.SetValue(4, output_count, Value(row.committer_name));
            output.SetValue(5, output_count, Value(row.committer_email));
            output.SetValue(6, output_count, Value::TIMESTAMP(row.author_date));
            output.SetValue(7, output_count, Value::TIMESTAMP(row.commit_date));
            output.SetValue(8, output_count, Value(row.message));
            output.SetValue(9, output_count, Value::INTEGER(row.parent_count));
            output.SetValue(10, output_count, Value(row.tree_hash));
            
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

// GitLogEachBind function - LATERAL function bind for dynamic repo_path processing
unique_ptr<FunctionData> GitLogEachBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
    
    // For LATERAL functions, use ParseLateralGitParams to get optional ref parameter
    // The repo_path will come from runtime DataChunk, not bind time
    auto params = ParseLateralGitParams(input, 1);  // ref parameter at index 1 (optional)
    
    // Define return schema with repo_path as first column
    return_types = {
        LogicalType::VARCHAR,    // repo_path
        LogicalType::VARCHAR,    // commit_hash
        LogicalType::VARCHAR,    // author_name  
        LogicalType::VARCHAR,    // author_email
        LogicalType::VARCHAR,    // committer_name
        LogicalType::VARCHAR,    // committer_email
        LogicalType::TIMESTAMP,  // author_date
        LogicalType::TIMESTAMP,  // commit_date
        LogicalType::VARCHAR,    // message
        LogicalType::INTEGER,    // parent_count
        LogicalType::VARCHAR     // tree_hash
    };
    
    names = {
        "repo_path", "commit_hash", "author_name", "author_email", "committer_name", "committer_email",
        "author_date", "commit_date", "message", "parent_count", "tree_hash"
    };
    
    // For LATERAL functions, store the ref parameter (defaults to "HEAD" from ParseLateralGitParams)
    return make_uniq<GitLogFunctionData>(params.ref);
}

void RegisterGitLogFunction(ExtensionLoader &loader) {
    // Single-argument version (existing)
    TableFunction git_log_func("git_log", {LogicalType::VARCHAR}, GitLogFunction, GitLogBind, GitLogInitGlobal);
    git_log_func.init_local = GitLogLocalInit;
    git_log_func.named_parameters["repo_path"] = LogicalType::VARCHAR;
    loader.RegisterFunction( git_log_func);

    // Zero-argument version (defaults to current directory)
    TableFunction git_log_func_zero("git_log", {}, GitLogFunction, GitLogBind, GitLogInitGlobal);
    git_log_func_zero.init_local = GitLogLocalInit;
    git_log_func_zero.named_parameters["repo_path"] = LogicalType::VARCHAR;
    loader.RegisterFunction( git_log_func_zero);
    
    // LATERAL git_log_each function (commit ref comes from LATERAL context) - ONLY for dynamic input
    TableFunctionSet git_log_each_set("git_log_each");
    
    // Version that takes commit ref as first parameter (for LATERAL context)
    TableFunction git_log_each_single({LogicalType::VARCHAR}, nullptr, GitLogEachBind, nullptr, GitLogLocalInit);
    git_log_each_single.in_out_function = GitLogEachFunction;
    git_log_each_single.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_log_each_set.AddFunction(git_log_each_single);
    
    // Two-argument version (ref, repo_path)
    TableFunction git_log_each_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, GitLogEachBind, nullptr, GitLogLocalInit);
    git_log_each_two.in_out_function = GitLogEachFunction;
    git_log_each_two.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_log_each_set.AddFunction(git_log_each_two);
    
    loader.RegisterFunction( git_log_each_set);
}

} // namespace duckdb
#include "git_functions.hpp"
#include "git_filesystem.hpp"
#include "git_utils.hpp"
#include "git_context_manager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Git Branches Function Data
//===--------------------------------------------------------------------===//

GitBranchesFunctionData::GitBranchesFunctionData(const string &repo_path, const string &resolved_repo_path)
    : repo_path(repo_path), resolved_repo_path(resolved_repo_path), ref("") {
}

GitBranchesFunctionData::GitBranchesFunctionData(const string &ref)
    : repo_path(""), resolved_repo_path(""), ref(ref) {
}

//===--------------------------------------------------------------------===//
// Git Branches Local Init
//===--------------------------------------------------------------------===//

unique_ptr<LocalTableFunctionState> GitBranchesLocalInit(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state) {
    return make_uniq<GitBranchesLocalState>();
}

//===--------------------------------------------------------------------===//
// Git Branches Bind Function
//===--------------------------------------------------------------------===//

unique_ptr<FunctionData> GitBranchesBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
    
    // Use unified parameter parsing to support both git:// URIs and filesystem paths
    auto params = ParseUnifiedGitParams(input, 1);  // ref parameter at index 1 (optional)
    
    // Use GitContextManager for unified git URI processing and reference validation
    string resolved_repo_path, resolved_file_path, final_ref;
    try {
        auto ctx = GitContextManager::Instance().ProcessGitUri(params.repo_path_or_uri, params.ref);
        resolved_repo_path = ctx.repo_path;
        resolved_file_path = ctx.file_path;
        final_ref = ctx.final_ref;
        // ctx.repo and ctx.resolved_object are managed by GitContextManager
    } catch (const std::exception &e) {
        throw BinderException("git_branches: %s", e.what());
    }
    
    return_types = {
        LogicalType::VARCHAR,  // repo_path
        LogicalType::VARCHAR,  // branch_name
        LogicalType::VARCHAR,  // commit_hash
        LogicalType::BOOLEAN,  // is_current
        LogicalType::BOOLEAN   // is_remote
    };
    
    names = {"repo_path", "branch_name", "commit_hash", "is_current", "is_remote"};
    
    return make_uniq<GitBranchesFunctionData>(params.repo_path_or_uri, resolved_repo_path);
}

//===--------------------------------------------------------------------===//
// Git Branches Each Bind Function (Stub)
//===--------------------------------------------------------------------===//

unique_ptr<FunctionData> GitBranchesEachBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
    
    // For LATERAL functions, use ParseLateralGitParams to get optional ref parameter
    // The repo_path will come from runtime DataChunk, not bind time
    auto params = ParseLateralGitParams(input, 1);  // ref parameter at index 1 (optional)
    
    // Define return schema - same as git_branches
    return_types = {
        LogicalType::VARCHAR,  // repo_path
        LogicalType::VARCHAR,  // branch_name
        LogicalType::VARCHAR,  // commit_hash
        LogicalType::BOOLEAN,  // is_current
        LogicalType::BOOLEAN   // is_remote
    };
    
    names = {"repo_path", "branch_name", "commit_hash", "is_current", "is_remote"};
    
    // For LATERAL functions, store the ref parameter (defaults to "HEAD" from ParseLateralGitParams)
    return make_uniq<GitBranchesFunctionData>(params.ref);
}

//===--------------------------------------------------------------------===//
// Git Branches Init Global
//===--------------------------------------------------------------------===//

unique_ptr<GlobalTableFunctionState> GitBranchesInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<GlobalTableFunctionState>();
}

//===--------------------------------------------------------------------===//
// Git Branches Function
//===--------------------------------------------------------------------===//

void GitBranchesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitBranchesFunctionData>();
    auto &local_state = data_p.local_state->Cast<GitBranchesLocalState>();

    if (!local_state.initialized) {
        int error = git_repository_open(&local_state.repo, bind_data.resolved_repo_path.c_str());
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to open git repository '%s': %s",
                            bind_data.resolved_repo_path, e ? e->message : "Unknown error");
        }

        error = git_branch_iterator_new(&local_state.iterator, local_state.repo, GIT_BRANCH_ALL);
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to create branch iterator: %s", e ? e->message : "Unknown error");
        }

        local_state.initialized = true;
    }

    idx_t count = 0;
    git_reference *ref = nullptr;
    git_branch_t branch_type;

    while (count < STANDARD_VECTOR_SIZE && git_branch_next(&ref, &branch_type, local_state.iterator) == 0) {
        // Set repo_path as first column
        output.SetValue(0, count, Value(bind_data.repo_path));
        
        // Get branch name
        const char *branch_name = nullptr;
        git_branch_name(&branch_name, ref);
        output.SetValue(1, count, Value(branch_name ? branch_name : ""));
        
        // Get commit hash
        const git_oid *oid = git_reference_target(ref);
        if (oid) {
            char hash_str[GIT_OID_HEXSZ + 1];
            git_oid_tostr(hash_str, sizeof(hash_str), oid);
            output.SetValue(2, count, Value(hash_str));
        } else {
            output.SetValue(2, count, Value(""));
        }
        
        // Check if current branch
        bool is_current = git_branch_is_head(ref) == 1;
        output.SetValue(3, count, Value::BOOLEAN(is_current));
        
        // Check if remote branch
        bool is_remote = (branch_type == GIT_BRANCH_REMOTE);
        output.SetValue(4, count, Value::BOOLEAN(is_remote));
        
        git_reference_free(ref);
        count++;
    }
    
    output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// LATERAL Functions Support
//===--------------------------------------------------------------------===//

// Helper function for LATERAL git_branches_each processing - processes entire git branches for a repository path
// Uses cached repository from local_state for performance
static void ProcessBranchesForInOut(git_repository *repo, const string &repo_path, vector<GitBranchesRow> &rows) {
    // repo is from the per-thread cache in GitBranchesLocalState

    git_branch_iterator *iterator = nullptr;

    // Create branch iterator
    int error = git_branch_iterator_new(&iterator, repo, GIT_BRANCH_ALL);
    if (error != 0) {
        const git_error *e = git_error_last();
        throw IOException("Failed to create branch iterator: %s", e ? e->message : "Unknown error");
    }

    // Iterate through branches
    git_reference *ref = nullptr;
    git_branch_t branch_type;

    while (git_branch_next(&ref, &branch_type, iterator) == 0) {
        GitBranchesRow row;
        row.repo_path = repo_path;

        // Get branch name
        const char *branch_name = nullptr;
        git_branch_name(&branch_name, ref);
        row.branch_name = branch_name ? branch_name : "";

        // Get commit hash
        const git_oid *oid = git_reference_target(ref);
        if (oid) {
            char hash_str[GIT_OID_HEXSZ + 1];
            git_oid_tostr(hash_str, sizeof(hash_str), oid);
            row.commit_hash = hash_str;
        } else {
            row.commit_hash = "";
        }

        // Check if current branch
        row.is_current = (git_branch_is_head(ref) == 1);

        // Check if remote branch
        row.is_remote = (branch_type == GIT_BRANCH_REMOTE);

        rows.push_back(row);

        git_reference_free(ref);
        ref = nullptr;
    }

    if (iterator) {
        git_branch_iterator_free(iterator);
    }
    // Note: Do NOT free repo here - it's cached in local_state
}

// LATERAL git_branches_each function - processes dynamic repository paths
static OperatorResultType GitBranchesEachFunction(ExecutionContext &context, TableFunctionInput &data_p, 
                                                 DataChunk &input, DataChunk &output) {
    auto &state = data_p.local_state->Cast<GitBranchesLocalState>();
    auto &bind_data = data_p.bind_data->Cast<GitBranchesFunctionData>();
    
    // Declare resolved_repo_path at function scope so it's accessible in output loop
    string resolved_repo_path;
    
    while (true) {
        if (!state.initialized_row) {
            if (state.current_input_row >= input.size()) {
                state.current_input_row = 0;
                state.initialized_row = false;
                return OperatorResultType::NEED_MORE_INPUT;
            }
            
            // LATERAL function: extract repo_path_or_uri from input DataChunk
            input.Flatten();
            
            // Check if input has columns and data
            if (input.ColumnCount() == 0) {
                throw BinderException("git_branches_each: no input columns available");
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
                throw BinderException("git_branches_each: no string data in input column");
            }
            
            // Get the string_t directly and convert to string
            auto string_t_value = data[state.current_input_row];
            string repo_path_or_uri(string_t_value.GetData(), string_t_value.GetSize());
            
            if (repo_path_or_uri.empty()) {
                // Skip empty repo_path_or_uri and continue with next row
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
            } catch (const std::exception &e) {
                // GitContextManager provides consistent error messages
                // Skip problematic row and continue with next input
                state.current_input_row++;
                state.initialized_row = false;
                continue;
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
                    // Skip problematic repository and continue with next input row
                    state.current_input_row++;
                    state.initialized_row = false;
                    continue;
                }

                state.cached_repo_path = resolved_repo_path;
            }

            // Process the git branches using the cached repository
            state.current_rows.clear();
            try {
                ProcessBranchesForInOut(state.cached_repo, resolved_repo_path, state.current_rows);
            } catch (const std::exception &e) {
                // Skip problematic repository and continue with next input row
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            state.initialized_row = true;
            state.current_output_row = 0;
        }
        
        idx_t output_count = 0;
        while (output_count < STANDARD_VECTOR_SIZE && 
               state.current_output_row < state.current_rows.size()) {
            
            auto &row = state.current_rows[state.current_output_row];
            
            output.SetValue(0, output_count, Value(resolved_repo_path));
            output.SetValue(1, output_count, Value(row.branch_name));
            output.SetValue(2, output_count, Value(row.commit_hash));
            output.SetValue(3, output_count, Value(row.is_current));
            output.SetValue(4, output_count, Value(row.is_remote));
            
            output_count++;
            state.current_output_row++;
        }
        
        output.SetCardinality(output_count);
        
        if (state.current_output_row >= state.current_rows.size()) {
            state.current_input_row++;
            state.initialized_row = false;
        }
        
        return OperatorResultType::HAVE_MORE_OUTPUT;
    }
}

//===--------------------------------------------------------------------===//
// Registration Function
//===--------------------------------------------------------------------===//

void RegisterGitBranchesFunction(ExtensionLoader &loader) {
    // Single-argument version (existing)
    TableFunction git_branches_func("git_branches", {LogicalType::VARCHAR}, GitBranchesFunction, GitBranchesBind, GitBranchesInitGlobal);
    git_branches_func.init_local = GitBranchesLocalInit;
    git_branches_func.named_parameters["repo_path"] = LogicalType::VARCHAR;
    loader.RegisterFunction( git_branches_func);

    // Zero-argument version (defaults to current directory)
    TableFunction git_branches_func_zero("git_branches", {}, GitBranchesFunction, GitBranchesBind, GitBranchesInitGlobal);
    git_branches_func_zero.init_local = GitBranchesLocalInit;
    git_branches_func_zero.named_parameters["repo_path"] = LogicalType::VARCHAR;
    loader.RegisterFunction( git_branches_func_zero);
    
    // LATERAL git_branches_each function (repository path comes from LATERAL context) - ONLY for dynamic input
    TableFunctionSet git_branches_each_set("git_branches_each");
    
    // Version that takes repository path as first parameter (for LATERAL context)
    TableFunction git_branches_each_single({LogicalType::VARCHAR}, nullptr, GitBranchesEachBind, nullptr, GitBranchesLocalInit);
    git_branches_each_single.in_out_function = GitBranchesEachFunction;
    git_branches_each_single.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_branches_each_set.AddFunction(git_branches_each_single);
    
    // Two-argument version (repo_path, repo_path)
    TableFunction git_branches_each_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, GitBranchesEachBind, nullptr, GitBranchesLocalInit);
    git_branches_each_two.in_out_function = GitBranchesEachFunction;
    git_branches_each_two.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_branches_each_set.AddFunction(git_branches_each_two);
    
    loader.RegisterFunction( git_branches_each_set);
}

} // namespace duckdb
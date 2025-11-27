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

namespace duckdb {

// Helper function needed by git_parents
static string oid_to_hex(const git_oid *oid) {
    char hex[GIT_OID_HEXSZ + 1];
    git_oid_tostr(hex, sizeof(hex), oid);
    return string(hex);
}

// Schema definition for git_parents (ALWAYS includes repo_path as first column)
static void DefineGitParentsSchema(vector<LogicalType> &return_types, vector<string> &names) {
    return_types = {
        LogicalType::VARCHAR,    // repo_path (ALWAYS first)
        LogicalType::VARCHAR,    // commit_hash
        LogicalType::VARCHAR,    // parent_hash
        LogicalType::INTEGER     // parent_index
    };
    names = {"repo_path", "commit_hash", "parent_hash", "parent_index"};
}

// Output helper for git_parents rows (repo_path is REQUIRED)
static void OutputGitParentsRow(DataChunk &output, idx_t row_idx,
                                const GitParentsRow &row, const string &repo_path) {
    idx_t col = 0;
    output.SetValue(col++, row_idx, Value(repo_path));              // repo_path
    output.SetValue(col++, row_idx, Value(row.commit_hash));        // commit_hash
    output.SetValue(col++, row_idx, Value(row.parent_hash));        // parent_hash
    output.SetValue(col++, row_idx, Value::INTEGER(row.parent_index)); // parent_index
}

//===--------------------------------------------------------------------===//
// Git Parents Function
//===--------------------------------------------------------------------===//

GitParentsFunctionData::GitParentsFunctionData(const string &ref, const string &repo_path, bool all_refs)
    : ref(ref), repo_path(repo_path), all_refs(all_refs) {
}

unique_ptr<FunctionData> GitParentsBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
    bool all_refs = false;
    
    // Use unified parameter parsing for new signature: git_parents(repo_path_or_uri, [ref])
    auto params = ParseUnifiedGitParams(input, 1);  // ref parameter at index 1
    
    // Check for named parameters
    if (input.named_parameters.count("repo_path") && !StringUtil::StartsWith(params.repo_path_or_uri, "git://")) {
        params.resolved_repo_path = StringValue::Get(input.named_parameters.at("repo_path"));
    }
    if (input.named_parameters.count("all_refs")) {
        all_refs = BooleanValue::Get(input.named_parameters.at("all_refs"));
    }
    
    // Process the git URI through GitContextManager
    string resolved_repo_path;
    string final_ref;
    try {
        auto ctx = GitContextManager::Instance().ProcessGitUri(params.repo_path_or_uri, params.ref);
        resolved_repo_path = ctx.repo_path;
        final_ref = ctx.final_ref;
        // file_path not needed for git_parents
    } catch (const std::exception &e) {
        throw BinderException("git_parents: %s", e.what());
    }
    
    // Use helper to define schema with repo_path as first column
    DefineGitParentsSchema(return_types, names);
    
    return make_uniq<GitParentsFunctionData>(final_ref, resolved_repo_path, all_refs);
}

unique_ptr<GlobalTableFunctionState> GitParentsInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind_data = const_cast<GitParentsFunctionData&>(input.bind_data->Cast<GitParentsFunctionData>());
    
    // libgit2 is initialized at extension load time
    
    git_repository *repo = nullptr;
    int error = git_repository_open(&repo, bind_data.repo_path.c_str());
    if (error != 0) {
        const git_error *e = git_error_last();
        throw IOException("git_parents: failed to open git repository '%s': %s", 
                        bind_data.repo_path, e ? e->message : "Unknown error");
    }

    git_revwalk *walk = nullptr;
    git_revwalk_new(&walk, repo);
    git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);

    if (bind_data.all_refs) {
        git_reference_iterator *it = nullptr;
        git_reference_iterator_new(&it, repo);
        git_reference *ref;
        while (!git_reference_next(&ref, it)) {
            const git_oid *target = git_reference_target(ref);
            if (target) {
                git_revwalk_push(walk, target);
            }
        }
        git_reference_iterator_free(it);
    } else {
        git_object *obj = nullptr;
        if (git_revparse_single(&obj, repo, bind_data.ref.c_str()) != 0) {
            const git_error *e = git_error_last();
            git_revwalk_free(walk);
            git_repository_free(repo);
            throw IOException("git_parents: unable to parse ref '%s': %s", 
                            bind_data.ref, e ? e->message : "unable to parse OID");
        }
        git_oid oid = *git_object_id(obj);
        git_revwalk_push(walk, &oid);
        git_object_free(obj);
    }

    vector<GitParentsRow> rows;
    git_oid oid;
    while (!git_revwalk_next(&oid, walk)) {
        git_commit *commit = nullptr;
        if (git_commit_lookup(&commit, repo, &oid) != 0) {
            continue;
        }
        
        unsigned int parent_count = git_commit_parentcount(commit);
        for (unsigned int i = 0; i < parent_count; i++) {
            const git_oid *parent_oid = git_commit_parent_id(commit, i);
            rows.push_back(GitParentsRow{
                oid_to_hex(&oid), 
                oid_to_hex(parent_oid), 
                static_cast<int32_t>(i)
            });
        }
        git_commit_free(commit);
    }

    // Store rows in bind_data
    bind_data.rows = std::move(rows);

    git_revwalk_free(walk);
    git_repository_free(repo);

    return make_uniq<GlobalTableFunctionState>();
}

void GitParentsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitParentsFunctionData>();
    auto &local_state = data_p.local_state->Cast<GitParentsLocalState>();

    idx_t remaining = bind_data.rows.size() - local_state.current_index;
    if (remaining == 0) {
        output.SetCardinality(0);
        return;
    }

    const idx_t count = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE);

    for (idx_t i = 0; i < count; i++) {
        auto &row = bind_data.rows[local_state.current_index + i];
        OutputGitParentsRow(output, i, row, bind_data.repo_path);
    }

    output.SetCardinality(count);
    local_state.current_index += count;
}

// Local init for git_parents_each
unique_ptr<LocalTableFunctionState> GitParentsLocalInit(ExecutionContext &context, TableFunctionInitInput &input, 
                                                       GlobalTableFunctionState *global_state) {
    return make_uniq<GitParentsLocalState>();
}

// Helper function for processing parents of a single commit
static bool ProcessParentsForCommit(const string &repo_path, const string &commit_ref, vector<GitParentsRow> &rows) {
    // libgit2 is initialized at extension load time
    
    git_repository *repo = nullptr;
    int error = git_repository_open(&repo, repo_path.c_str());
    if (error != 0) {
        // Skip this row - repository not accessible
        return false;
    }
    
    git_object *obj = nullptr;
    if (git_revparse_single(&obj, repo, commit_ref.c_str()) != 0) {
        git_repository_free(repo);
        // Skip this row - unable to parse ref
        return false;
    }
    
    if (git_object_type(obj) != GIT_OBJECT_COMMIT) {
        git_object_free(obj);
        git_repository_free(repo);
        // Skip this row - ref is not a commit
        return false;
    }
    
    git_commit *commit = reinterpret_cast<git_commit *>(obj);
    string commit_hash = oid_to_hex(git_commit_id(commit));
    
    unsigned int parent_count = git_commit_parentcount(commit);
    for (unsigned int i = 0; i < parent_count; i++) {
        const git_oid *parent_oid = git_commit_parent_id(commit, i);
        rows.push_back(GitParentsRow{
            commit_hash,
            oid_to_hex(parent_oid),
            static_cast<int32_t>(i)
        });
    }
    
    git_commit_free(commit);
    git_repository_free(repo);
    return true;
}

// LATERAL git_parents_each function
OperatorResultType GitParentsEachFunction(ExecutionContext &context, TableFunctionInput &data_p,
                                         DataChunk &input, DataChunk &output) {
    auto &state = data_p.local_state->Cast<GitParentsLocalState>();
    auto &bind_data = data_p.bind_data->Cast<GitParentsEachBindData>();
    
    while (true) {
        if (!state.initialized_row) {
            if (state.current_input_row >= input.size()) {
                state.current_input_row = 0;
                state.initialized_row = false;
                return OperatorResultType::NEED_MORE_INPUT;
            }
            
            // LATERAL function: extract URI from input DataChunk
            input.Flatten();
            
            if (input.ColumnCount() == 0) {
                throw BinderException("git_parents_each: no input columns available");
            }
            
            if (FlatVector::IsNull(input.data[0], state.current_input_row)) {
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            auto data = FlatVector::GetData<string_t>(input.data[0]);
            if (!data) {
                // Skip this row - no string data in input column
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            auto string_t_value = data[state.current_input_row];
            string repo_path_or_uri(string_t_value.GetData(), string_t_value.GetSize());
            
            if (repo_path_or_uri.empty()) {
                // Skip this row - empty repository URI
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            // Process the git URI through GitContextManager using bind_data.ref as fallback
            string resolved_repo_path;
            string final_ref;
            try {
                auto ctx = GitContextManager::Instance().ProcessGitUri(repo_path_or_uri, bind_data.ref);
                resolved_repo_path = ctx.repo_path;
                final_ref = ctx.final_ref;
                // file_path not needed for git_parents
            } catch (const std::exception &e) {
                // Skip this row - unable to process git URI
                state.current_input_row++;
                state.initialized_row = false;
                continue;
            }
            
            // Process the parents for this commit
            state.current_rows.clear();
            state.current_repo_path = resolved_repo_path;
            if (!ProcessParentsForCommit(resolved_repo_path, final_ref, state.current_rows)) {
                // Skip this row - unable to process commit parents
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
            OutputGitParentsRow(output, output_count, row, state.current_repo_path);
            
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

// Bind function for git_parents_each
unique_ptr<FunctionData> GitParentsEachBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
    // Use lateral parameter parsing - ref parameter at index 1 (optional)
    auto params = ParseLateralGitParams(input, 1);  // ref at index 1 (optional)
    
    // Define return schema
    DefineGitParentsSchema(return_types, names);
    
    // Create bind data with parsed ref as fallback for GitContextManager
    auto bind_data = make_uniq<GitParentsEachBindData>();
    bind_data->repo_path = ".";  // Placeholder, actual repo paths come from runtime DataChunk
    bind_data->ref = params.ref;  // This will be "HEAD" by default from ParseLateralGitParams
    return std::move(bind_data);
}

void RegisterGitParentsFunction(ExtensionLoader &loader) {
    // Single-argument version (ref)
    TableFunction git_parents_func("git_parents", {LogicalType::VARCHAR}, GitParentsFunction, GitParentsBind, GitParentsInitGlobal);
    git_parents_func.init_local = GitParentsLocalInit;
    git_parents_func.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_parents_func.named_parameters["all_refs"] = LogicalType::BOOLEAN;
    loader.RegisterFunction( git_parents_func);

    // Two-argument version (repo_path_or_uri, ref)
    TableFunction git_parents_two("git_parents", {LogicalType::VARCHAR, LogicalType::VARCHAR}, GitParentsFunction, GitParentsBind, GitParentsInitGlobal);
    git_parents_two.init_local = GitParentsLocalInit;
    git_parents_two.named_parameters["all_refs"] = LogicalType::BOOLEAN;
    loader.RegisterFunction( git_parents_two);

    // Zero-argument version (defaults to HEAD and current directory)
    TableFunction git_parents_zero("git_parents", {}, GitParentsFunction, GitParentsBind, GitParentsInitGlobal);
    git_parents_zero.init_local = GitParentsLocalInit;
    git_parents_zero.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_parents_zero.named_parameters["all_refs"] = LogicalType::BOOLEAN;
    loader.RegisterFunction( git_parents_zero);
    
    // LATERAL git_parents_each function
    TableFunctionSet git_parents_each_set("git_parents_each");
    
    // Version that takes commit ref as first parameter
    TableFunction git_parents_each_single({LogicalType::VARCHAR}, nullptr, GitParentsEachBind, nullptr, GitParentsLocalInit);
    git_parents_each_single.in_out_function = GitParentsEachFunction;
    git_parents_each_single.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_parents_each_set.AddFunction(git_parents_each_single);
    
    // Two-argument version (commit_ref, repo_path)
    TableFunction git_parents_each_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, GitParentsEachBind, nullptr, GitParentsLocalInit);
    git_parents_each_two.in_out_function = GitParentsEachFunction;
    git_parents_each_set.AddFunction(git_parents_each_two);
    
    loader.RegisterFunction( git_parents_each_set);
}

} // namespace duckdb
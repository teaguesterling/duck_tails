#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include <git2.h>

namespace duckdb {

// Git log table function
struct GitLogFunctionData : public TableFunctionData {
    explicit GitLogFunctionData(const string &repo_path, const string &resolved_repo_path);
    ~GitLogFunctionData();
    
    string repo_path;
    string resolved_repo_path;
    git_repository *repo;
    git_revwalk *walker;
    bool initialized;
};

void GitLogFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
unique_ptr<FunctionData> GitLogBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitLogInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Git branches table function  
struct GitBranchesFunctionData : public TableFunctionData {
    explicit GitBranchesFunctionData(const string &repo_path, const string &resolved_repo_path);
    ~GitBranchesFunctionData();
    
    string repo_path;
    string resolved_repo_path;
    git_repository *repo;
    git_branch_iterator *iterator;
    bool initialized;
};

void GitBranchesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
unique_ptr<FunctionData> GitBranchesBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitBranchesInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Git tags table function
struct GitTagsFunctionData : public TableFunctionData {
    explicit GitTagsFunctionData(const string &repo_path, const string &resolved_repo_path);
    ~GitTagsFunctionData();
    
    string repo_path;
    string resolved_repo_path;
    git_repository *repo;
    vector<string> tag_names;
    idx_t current_index;
    bool initialized;
};

void GitTagsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
unique_ptr<FunctionData> GitTagsBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names);
unique_ptr<GlobalTableFunctionState> GitTagsInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Registration functions
void RegisterGitLogFunction(DatabaseInstance &db);
void RegisterGitBranchesFunction(DatabaseInstance &db);  
void RegisterGitTagsFunction(DatabaseInstance &db);
void RegisterGitFunctions(DatabaseInstance &db);

} // namespace duckdb
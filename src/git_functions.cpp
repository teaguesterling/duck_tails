#include "git_functions.hpp"
#include "git_filesystem.hpp"
#include "git_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {

// UnifiedGitParams and parsing functions moved to git_utils.hpp/cpp
// All git functions have been extracted to their respective files:
// - git_log.cpp
// - git_branches.cpp
// - git_tags.cpp
// - git_tree.cpp
// - git_parents.cpp
// - git_read.cpp
// - git_uri.cpp

// Forward declarations for functions defined in other files
void RegisterGitLogFunction(ExtensionLoader &loader);
void RegisterGitBranchesFunction(ExtensionLoader &loader);
void RegisterGitTagsFunction(ExtensionLoader &loader);
void RegisterGitTreeFunction(ExtensionLoader &loader);
void RegisterGitParentsFunction(ExtensionLoader &loader);
void RegisterGitReadFunction(ExtensionLoader &loader);
void RegisterGitUriFunction(ExtensionLoader &loader);

void RegisterGitFunctions(ExtensionLoader &loader) {
	RegisterGitLogFunction(loader);
	RegisterGitBranchesFunction(loader);
	RegisterGitTagsFunction(loader);
	RegisterGitTreeFunction(loader);
	RegisterGitParentsFunction(loader);
	RegisterGitReadFunction(loader);
	RegisterGitUriFunction(loader);
}

} // namespace duckdb

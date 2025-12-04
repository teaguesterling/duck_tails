#include "git_functions.hpp"
#include "git_filesystem.hpp"
#include "git_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Helper Function for URI Construction
//===--------------------------------------------------------------------===//

static string ConstructGitUri(const string &repo_path, const string &file_path, const string &commit_ref) {
	if (file_path.empty()) {
		return "git://" + repo_path + "@" + commit_ref;
	}

	// Normalize paths to avoid double slashes
	string normalized_repo = repo_path;
	string normalized_file = file_path;

	// Remove trailing slash from repo_path
	while (!normalized_repo.empty() && normalized_repo.back() == '/') {
		normalized_repo.pop_back();
	}

	// Remove leading slash from file_path
	while (!normalized_file.empty() && normalized_file.front() == '/') {
		normalized_file.erase(0, 1);
	}

	return "git://" + normalized_repo + "/" + normalized_file + "@" + commit_ref;
}

//===--------------------------------------------------------------------===//
// git_uri() Scalar Function - Helper for URI construction
//===--------------------------------------------------------------------===//

static void GitUriFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// Handle case where all inputs are constant - create constant output
	if (args.data[0].GetVectorType() == VectorType::CONSTANT_VECTOR &&
	    args.data[1].GetVectorType() == VectorType::CONSTANT_VECTOR &&
	    args.data[2].GetVectorType() == VectorType::CONSTANT_VECTOR) {

		auto repo_path_value = ConstantVector::GetData<string_t>(args.data[0]);
		auto file_path_value = ConstantVector::GetData<string_t>(args.data[1]);
		auto commit_ref_value = ConstantVector::GetData<string_t>(args.data[2]);

		if (ConstantVector::IsNull(args.data[0]) || ConstantVector::IsNull(args.data[1]) ||
		    ConstantVector::IsNull(args.data[2])) {
			ConstantVector::SetNull(result, true);
			return;
		}

		string repo_path = repo_path_value->GetString();
		string file_path = file_path_value->GetString();
		string commit_ref = commit_ref_value->GetString();

		// Use common function to construct URI
		string uri = ConstructGitUri(repo_path, file_path, commit_ref);

		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		auto result_data = ConstantVector::GetData<string_t>(result);
		*result_data = StringVector::AddString(result, uri);
		return;
	}

	// Handle general case with flat vectors
	auto &repo_path_vector = args.data[0];
	auto &file_path_vector = args.data[1];
	auto &commit_ref_vector = args.data[2];

	UnifiedVectorFormat repo_path_format, file_path_format, commit_ref_format;
	repo_path_vector.ToUnifiedFormat(args.size(), repo_path_format);
	file_path_vector.ToUnifiedFormat(args.size(), file_path_format);
	commit_ref_vector.ToUnifiedFormat(args.size(), commit_ref_format);

	auto repo_path_data = UnifiedVectorFormat::GetData<string_t>(repo_path_format);
	auto file_path_data = UnifiedVectorFormat::GetData<string_t>(file_path_format);
	auto commit_ref_data = UnifiedVectorFormat::GetData<string_t>(commit_ref_format);

	// Ensure result vector is flat for per-row writes (ChatGPT Fix #1)
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<string_t>(result);
	for (idx_t i = 0; i < args.size(); i++) {
		auto repo_idx = repo_path_format.sel->get_index(i);
		auto file_idx = file_path_format.sel->get_index(i);
		auto commit_idx = commit_ref_format.sel->get_index(i);

		if (!repo_path_format.validity.RowIsValid(repo_idx) || !file_path_format.validity.RowIsValid(file_idx) ||
		    !commit_ref_format.validity.RowIsValid(commit_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		string repo_path = repo_path_data[repo_idx].GetString();
		string file_path = file_path_data[file_idx].GetString();
		string commit_ref = commit_ref_data[commit_idx].GetString();

		// Use common function to construct URI
		string uri = ConstructGitUri(repo_path, file_path, commit_ref);

		result_data[i] = StringVector::AddString(result, uri);
	}
}

void RegisterGitUriFunction(ExtensionLoader &loader) {
	auto git_uri_func = ScalarFunction("git_uri", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                   LogicalType::VARCHAR, GitUriFunction);
	loader.RegisterFunction(git_uri_func);
}

} // namespace duckdb

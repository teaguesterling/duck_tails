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

//===--------------------------------------------------------------------===//
// GitDiffTreeRow
//===--------------------------------------------------------------------===//

struct GitDiffTreeRow {
	string repo_path;
	string file_path;
	string file_ext;
	string status;   // "added", "deleted", "modified", "renamed", "copied", "typechange"
	string old_path; // rename/copy source (empty otherwise)
};

//===--------------------------------------------------------------------===//
// Bind Data
//===--------------------------------------------------------------------===//

struct GitDiffTreeFunctionData : public TableFunctionData {
	string repo_path;
	string ref;
	string path_filter;
	bool include_untracked;
	vector<GitDiffTreeRow> rows;
	bool is_lateral;

	GitDiffTreeFunctionData(const string &repo_path, const string &ref, const string &path_filter,
	                        bool include_untracked, bool is_lateral = false)
	    : repo_path(repo_path), ref(ref), path_filter(path_filter), include_untracked(include_untracked),
	      is_lateral(is_lateral) {
	}
};

//===--------------------------------------------------------------------===//
// Local State
//===--------------------------------------------------------------------===//

struct GitDiffTreeLocalState : public LocalTableFunctionState {
	idx_t current_index = 0;

	// LATERAL processing state
	vector<GitDiffTreeRow> current_rows;
	idx_t current_input_row = 0;
	idx_t current_output_row = 0;
	bool initialized_row = false;
};

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//

static string ExtractFileExtension(const string &path) {
	size_t dot_pos = path.find_last_of('.');
	if (dot_pos == string::npos || dot_pos == path.length() - 1) {
		return "";
	}
	return path.substr(dot_pos);
}

static string DeltaStatusToString(git_delta_t delta) {
	switch (delta) {
	case GIT_DELTA_ADDED:
		return "added";
	case GIT_DELTA_DELETED:
		return "deleted";
	case GIT_DELTA_MODIFIED:
		return "modified";
	case GIT_DELTA_RENAMED:
		return "renamed";
	case GIT_DELTA_COPIED:
		return "copied";
	case GIT_DELTA_TYPECHANGE:
		return "typechange";
	case GIT_DELTA_UNTRACKED:
		return "untracked";
	case GIT_DELTA_IGNORED:
		return "ignored";
	default:
		return "unknown";
	}
}

static void CollectDiffRows(git_repository *repo, const string &repo_path, const string &ref,
                            const string &path_filter, bool include_untracked, vector<GitDiffTreeRow> &rows) {
	// Resolve the ref to a commit, then get its tree
	git_object *obj = nullptr;
	int error = git_revparse_single(&obj, repo, ref.c_str());
	if (error != 0) {
		const git_error *e = git_error_last();
		throw IOException("git_diff_tree: unable to resolve ref '%s': %s", ref, e ? e->message : "unknown error");
	}

	git_commit *commit = nullptr;
	error = git_object_peel(reinterpret_cast<git_object **>(&commit), obj, GIT_OBJECT_COMMIT);
	if (error != 0) {
		git_object_free(obj);
		throw IOException("git_diff_tree: ref '%s' does not resolve to a commit", ref);
	}

	git_tree *tree = nullptr;
	error = git_commit_tree(&tree, commit);
	if (error != 0) {
		git_commit_free(commit);
		git_object_free(obj);
		throw IOException("git_diff_tree: failed to get tree for ref '%s'", ref);
	}

	// Set up diff options
	git_diff_options diff_opts = GIT_DIFF_OPTIONS_INIT;
	diff_opts.flags = GIT_DIFF_INCLUDE_TYPECHANGE;
	if (include_untracked) {
		diff_opts.flags |= GIT_DIFF_INCLUDE_UNTRACKED;
	}

	// Path filter
	char *pathspec_cstr = nullptr;
	if (!path_filter.empty()) {
		diff_opts.pathspec.count = 1;
		pathspec_cstr = const_cast<char *>(path_filter.c_str());
		diff_opts.pathspec.strings = &pathspec_cstr;
	}

	// Diff the tree against the working directory
	git_diff *diff = nullptr;
	error = git_diff_tree_to_workdir_with_index(&diff, repo, tree, &diff_opts);
	if (error != 0) {
		const git_error *e = git_error_last();
		git_tree_free(tree);
		git_commit_free(commit);
		git_object_free(obj);
		throw IOException("git_diff_tree: failed to compute diff: %s", e ? e->message : "unknown error");
	}

	// Enable rename/copy detection
	git_diff_find_options find_opts = GIT_DIFF_FIND_OPTIONS_INIT;
	find_opts.flags = GIT_DIFF_FIND_RENAMES | GIT_DIFF_FIND_COPIES;
	git_diff_find_similar(diff, &find_opts);

	// Iterate diff deltas
	size_t num_deltas = git_diff_num_deltas(diff);
	for (size_t i = 0; i < num_deltas; i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);
		if (!delta) {
			continue;
		}

		// Skip ignored files
		if (delta->status == GIT_DELTA_IGNORED) {
			continue;
		}

		GitDiffTreeRow row;
		row.repo_path = repo_path;
		row.status = DeltaStatusToString(delta->status);

		// Use new_file path for most cases, old_file for deletions
		if (delta->status == GIT_DELTA_DELETED) {
			row.file_path = delta->old_file.path ? delta->old_file.path : "";
		} else {
			row.file_path = delta->new_file.path ? delta->new_file.path : "";
		}

		row.file_ext = ExtractFileExtension(row.file_path);

		// Track old path for renames/copies
		if ((delta->status == GIT_DELTA_RENAMED || delta->status == GIT_DELTA_COPIED) && delta->old_file.path) {
			row.old_path = delta->old_file.path;
		}

		rows.push_back(std::move(row));
	}

	git_diff_free(diff);
	git_tree_free(tree);
	git_commit_free(commit);
	git_object_free(obj);
}

//===--------------------------------------------------------------------===//
// Schema
//===--------------------------------------------------------------------===//

static void DefineGitDiffTreeSchema(vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {
	    LogicalType::VARCHAR, // repo_path
	    LogicalType::VARCHAR, // file_path
	    LogicalType::VARCHAR, // file_ext
	    LogicalType::VARCHAR, // status
	    LogicalType::VARCHAR  // old_path
	};
	names = {"repo_path", "file_path", "file_ext", "status", "old_path"};
}

static vector<LogicalType> GetGitDiffTreeSchema() {
	vector<LogicalType> return_types;
	vector<string> names;
	DefineGitDiffTreeSchema(return_types, names);
	return return_types;
}

static vector<string> GetGitDiffTreeColumnNames() {
	vector<LogicalType> return_types;
	vector<string> names;
	DefineGitDiffTreeSchema(return_types, names);
	return names;
}

static void OutputGitDiffTreeRow(DataChunk &output, const GitDiffTreeRow &row, idx_t row_idx) {
	output.SetValue(0, row_idx, Value(row.repo_path));
	output.SetValue(1, row_idx, Value(row.file_path));
	output.SetValue(2, row_idx, Value(row.file_ext));
	output.SetValue(3, row_idx, Value(row.status));
	if (!row.old_path.empty()) {
		output.SetValue(4, row_idx, Value(row.old_path));
	} else {
		output.SetValue(4, row_idx, Value());
	}
}

//===--------------------------------------------------------------------===//
// Bind
//===--------------------------------------------------------------------===//

static unique_ptr<FunctionData> GitDiffTreeBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	return_types = GetGitDiffTreeSchema();
	names = GetGitDiffTreeColumnNames();

	string repo_path = ".";
	string ref = "HEAD";
	string path_filter;
	bool include_untracked = false;

	// Parse positional parameters
	if (!input.inputs.empty()) {
		string first_param = input.inputs[0].GetValue<string>();
		try {
			auto git_path = GitPath::Parse("git://" + first_param + "@HEAD");
			repo_path = git_path.repository_path;
		} catch (const std::exception &e) {
			throw BinderException("git_diff_tree: failed to resolve repository path '%s': %s", first_param, e.what());
		}
	} else {
		try {
			auto git_path = GitPath::Parse("git://.@HEAD");
			repo_path = git_path.repository_path;
		} catch (const std::exception &e) {
			throw BinderException("git_diff_tree: failed to resolve repository: %s", e.what());
		}
	}

	// Second positional parameter: ref
	if (input.inputs.size() > 1 && !input.inputs[1].IsNull()) {
		ref = input.inputs[1].GetValue<string>();
	}

	// Parse named parameters
	for (const auto &kv : input.named_parameters) {
		if (kv.first == "path") {
			path_filter = kv.second.GetValue<string>();
		} else if (kv.first == "untracked") {
			include_untracked = kv.second.GetValue<bool>();
		}
	}

	return make_uniq<GitDiffTreeFunctionData>(repo_path, ref, path_filter, include_untracked);
}

//===--------------------------------------------------------------------===//
// Init Global
//===--------------------------------------------------------------------===//

static unique_ptr<GlobalTableFunctionState> GitDiffTreeInitGlobal(ClientContext &context,
                                                                   TableFunctionInitInput &input) {
	auto &bind_data = const_cast<GitDiffTreeFunctionData &>(input.bind_data->Cast<GitDiffTreeFunctionData>());

	if (!bind_data.is_lateral) {
		git_repository *repo = nullptr;
		int error = git_repository_open(&repo, bind_data.repo_path.c_str());
		if (error != 0) {
			const git_error *git_err = git_error_last();
			string error_msg = git_err ? git_err->message : "Unknown git error";
			throw BinderException("git_diff_tree: failed to open repository '%s': %s", bind_data.repo_path, error_msg);
		}

		try {
			CollectDiffRows(repo, bind_data.repo_path, bind_data.ref, bind_data.path_filter,
			                bind_data.include_untracked, bind_data.rows);
		} catch (...) {
			git_repository_free(repo);
			throw;
		}

		git_repository_free(repo);
	}

	return make_uniq<GlobalTableFunctionState>();
}

//===--------------------------------------------------------------------===//
// Function (scan)
//===--------------------------------------------------------------------===//

static void GitDiffTreeFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<GitDiffTreeFunctionData>();
	auto &local_state = data_p.local_state->Cast<GitDiffTreeLocalState>();

	idx_t output_count = 0;
	const idx_t max_output = STANDARD_VECTOR_SIZE;

	while (local_state.current_index < bind_data.rows.size() && output_count < max_output) {
		auto &row = bind_data.rows[local_state.current_index];
		OutputGitDiffTreeRow(output, row, output_count);
		local_state.current_index++;
		output_count++;
	}

	output.SetCardinality(output_count);
}

//===--------------------------------------------------------------------===//
// Local Init
//===--------------------------------------------------------------------===//

static unique_ptr<LocalTableFunctionState> GitDiffTreeLocalInit(ExecutionContext &context,
                                                                 TableFunctionInitInput &input,
                                                                 GlobalTableFunctionState *global_state) {
	return make_uniq<GitDiffTreeLocalState>();
}

//===--------------------------------------------------------------------===//
// LATERAL (git_diff_tree_each)
//===--------------------------------------------------------------------===//

static unique_ptr<FunctionData> GitDiffTreeEachBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	return_types = GetGitDiffTreeSchema();
	names = GetGitDiffTreeColumnNames();

	string ref = "HEAD";
	string path_filter;
	bool include_untracked = false;

	// Second positional parameter: ref
	if (input.inputs.size() > 1 && !input.inputs[1].IsNull()) {
		ref = input.inputs[1].GetValue<string>();
	}

	for (const auto &kv : input.named_parameters) {
		if (kv.first == "path") {
			path_filter = kv.second.GetValue<string>();
		} else if (kv.first == "untracked") {
			include_untracked = kv.second.GetValue<bool>();
		}
	}

	return make_uniq<GitDiffTreeFunctionData>(".", ref, path_filter, include_untracked, true);
}

static OperatorResultType GitDiffTreeEachFunction(ExecutionContext &context, TableFunctionInput &data_p,
                                                  DataChunk &input, DataChunk &output) {
	auto &state = data_p.local_state->Cast<GitDiffTreeLocalState>();
	auto &bind_data = data_p.bind_data->Cast<GitDiffTreeFunctionData>();

	while (true) {
		if (!state.initialized_row) {
			if (state.current_input_row >= input.size()) {
				state.current_input_row = 0;
				state.initialized_row = false;
				return OperatorResultType::NEED_MORE_INPUT;
			}

			input.Flatten();

			if (input.ColumnCount() == 0 || FlatVector::IsNull(input.data[0], state.current_input_row)) {
				state.current_input_row++;
				continue;
			}

			auto data = FlatVector::GetData<string_t>(input.data[0]);
			string repo_path_or_uri(data[state.current_input_row].GetData(), data[state.current_input_row].GetSize());

			if (repo_path_or_uri.empty()) {
				state.current_input_row++;
				continue;
			}

			// Resolve repo path
			string resolved_repo_path;
			try {
				auto git_path = GitPath::Parse("git://" + repo_path_or_uri + "@HEAD");
				resolved_repo_path = git_path.repository_path;
			} catch (...) {
				state.current_input_row++;
				continue;
			}

			state.current_rows.clear();

			git_repository *repo = nullptr;
			int error = git_repository_open(&repo, resolved_repo_path.c_str());
			if (error != 0) {
				state.current_input_row++;
				continue;
			}

			try {
				CollectDiffRows(repo, resolved_repo_path, bind_data.ref, bind_data.path_filter,
				                bind_data.include_untracked, state.current_rows);
			} catch (...) {
				git_repository_free(repo);
				state.current_input_row++;
				continue;
			}

			git_repository_free(repo);
			state.initialized_row = true;
			state.current_output_row = 0;
		}

		// Output rows
		idx_t output_count = 0;
		while (output_count < STANDARD_VECTOR_SIZE && state.current_output_row < state.current_rows.size()) {
			OutputGitDiffTreeRow(output, state.current_rows[state.current_output_row], output_count);
			output_count++;
			state.current_output_row++;
		}

		output.SetCardinality(output_count);

		if (state.current_output_row >= state.current_rows.size()) {
			state.current_input_row++;
			state.initialized_row = false;
		}

		if (output_count > 0) {
			return OperatorResultType::HAVE_MORE_OUTPUT;
		}
	}
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void RegisterGitDiffTreeFunction(ExtensionLoader &loader) {
	TableFunctionSet git_diff_tree_set("git_diff_tree");

	// Zero parameters: git_diff_tree()
	TableFunction git_diff_tree_zero({}, GitDiffTreeFunction, GitDiffTreeBind, GitDiffTreeInitGlobal);
	git_diff_tree_zero.init_local = GitDiffTreeLocalInit;
	git_diff_tree_zero.named_parameters["path"] = LogicalType::VARCHAR;
	git_diff_tree_zero.named_parameters["untracked"] = LogicalType::BOOLEAN;
	git_diff_tree_set.AddFunction(git_diff_tree_zero);

	// Single parameter: git_diff_tree(repo_path)
	TableFunction git_diff_tree_single({LogicalType::VARCHAR}, GitDiffTreeFunction, GitDiffTreeBind,
	                                   GitDiffTreeInitGlobal);
	git_diff_tree_single.init_local = GitDiffTreeLocalInit;
	git_diff_tree_single.named_parameters["path"] = LogicalType::VARCHAR;
	git_diff_tree_single.named_parameters["untracked"] = LogicalType::BOOLEAN;
	git_diff_tree_set.AddFunction(git_diff_tree_single);

	// Two parameters: git_diff_tree(repo_path, ref)
	TableFunction git_diff_tree_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, GitDiffTreeFunction, GitDiffTreeBind,
	                                GitDiffTreeInitGlobal);
	git_diff_tree_two.init_local = GitDiffTreeLocalInit;
	git_diff_tree_two.named_parameters["path"] = LogicalType::VARCHAR;
	git_diff_tree_two.named_parameters["untracked"] = LogicalType::BOOLEAN;
	git_diff_tree_set.AddFunction(git_diff_tree_two);

	loader.RegisterFunction(git_diff_tree_set);

	// LATERAL: git_diff_tree_each
	TableFunctionSet git_diff_tree_each_set("git_diff_tree_each");

	TableFunction git_diff_tree_each_single({LogicalType::VARCHAR}, nullptr, GitDiffTreeEachBind, nullptr,
	                                        GitDiffTreeLocalInit);
	git_diff_tree_each_single.in_out_function = GitDiffTreeEachFunction;
	git_diff_tree_each_single.named_parameters["path"] = LogicalType::VARCHAR;
	git_diff_tree_each_single.named_parameters["untracked"] = LogicalType::BOOLEAN;
	git_diff_tree_each_set.AddFunction(git_diff_tree_each_single);

	TableFunction git_diff_tree_each_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, GitDiffTreeEachBind,
	                                     nullptr, GitDiffTreeLocalInit);
	git_diff_tree_each_two.in_out_function = GitDiffTreeEachFunction;
	git_diff_tree_each_two.named_parameters["path"] = LogicalType::VARCHAR;
	git_diff_tree_each_two.named_parameters["untracked"] = LogicalType::BOOLEAN;
	git_diff_tree_each_set.AddFunction(git_diff_tree_each_two);

	loader.RegisterFunction(git_diff_tree_each_set);
}

} // namespace duckdb

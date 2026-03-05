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
// GitStatusRow
//===--------------------------------------------------------------------===//

struct GitStatusRow {
	string repo_path;
	string file_path;
	string file_ext;
	string status;         // "modified", "added", "deleted", "renamed", "untracked", "ignored", "conflicted"
	uint32_t status_flags; // raw git_status_t bitmask
	bool staged;           // has index changes
	bool unstaged;         // has workdir changes
	string old_path;       // rename/copy source (empty otherwise)
};

//===--------------------------------------------------------------------===//
// Bind Data
//===--------------------------------------------------------------------===//

struct GitStatusFunctionData : public TableFunctionData {
	string repo_path;
	bool include_untracked;
	bool include_ignored;
	string path_filter;
	vector<GitStatusRow> rows;
	bool is_lateral;

	GitStatusFunctionData(const string &repo_path, bool include_untracked, bool include_ignored,
	                      const string &path_filter, bool is_lateral = false)
	    : repo_path(repo_path), include_untracked(include_untracked), include_ignored(include_ignored),
	      path_filter(path_filter), is_lateral(is_lateral) {
	}
};

//===--------------------------------------------------------------------===//
// Local State
//===--------------------------------------------------------------------===//

struct GitStatusLocalState : public LocalTableFunctionState {
	idx_t current_index = 0;

	// LATERAL processing state
	vector<GitStatusRow> current_rows;
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

static string StatusFlagsToString(unsigned int flags) {
	// Check for conflict first
	if (flags & GIT_STATUS_CONFLICTED) {
		return "conflicted";
	}

	// Check for rename (index or workdir)
	if ((flags & GIT_STATUS_INDEX_RENAMED) || (flags & GIT_STATUS_WT_RENAMED)) {
		return "renamed";
	}

	// Check for additions
	if ((flags & GIT_STATUS_INDEX_NEW) || (flags & GIT_STATUS_WT_NEW)) {
		if (flags & GIT_STATUS_WT_NEW) {
			return "untracked";
		}
		return "added";
	}

	// Check for deletions
	if ((flags & GIT_STATUS_INDEX_DELETED) || (flags & GIT_STATUS_WT_DELETED)) {
		return "deleted";
	}

	// Check for modifications
	if ((flags & GIT_STATUS_INDEX_MODIFIED) || (flags & GIT_STATUS_WT_MODIFIED)) {
		return "modified";
	}

	// Ignored
	if (flags & GIT_STATUS_IGNORED) {
		return "ignored";
	}

	// Check for type changes
	if ((flags & GIT_STATUS_INDEX_TYPECHANGE) || (flags & GIT_STATUS_WT_TYPECHANGE)) {
		return "modified";
	}

	return "unknown";
}

static void CollectStatusRows(git_repository *repo, const string &repo_path, bool include_untracked,
                              bool include_ignored, const string &path_filter, vector<GitStatusRow> &rows) {
	git_status_options opts = GIT_STATUS_OPTIONS_INIT;
	opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
	opts.flags = GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX | GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR |
	             GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;

	if (include_untracked) {
		opts.flags |= GIT_STATUS_OPT_INCLUDE_UNTRACKED;
	}
	if (include_ignored) {
		opts.flags |= GIT_STATUS_OPT_INCLUDE_IGNORED;
	}

	// Path filter (exact matching, not glob)
	char *pathspec_cstr = nullptr;
	if (!path_filter.empty()) {
		opts.flags |= GIT_STATUS_OPT_DISABLE_PATHSPEC_MATCH;
		opts.pathspec.count = 1;
		pathspec_cstr = const_cast<char *>(path_filter.c_str());
		opts.pathspec.strings = &pathspec_cstr;
	}

	git_status_list *status_list = nullptr;
	int error = git_status_list_new(&status_list, repo, &opts);
	if (error != 0) {
		const git_error *e = git_error_last();
		throw IOException("git_status: failed to get status: %s", e ? e->message : "Unknown error");
	}

	size_t count = git_status_list_entrycount(status_list);
	for (size_t i = 0; i < count; i++) {
		const git_status_entry *entry = git_status_byindex(status_list, i);
		if (!entry) {
			continue;
		}

		GitStatusRow row;
		row.repo_path = repo_path;
		row.status_flags = entry->status;
		row.status = StatusFlagsToString(entry->status);

		// Use status flags to determine staged/unstaged (not pointer presence)
		// This avoids marking untracked files as "unstaged"
		row.staged = (entry->status & (GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED | GIT_STATUS_INDEX_DELETED |
		                               GIT_STATUS_INDEX_RENAMED | GIT_STATUS_INDEX_TYPECHANGE)) != 0;
		row.unstaged = (entry->status & (GIT_STATUS_WT_MODIFIED | GIT_STATUS_WT_DELETED | GIT_STATUS_WT_TYPECHANGE |
		                                 GIT_STATUS_WT_RENAMED)) != 0;

		if (entry->head_to_index) {
			row.file_path = entry->head_to_index->new_file.path ? entry->head_to_index->new_file.path : "";
			if (entry->head_to_index->old_file.path && entry->head_to_index->status == GIT_DELTA_RENAMED) {
				row.old_path = entry->head_to_index->old_file.path;
			}
		}
		if (entry->index_to_workdir) {
			// Prefer workdir path if available (more current)
			string workdir_path = entry->index_to_workdir->new_file.path ? entry->index_to_workdir->new_file.path : "";
			if (!workdir_path.empty()) {
				row.file_path = workdir_path;
			}
			if (entry->index_to_workdir->old_file.path && entry->index_to_workdir->status == GIT_DELTA_RENAMED) {
				row.old_path = entry->index_to_workdir->old_file.path;
			}
		}

		row.file_ext = ExtractFileExtension(row.file_path);
		rows.push_back(std::move(row));
	}

	git_status_list_free(status_list);
}

//===--------------------------------------------------------------------===//
// Schema
//===--------------------------------------------------------------------===//

static void DefineGitStatusSchema(vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {
	    LogicalType::VARCHAR,  // repo_path
	    LogicalType::VARCHAR,  // file_path
	    LogicalType::VARCHAR,  // file_ext
	    LogicalType::VARCHAR,  // status
	    LogicalType::UINTEGER, // status_flags
	    LogicalType::BOOLEAN,  // staged
	    LogicalType::BOOLEAN,  // unstaged
	    LogicalType::VARCHAR   // old_path
	};
	names = {"repo_path", "file_path", "file_ext", "status", "status_flags", "staged", "unstaged", "old_path"};
}

static vector<LogicalType> GetGitStatusSchema() {
	vector<LogicalType> return_types;
	vector<string> names;
	DefineGitStatusSchema(return_types, names);
	return return_types;
}

static vector<string> GetGitStatusColumnNames() {
	vector<LogicalType> return_types;
	vector<string> names;
	DefineGitStatusSchema(return_types, names);
	return names;
}

static void OutputGitStatusRow(DataChunk &output, const GitStatusRow &row, idx_t row_idx) {
	output.SetValue(0, row_idx, Value(row.repo_path));
	output.SetValue(1, row_idx, Value(row.file_path));
	output.SetValue(2, row_idx, Value(row.file_ext));
	output.SetValue(3, row_idx, Value(row.status));
	output.SetValue(4, row_idx, Value::UINTEGER(row.status_flags));
	output.SetValue(5, row_idx, Value::BOOLEAN(row.staged));
	output.SetValue(6, row_idx, Value::BOOLEAN(row.unstaged));
	if (!row.old_path.empty()) {
		output.SetValue(7, row_idx, Value(row.old_path));
	} else {
		output.SetValue(7, row_idx, Value());
	}
}

//===--------------------------------------------------------------------===//
// Bind
//===--------------------------------------------------------------------===//

static unique_ptr<FunctionData> GitStatusBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	return_types = GetGitStatusSchema();
	names = GetGitStatusColumnNames();

	string repo_path = ".";
	bool include_untracked = true;
	bool include_ignored = false;
	string path_filter;

	// Parse positional parameters
	if (!input.inputs.empty()) {
		string first_param = input.inputs[0].GetValue<string>();
		// Use GitContextManager for repo discovery (but we only need the repo path, not a ref)
		// For git_status, we just need the repo root - no ref resolution needed
		try {
			auto git_path = GitPath::Parse("git://" + first_param + "@HEAD");
			repo_path = git_path.repository_path;
		} catch (const std::exception &e) {
			throw BinderException("git_status: failed to resolve repository path '%s': %s", first_param, e.what());
		}
	} else {
		// Zero-arg: discover repo from cwd
		try {
			auto git_path = GitPath::Parse("git://.@HEAD");
			repo_path = git_path.repository_path;
		} catch (const std::exception &e) {
			throw BinderException("git_status: failed to resolve repository: %s", e.what());
		}
	}

	// Parse named parameters
	for (const auto &kv : input.named_parameters) {
		if (kv.first == "untracked") {
			include_untracked = kv.second.GetValue<bool>();
		} else if (kv.first == "ignored") {
			include_ignored = kv.second.GetValue<bool>();
		} else if (kv.first == "path") {
			path_filter = kv.second.GetValue<string>();
		}
	}

	return make_uniq<GitStatusFunctionData>(repo_path, include_untracked, include_ignored, path_filter);
}

//===--------------------------------------------------------------------===//
// Init Global
//===--------------------------------------------------------------------===//

static unique_ptr<GlobalTableFunctionState> GitStatusInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = const_cast<GitStatusFunctionData &>(input.bind_data->Cast<GitStatusFunctionData>());

	if (!bind_data.is_lateral) {
		git_repository *repo = nullptr;
		int error = git_repository_open(&repo, bind_data.repo_path.c_str());
		if (error != 0) {
			const git_error *git_err = git_error_last();
			string error_msg = git_err ? git_err->message : "Unknown git error";
			throw BinderException("git_status: failed to open repository '%s': %s", bind_data.repo_path, error_msg);
		}

		try {
			CollectStatusRows(repo, bind_data.repo_path, bind_data.include_untracked, bind_data.include_ignored,
			                  bind_data.path_filter, bind_data.rows);
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

static void GitStatusFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<GitStatusFunctionData>();
	auto &local_state = data_p.local_state->Cast<GitStatusLocalState>();

	idx_t output_count = 0;
	const idx_t max_output = STANDARD_VECTOR_SIZE;

	while (local_state.current_index < bind_data.rows.size() && output_count < max_output) {
		auto &row = bind_data.rows[local_state.current_index];
		OutputGitStatusRow(output, row, output_count);
		local_state.current_index++;
		output_count++;
	}

	output.SetCardinality(output_count);
}

//===--------------------------------------------------------------------===//
// Local Init
//===--------------------------------------------------------------------===//

static unique_ptr<LocalTableFunctionState> GitStatusLocalInit(ExecutionContext &context, TableFunctionInitInput &input,
                                                              GlobalTableFunctionState *global_state) {
	return make_uniq<GitStatusLocalState>();
}

//===--------------------------------------------------------------------===//
// LATERAL (git_status_each)
//===--------------------------------------------------------------------===//

static unique_ptr<FunctionData> GitStatusEachBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	return_types = GetGitStatusSchema();
	names = GetGitStatusColumnNames();

	bool include_untracked = true;
	bool include_ignored = false;
	string path_filter;

	for (const auto &kv : input.named_parameters) {
		if (kv.first == "untracked") {
			include_untracked = kv.second.GetValue<bool>();
		} else if (kv.first == "ignored") {
			include_ignored = kv.second.GetValue<bool>();
		} else if (kv.first == "path") {
			path_filter = kv.second.GetValue<string>();
		}
	}

	return make_uniq<GitStatusFunctionData>(".", include_untracked, include_ignored, path_filter, true);
}

static OperatorResultType GitStatusEachFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                                DataChunk &output) {
	auto &state = data_p.local_state->Cast<GitStatusLocalState>();
	auto &bind_data = data_p.bind_data->Cast<GitStatusFunctionData>();

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
				CollectStatusRows(repo, resolved_repo_path, bind_data.include_untracked, bind_data.include_ignored,
				                  bind_data.path_filter, state.current_rows);
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
			OutputGitStatusRow(output, state.current_rows[state.current_output_row], output_count);
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
		// Zero rows for this input — continue to next input row
	}
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void RegisterGitStatusFunction(ExtensionLoader &loader) {
	TableFunctionSet git_status_set("git_status");

	// Zero parameters: git_status()
	TableFunction git_status_zero({}, GitStatusFunction, GitStatusBind, GitStatusInitGlobal);
	git_status_zero.init_local = GitStatusLocalInit;
	git_status_zero.named_parameters["untracked"] = LogicalType::BOOLEAN;
	git_status_zero.named_parameters["ignored"] = LogicalType::BOOLEAN;
	git_status_zero.named_parameters["path"] = LogicalType::VARCHAR;
	git_status_set.AddFunction(git_status_zero);

	// Single parameter: git_status(repo_path_or_uri)
	TableFunction git_status_single({LogicalType::VARCHAR}, GitStatusFunction, GitStatusBind, GitStatusInitGlobal);
	git_status_single.init_local = GitStatusLocalInit;
	git_status_single.named_parameters["untracked"] = LogicalType::BOOLEAN;
	git_status_single.named_parameters["ignored"] = LogicalType::BOOLEAN;
	git_status_single.named_parameters["path"] = LogicalType::VARCHAR;
	git_status_set.AddFunction(git_status_single);

	loader.RegisterFunction(git_status_set);

	// LATERAL: git_status_each
	TableFunctionSet git_status_each_set("git_status_each");

	TableFunction git_status_each_single({LogicalType::VARCHAR}, nullptr, GitStatusEachBind, nullptr,
	                                     GitStatusLocalInit);
	git_status_each_single.in_out_function = GitStatusEachFunction;
	git_status_each_single.named_parameters["untracked"] = LogicalType::BOOLEAN;
	git_status_each_single.named_parameters["ignored"] = LogicalType::BOOLEAN;
	git_status_each_single.named_parameters["path"] = LogicalType::VARCHAR;
	git_status_each_set.AddFunction(git_status_each_single);

	loader.RegisterFunction(git_status_each_set);
}

} // namespace duckdb

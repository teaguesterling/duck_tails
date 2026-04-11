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
// Helpers
//===--------------------------------------------------------------------===//

static string ExtractFileExtension(const string &path) {
	size_t dot_pos = path.find_last_of('.');
	if (dot_pos == string::npos || dot_pos == path.length() - 1) {
		return "";
	}
	return path.substr(dot_pos);
}

static string OidToHex(const git_oid *oid) {
	char hex[GIT_OID_HEXSZ + 1];
	git_oid_tostr(hex, sizeof(hex), oid);
	return string(hex);
}

//===--------------------------------------------------------------------===//
// GitBlameRow — covers both per-line and per-hunk output shapes.
//===--------------------------------------------------------------------===//

struct GitBlameRow {
	// Identity (populated by caller, not CollectBlameRows)
	string repo_path;
	string file_path;
	string file_ext;
	string revision;

	// Per-line fields (git_blame)
	int64_t line_number = 0;
	string line_content;
	bool line_content_is_null = false;

	// Per-hunk fields (git_blame_hunks)
	int64_t start_line = 0;
	int64_t line_count = 0;
	int64_t orig_start_line = 0;

	// Shared fields
	string commit_hash;
	string author_name;
	string author_email;
	timestamp_t author_date = timestamp_t(0);
	string orig_commit_hash;
	string orig_path;
	int64_t orig_line_number = 0;
	bool boundary = false;
};

//===--------------------------------------------------------------------===//
// GitBlameOptions — mirror of the blame-related named parameters.
//===--------------------------------------------------------------------===//

struct GitBlameOptions {
	int64_t min_line = 0; // 0 means "no lower bound"
	int64_t max_line = 0; // 0 means "no upper bound"
	bool ignore_whitespace = false;
	bool use_mailmap = false;
	bool first_parent = false;
};

//===--------------------------------------------------------------------===//
// Blame core
//===--------------------------------------------------------------------===//

// Loads the blob for `file_path` at `commit` and splits it into lines
// (strips trailing `\r`). Returns false if the blob is binary; `lines` is
// left empty in that case.
static bool LoadFileLines(git_repository *repo, git_commit *commit, const string &file_path, vector<string> &lines) {
	git_tree *tree = nullptr;
	int error = git_commit_tree(&tree, commit);
	if (error != 0) {
		const git_error *e = git_error_last();
		throw IOException("git_blame: failed to get commit tree: %s", e ? e->message : "unknown error");
	}

	git_tree_entry *entry = nullptr;
	error = git_tree_entry_bypath(&entry, tree, file_path.c_str());
	if (error != 0) {
		git_tree_free(tree);
		throw IOException("git_blame: file not found '%s' at revision", file_path);
	}

	git_blob *blob = nullptr;
	error = git_blob_lookup(&blob, repo, git_tree_entry_id(entry));
	if (error != 0) {
		git_tree_entry_free(entry);
		git_tree_free(tree);
		throw IOException("git_blame: failed to load blob for '%s'", file_path);
	}

	bool is_binary = git_blob_is_binary(blob) != 0;
	if (!is_binary) {
		const char *data = static_cast<const char *>(git_blob_rawcontent(blob));
		size_t size = static_cast<size_t>(git_blob_rawsize(blob));
		size_t start = 0;
		for (size_t i = 0; i < size; i++) {
			if (data[i] == '\n') {
				size_t end = i;
				if (end > start && data[end - 1] == '\r') {
					end--;
				}
				lines.emplace_back(data + start, end - start);
				start = i + 1;
			}
		}
		if (start < size) {
			size_t end = size;
			if (end > start && data[end - 1] == '\r') {
				end--;
			}
			lines.emplace_back(data + start, end - start);
		}
	}

	git_blob_free(blob);
	git_tree_entry_free(entry);
	git_tree_free(tree);
	return !is_binary;
}

// Computes blame rows for (repo, file_path) at `revision`. If `per_line` is
// true, emits one row per line with `line_content`; otherwise emits one row
// per hunk with `start_line`/`line_count`.
static void CollectBlameRows(git_repository *repo, const string &repo_path, const string &file_path,
                             const string &revision, const GitBlameOptions &opts, bool per_line,
                             vector<GitBlameRow> &rows) {
	git_object *rev_obj = nullptr;
	int error = git_revparse_single(&rev_obj, repo, revision.c_str());
	if (error != 0) {
		const git_error *e = git_error_last();
		throw IOException("git_blame: unable to resolve revision '%s': %s", revision, e ? e->message : "unknown error");
	}

	git_commit *commit = nullptr;
	error = git_object_peel(reinterpret_cast<git_object **>(&commit), rev_obj, GIT_OBJECT_COMMIT);
	if (error != 0) {
		git_object_free(rev_obj);
		throw IOException("git_blame: revision '%s' does not resolve to a commit", revision);
	}

	// Build blame options.
	git_blame_options blame_opts = GIT_BLAME_OPTIONS_INIT;
	git_oid_cpy(&blame_opts.newest_commit, git_commit_id(commit));
	if (opts.min_line > 0) {
		blame_opts.min_line = static_cast<size_t>(opts.min_line);
	}
	if (opts.max_line > 0) {
		blame_opts.max_line = static_cast<size_t>(opts.max_line);
	}
	uint32_t flags = 0;
	if (opts.ignore_whitespace) {
		flags |= GIT_BLAME_IGNORE_WHITESPACE;
	}
	if (opts.use_mailmap) {
		flags |= GIT_BLAME_USE_MAILMAP;
	}
	if (opts.first_parent) {
		flags |= GIT_BLAME_FIRST_PARENT;
	}
	blame_opts.flags = flags;

	git_blame *blame = nullptr;
	error = git_blame_file(&blame, repo, file_path.c_str(), &blame_opts);
	if (error != 0) {
		const git_error *e = git_error_last();
		git_commit_free(commit);
		git_object_free(rev_obj);
		throw IOException("git_blame: blame failed for '%s': %s", file_path, e ? e->message : "unknown error");
	}

	// Load file lines if we need line_content.
	vector<string> file_lines;
	bool have_text = false;
	if (per_line) {
		try {
			have_text = LoadFileLines(repo, commit, file_path, file_lines);
		} catch (...) {
			git_blame_free(blame);
			git_commit_free(commit);
			git_object_free(rev_obj);
			throw;
		}
	}

	string file_ext = ExtractFileExtension(file_path);
	uint32_t hunk_count = git_blame_get_hunk_count(blame);
	for (uint32_t i = 0; i < hunk_count; i++) {
		const git_blame_hunk *hunk = git_blame_get_hunk_byindex(blame, i);
		if (!hunk) {
			continue;
		}

		string commit_hash = OidToHex(&hunk->final_commit_id);
		string orig_commit_hash = OidToHex(&hunk->orig_commit_id);
		string author_name = hunk->final_signature && hunk->final_signature->name ? hunk->final_signature->name : "";
		string author_email =
		    hunk->final_signature && hunk->final_signature->email ? hunk->final_signature->email : "";
		timestamp_t author_date = timestamp_t(0);
		if (hunk->final_signature) {
			// libgit2 git_time is seconds since epoch (UTC).
			author_date = Timestamp::FromEpochSeconds(hunk->final_signature->when.time);
		}
		string orig_path = hunk->orig_path ? string(hunk->orig_path) : file_path;
		bool boundary = hunk->boundary != 0;

		if (per_line) {
			for (size_t offset = 0; offset < hunk->lines_in_hunk; offset++) {
				int64_t line_no = static_cast<int64_t>(hunk->final_start_line_number + offset);
				GitBlameRow row;
				row.repo_path = repo_path;
				row.file_path = file_path;
				row.file_ext = file_ext;
				row.revision = revision;
				row.line_number = line_no;
				if (have_text && line_no >= 1 && static_cast<size_t>(line_no) <= file_lines.size()) {
					row.line_content = file_lines[line_no - 1];
					row.line_content_is_null = false;
				} else {
					row.line_content_is_null = true;
				}
				row.commit_hash = commit_hash;
				row.author_name = author_name;
				row.author_email = author_email;
				row.author_date = author_date;
				row.orig_commit_hash = orig_commit_hash;
				row.orig_path = orig_path;
				row.orig_line_number = static_cast<int64_t>(hunk->orig_start_line_number + offset);
				row.boundary = boundary;
				rows.push_back(std::move(row));
			}
		} else {
			GitBlameRow row;
			row.repo_path = repo_path;
			row.file_path = file_path;
			row.file_ext = file_ext;
			row.revision = revision;
			row.start_line = static_cast<int64_t>(hunk->final_start_line_number);
			row.line_count = static_cast<int64_t>(hunk->lines_in_hunk);
			row.orig_start_line = static_cast<int64_t>(hunk->orig_start_line_number);
			row.commit_hash = commit_hash;
			row.author_name = author_name;
			row.author_email = author_email;
			row.author_date = author_date;
			row.orig_commit_hash = orig_commit_hash;
			row.orig_path = orig_path;
			row.boundary = boundary;
			rows.push_back(std::move(row));
		}
	}

	git_blame_free(blame);
	git_commit_free(commit);
	git_object_free(rev_obj);
}

//===--------------------------------------------------------------------===//
// Schemas
//===--------------------------------------------------------------------===//

static void DefineHunksSchema(vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {LogicalType::VARCHAR,   LogicalType::VARCHAR,   LogicalType::VARCHAR,  LogicalType::VARCHAR,
	                LogicalType::BIGINT,    LogicalType::BIGINT,    LogicalType::VARCHAR,  LogicalType::VARCHAR,
	                LogicalType::VARCHAR,   LogicalType::TIMESTAMP, LogicalType::VARCHAR,  LogicalType::VARCHAR,
	                LogicalType::BIGINT,    LogicalType::BOOLEAN};
	names = {"repo_path",        "file_path",   "file_ext",         "revision",     "start_line",
	         "line_count",       "commit_hash", "author_name",      "author_email", "author_date",
	         "orig_commit_hash", "orig_path",   "orig_start_line",  "boundary"};
}

static void OutputHunkRow(DataChunk &output, const GitBlameRow &row, idx_t row_idx) {
	output.SetValue(0, row_idx, Value(row.repo_path));
	output.SetValue(1, row_idx, Value(row.file_path));
	output.SetValue(2, row_idx, Value(row.file_ext));
	output.SetValue(3, row_idx, Value(row.revision));
	output.SetValue(4, row_idx, Value::BIGINT(row.start_line));
	output.SetValue(5, row_idx, Value::BIGINT(row.line_count));
	output.SetValue(6, row_idx, Value(row.commit_hash));
	output.SetValue(7, row_idx, Value(row.author_name));
	output.SetValue(8, row_idx, Value(row.author_email));
	output.SetValue(9, row_idx, Value::TIMESTAMP(row.author_date));
	output.SetValue(10, row_idx, Value(row.orig_commit_hash));
	output.SetValue(11, row_idx, Value(row.orig_path));
	output.SetValue(12, row_idx, Value::BIGINT(row.orig_start_line));
	output.SetValue(13, row_idx, Value::BOOLEAN(row.boundary));
}

//===--------------------------------------------------------------------===//
// Bind data — single struct used for all four functions.
//===--------------------------------------------------------------------===//

struct GitBlameBindData : public TableFunctionData {
	string repo_path;
	string file_path;
	string revision;
	GitBlameOptions opts;
	bool per_line = true;  // false for *_hunks variants
	bool is_lateral = false;
	vector<GitBlameRow> rows; // Materialized at bind time for static forms
};

struct GitBlameLocalState : public LocalTableFunctionState {
	idx_t current_index = 0;

	// LATERAL processing state (unused in static form)
	vector<GitBlameRow> current_rows;
	idx_t current_input_row = 0;
	idx_t current_output_row = 0;
	bool initialized_row = false;
};

//===--------------------------------------------------------------------===//
// git_blame_hunks static bind + exec
//===--------------------------------------------------------------------===//

static unique_ptr<FunctionData> GitBlameHunksBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	DefineHunksSchema(return_types, names);

	if (input.inputs.empty()) {
		throw BinderException("git_blame_hunks requires a file path as its first argument");
	}

	auto params = ParseUnifiedGitParams(input, /*ref_param_index=*/999);
	if (params.resolved_file_path.empty()) {
		throw BinderException("git_blame_hunks: '%s' must refer to a file inside a git repository",
		                      params.repo_path_or_uri);
	}

	auto bind_data = make_uniq<GitBlameBindData>();
	bind_data->repo_path = params.resolved_repo_path;
	bind_data->file_path = params.resolved_file_path;
	bind_data->revision = params.ref;
	bind_data->per_line = false;

	git_repository *repo = nullptr;
	int error = git_repository_open(&repo, bind_data->repo_path.c_str());
	if (error != 0) {
		const git_error *e = git_error_last();
		throw BinderException("git_blame_hunks: failed to open repository '%s': %s", bind_data->repo_path,
		                      e ? e->message : "unknown error");
	}

	try {
		CollectBlameRows(repo, bind_data->repo_path, bind_data->file_path, bind_data->revision, bind_data->opts,
		                 /*per_line=*/false, bind_data->rows);
	} catch (...) {
		git_repository_free(repo);
		throw;
	}
	git_repository_free(repo);

	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> GitBlameInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<GlobalTableFunctionState>();
}

static unique_ptr<LocalTableFunctionState>
GitBlameLocalInit(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state) {
	return make_uniq<GitBlameLocalState>();
}

static void GitBlameHunksFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<GitBlameBindData>();
	auto &local_state = data_p.local_state->Cast<GitBlameLocalState>();

	idx_t output_count = 0;
	while (local_state.current_index < bind_data.rows.size() && output_count < STANDARD_VECTOR_SIZE) {
		OutputHunkRow(output, bind_data.rows[local_state.current_index], output_count);
		local_state.current_index++;
		output_count++;
	}
	output.SetCardinality(output_count);
}

//===--------------------------------------------------------------------===//
// git_blame per-line schema + exec
//===--------------------------------------------------------------------===//

static void DefineBlameSchema(vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {LogicalType::VARCHAR,   LogicalType::VARCHAR,   LogicalType::VARCHAR,  LogicalType::VARCHAR,
	                LogicalType::BIGINT,    LogicalType::VARCHAR,   LogicalType::VARCHAR,  LogicalType::VARCHAR,
	                LogicalType::VARCHAR,   LogicalType::TIMESTAMP, LogicalType::VARCHAR,  LogicalType::VARCHAR,
	                LogicalType::BIGINT,    LogicalType::BOOLEAN};
	names = {"repo_path",        "file_path",   "file_ext",         "revision",     "line_number",
	         "line_content",     "commit_hash", "author_name",      "author_email", "author_date",
	         "orig_commit_hash", "orig_path",   "orig_line_number", "boundary"};
}

static void OutputBlameRow(DataChunk &output, const GitBlameRow &row, idx_t row_idx) {
	output.SetValue(0, row_idx, Value(row.repo_path));
	output.SetValue(1, row_idx, Value(row.file_path));
	output.SetValue(2, row_idx, Value(row.file_ext));
	output.SetValue(3, row_idx, Value(row.revision));
	output.SetValue(4, row_idx, Value::BIGINT(row.line_number));
	if (row.line_content_is_null) {
		output.SetValue(5, row_idx, Value());
	} else {
		output.SetValue(5, row_idx, Value(row.line_content));
	}
	output.SetValue(6, row_idx, Value(row.commit_hash));
	output.SetValue(7, row_idx, Value(row.author_name));
	output.SetValue(8, row_idx, Value(row.author_email));
	output.SetValue(9, row_idx, Value::TIMESTAMP(row.author_date));
	output.SetValue(10, row_idx, Value(row.orig_commit_hash));
	output.SetValue(11, row_idx, Value(row.orig_path));
	output.SetValue(12, row_idx, Value::BIGINT(row.orig_line_number));
	output.SetValue(13, row_idx, Value::BOOLEAN(row.boundary));
}

static unique_ptr<FunctionData> GitBlameBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	DefineBlameSchema(return_types, names);

	if (input.inputs.empty()) {
		throw BinderException("git_blame requires a file path as its first argument");
	}

	auto params = ParseUnifiedGitParams(input, /*ref_param_index=*/999);
	if (params.resolved_file_path.empty()) {
		throw BinderException("git_blame: '%s' must refer to a file inside a git repository",
		                      params.repo_path_or_uri);
	}

	auto bind_data = make_uniq<GitBlameBindData>();
	bind_data->repo_path = params.resolved_repo_path;
	bind_data->file_path = params.resolved_file_path;
	bind_data->revision = params.ref;
	bind_data->per_line = true;

	git_repository *repo = nullptr;
	int error = git_repository_open(&repo, bind_data->repo_path.c_str());
	if (error != 0) {
		const git_error *e = git_error_last();
		throw BinderException("git_blame: failed to open repository '%s': %s", bind_data->repo_path,
		                      e ? e->message : "unknown error");
	}
	try {
		CollectBlameRows(repo, bind_data->repo_path, bind_data->file_path, bind_data->revision, bind_data->opts,
		                 /*per_line=*/true, bind_data->rows);
	} catch (...) {
		git_repository_free(repo);
		throw;
	}
	git_repository_free(repo);

	return std::move(bind_data);
}

static void GitBlameFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<GitBlameBindData>();
	auto &local_state = data_p.local_state->Cast<GitBlameLocalState>();

	idx_t output_count = 0;
	while (local_state.current_index < bind_data.rows.size() && output_count < STANDARD_VECTOR_SIZE) {
		OutputBlameRow(output, bind_data.rows[local_state.current_index], output_count);
		local_state.current_index++;
		output_count++;
	}
	output.SetCardinality(output_count);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void RegisterGitBlameFunction(ExtensionLoader &loader) {
	TableFunctionSet git_blame_hunks_set("git_blame_hunks");
	TableFunction hunks_one({LogicalType::VARCHAR}, GitBlameHunksFunction, GitBlameHunksBind, GitBlameInitGlobal);
	hunks_one.init_local = GitBlameLocalInit;
	git_blame_hunks_set.AddFunction(hunks_one);
	loader.RegisterFunction(git_blame_hunks_set);

	TableFunctionSet git_blame_set("git_blame");
	TableFunction blame_one({LogicalType::VARCHAR}, GitBlameFunction, GitBlameBind, GitBlameInitGlobal);
	blame_one.init_local = GitBlameLocalInit;
	git_blame_set.AddFunction(blame_one);
	loader.RegisterFunction(git_blame_set);
}

} // namespace duckdb

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
		string author_email = hunk->final_signature && hunk->final_signature->email ? hunk->final_signature->email : "";
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
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,   LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BIGINT,  LogicalType::BIGINT,    LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::TIMESTAMP, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BIGINT,  LogicalType::BOOLEAN};
	names = {"repo_path",        "file_path",   "file_ext",        "revision",     "start_line",
	         "line_count",       "commit_hash", "author_name",     "author_email", "author_date",
	         "orig_commit_hash", "orig_path",   "orig_start_line", "boundary"};
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
	bool per_line = true; // false for *_hunks variants
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

// Raise if min_line/max_line are out of range. Internally, 0 means "unset"
// (GIT_BLAME_OPTIONS_INIT defaults); the user-facing minimum is 1.
static void ValidateBlameOptions(const GitBlameOptions &opts, const char *func_name) {
	if (opts.min_line < 0) {
		throw BinderException("%s: min_line must be >= 1", func_name);
	}
	if (opts.max_line < 0) {
		throw BinderException("%s: max_line must be >= 1", func_name);
	}
	if (opts.min_line > 0 && opts.max_line > 0 && opts.min_line > opts.max_line) {
		throw BinderException("%s: min_line (%lld) must be <= max_line (%lld)", func_name, (long long)opts.min_line,
		                      (long long)opts.max_line);
	}
}

// Extract the named parameters shared by every git_blame* static form.
// Mutates `bind_data` and returns the overridden repo_path/revision for the
// caller to apply *after* initial positional parsing.
static void ApplyBlameNamedParams(const TableFunctionBindInput &input, GitBlameBindData &bind_data,
                                  string &override_repo_path, string &override_revision) {
	for (const auto &kv : input.named_parameters) {
		if (kv.first == "repo_path") {
			override_repo_path = kv.second.GetValue<string>();
		} else if (kv.first == "revision") {
			override_revision = kv.second.GetValue<string>();
		} else if (kv.first == "min_line") {
			int64_t v = kv.second.GetValue<int64_t>();
			if (v <= 0) {
				throw BinderException("git_blame: min_line must be >= 1");
			}
			bind_data.opts.min_line = v;
		} else if (kv.first == "max_line") {
			int64_t v = kv.second.GetValue<int64_t>();
			if (v <= 0) {
				throw BinderException("git_blame: max_line must be >= 1");
			}
			bind_data.opts.max_line = v;
		} else if (kv.first == "ignore_whitespace") {
			bind_data.opts.ignore_whitespace = kv.second.GetValue<bool>();
		} else if (kv.first == "use_mailmap") {
			bind_data.opts.use_mailmap = kv.second.GetValue<bool>();
		} else if (kv.first == "first_parent") {
			bind_data.opts.first_parent = kv.second.GetValue<bool>();
		}
	}
}

static unique_ptr<FunctionData> GitBlameHunksBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	DefineHunksSchema(return_types, names);

	if (input.inputs.empty()) {
		throw BinderException("git_blame_hunks requires a file path as its first argument");
	}

	auto bind_data = make_uniq<GitBlameBindData>();
	bind_data->per_line = false;

	string override_repo_path;
	string override_revision;
	ApplyBlameNamedParams(input, *bind_data, override_repo_path, override_revision);
	ValidateBlameOptions(bind_data->opts, "git_blame_hunks");

	string first_param = input.inputs[0].GetValue<string>();
	string resolved_repo_path;
	string resolved_file_path;
	string resolved_revision;

	if (!override_repo_path.empty()) {
		// User provided repo_path explicitly; first positional is the file path within it.
		string uri = "git://" + override_repo_path + "/" + first_param + "@HEAD";
		auto ctx = GitContextManager::Instance().ProcessGitUri(uri, "HEAD");
		resolved_repo_path = ctx.repo_path;
		resolved_file_path = ctx.file_path;
		resolved_revision = "HEAD";
	} else {
		auto params = ParseUnifiedGitParams(input, /*ref_param_index=*/999);
		if (params.resolved_file_path.empty()) {
			throw BinderException("git_blame_hunks: '%s' must refer to a file inside a git repository",
			                      params.repo_path_or_uri);
		}
		resolved_repo_path = params.resolved_repo_path;
		resolved_file_path = params.resolved_file_path;
		resolved_revision = params.ref;
	}

	if (!override_revision.empty()) {
		resolved_revision = override_revision;
	}

	bind_data->repo_path = resolved_repo_path;
	bind_data->file_path = resolved_file_path;
	bind_data->revision = resolved_revision;

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

static unique_ptr<LocalTableFunctionState> GitBlameLocalInit(ExecutionContext &context, TableFunctionInitInput &input,
                                                             GlobalTableFunctionState *global_state) {
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
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,   LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BIGINT,  LogicalType::VARCHAR,   LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::TIMESTAMP, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BIGINT,  LogicalType::BOOLEAN};
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

	auto bind_data = make_uniq<GitBlameBindData>();
	bind_data->per_line = true;

	string override_repo_path;
	string override_revision;
	ApplyBlameNamedParams(input, *bind_data, override_repo_path, override_revision);
	ValidateBlameOptions(bind_data->opts, "git_blame");

	string first_param = input.inputs[0].GetValue<string>();
	string resolved_repo_path;
	string resolved_file_path;
	string resolved_revision;

	if (!override_repo_path.empty()) {
		string uri = "git://" + override_repo_path + "/" + first_param + "@HEAD";
		auto ctx = GitContextManager::Instance().ProcessGitUri(uri, "HEAD");
		resolved_repo_path = ctx.repo_path;
		resolved_file_path = ctx.file_path;
		resolved_revision = "HEAD";
	} else {
		auto params = ParseUnifiedGitParams(input, /*ref_param_index=*/999);
		if (params.resolved_file_path.empty()) {
			throw BinderException("git_blame: '%s' must refer to a file inside a git repository",
			                      params.repo_path_or_uri);
		}
		resolved_repo_path = params.resolved_repo_path;
		resolved_file_path = params.resolved_file_path;
		resolved_revision = params.ref;
	}

	if (!override_revision.empty()) {
		resolved_revision = override_revision;
	}

	bind_data->repo_path = resolved_repo_path;
	bind_data->file_path = resolved_file_path;
	bind_data->revision = resolved_revision;

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
// LATERAL variants
//===--------------------------------------------------------------------===//

// Shared bind for LATERAL forms: stores bind-time named parameters in bind
// data; the runtime file path/URI and optional revision come from the input
// DataChunk per row.
static unique_ptr<FunctionData> GitBlameLateralBind(ClientContext &context, TableFunctionBindInput &input,
                                                    bool per_line) {
	auto bind_data = make_uniq<GitBlameBindData>();
	bind_data->per_line = per_line;
	bind_data->is_lateral = true;
	bind_data->revision = "HEAD";

	string override_repo_path; // ignored for LATERAL (runtime provides path)
	string override_revision;
	ApplyBlameNamedParams(input, *bind_data, override_repo_path, override_revision);
	ValidateBlameOptions(bind_data->opts, per_line ? "git_blame_each" : "git_blame_hunks_each");

	if (!override_revision.empty()) {
		bind_data->revision = override_revision;
	}
	return std::move(bind_data);
}

static unique_ptr<FunctionData> GitBlameHunksEachBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<string> &names) {
	DefineHunksSchema(return_types, names);
	return GitBlameLateralBind(context, input, /*per_line=*/false);
}

// Resolve a LATERAL input path (URI or plain path) + optional per-row revision
// override into (repo_path, file_path, revision).
static void ResolveLateralInput(const string &raw_input, const string &bind_revision, const string &row_revision,
                                string &out_repo_path, string &out_file_path, string &out_revision) {
	// If the input is a git:// URI, let GitContextManager parse it fully.
	if (StringUtil::StartsWith(raw_input, "git://")) {
		auto ctx = GitContextManager::Instance().ProcessGitUri(raw_input, "HEAD");
		out_repo_path = ctx.repo_path;
		out_file_path = ctx.file_path;
		// URI @rev wins unless the caller passed an explicit row revision.
		if (!row_revision.empty()) {
			out_revision = row_revision;
		} else if (!ctx.final_ref.empty() && ctx.final_ref != "HEAD") {
			out_revision = ctx.final_ref;
		} else {
			out_revision = bind_revision;
		}
		return;
	}

	// Plain path — discover repo via GitContextManager.
	auto ctx = GitContextManager::Instance().ProcessGitUri("git://" + raw_input + "@HEAD", "HEAD");
	out_repo_path = ctx.repo_path;
	out_file_path = ctx.file_path;
	out_revision = row_revision.empty() ? bind_revision : row_revision;
}

static OperatorResultType GitBlameEachImpl(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                           DataChunk &output, bool per_line) {
	auto &state = data_p.local_state->Cast<GitBlameLocalState>();
	auto &bind_data = data_p.bind_data->Cast<GitBlameBindData>();

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

			auto file_vec = FlatVector::GetData<string_t>(input.data[0]);
			string raw_input(file_vec[state.current_input_row].GetData(), file_vec[state.current_input_row].GetSize());

			string row_revision;
			if (input.ColumnCount() > 1 && !FlatVector::IsNull(input.data[1], state.current_input_row)) {
				auto rev_vec = FlatVector::GetData<string_t>(input.data[1]);
				row_revision =
				    string(rev_vec[state.current_input_row].GetData(), rev_vec[state.current_input_row].GetSize());
			}

			string repo_path, file_path, revision;
			try {
				ResolveLateralInput(raw_input, bind_data.revision, row_revision, repo_path, file_path, revision);
			} catch (...) {
				state.current_input_row++;
				continue;
			}

			state.current_rows.clear();
			git_repository *repo = nullptr;
			int error = git_repository_open(&repo, repo_path.c_str());
			if (error != 0) {
				state.current_input_row++;
				continue;
			}
			try {
				// Populate identity fields on each row via caller post-process below.
				CollectBlameRows(repo, repo_path, file_path, revision, bind_data.opts, per_line, state.current_rows);
			} catch (...) {
				git_repository_free(repo);
				state.current_input_row++;
				continue;
			}
			git_repository_free(repo);

			state.initialized_row = true;
			state.current_output_row = 0;
		}

		idx_t output_count = 0;
		while (output_count < STANDARD_VECTOR_SIZE && state.current_output_row < state.current_rows.size()) {
			if (per_line) {
				OutputBlameRow(output, state.current_rows[state.current_output_row], output_count);
			} else {
				OutputHunkRow(output, state.current_rows[state.current_output_row], output_count);
			}
			state.current_output_row++;
			output_count++;
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

static OperatorResultType GitBlameHunksEachFunction(ExecutionContext &context, TableFunctionInput &data_p,
                                                    DataChunk &input, DataChunk &output) {
	return GitBlameEachImpl(context, data_p, input, output, /*per_line=*/false);
}

static unique_ptr<FunctionData> GitBlameEachBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	DefineBlameSchema(return_types, names);
	return GitBlameLateralBind(context, input, /*per_line=*/true);
}

static OperatorResultType GitBlameEachFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                               DataChunk &output) {
	return GitBlameEachImpl(context, data_p, input, output, /*per_line=*/true);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void RegisterGitBlameFunction(ExtensionLoader &loader) {
	auto declare_named_params = [](TableFunction &fn) {
		fn.named_parameters["repo_path"] = LogicalType::VARCHAR;
		fn.named_parameters["revision"] = LogicalType::VARCHAR;
		fn.named_parameters["min_line"] = LogicalType::BIGINT;
		fn.named_parameters["max_line"] = LogicalType::BIGINT;
		fn.named_parameters["ignore_whitespace"] = LogicalType::BOOLEAN;
		fn.named_parameters["use_mailmap"] = LogicalType::BOOLEAN;
		fn.named_parameters["first_parent"] = LogicalType::BOOLEAN;
	};

	TableFunctionSet git_blame_hunks_set("git_blame_hunks");
	TableFunction hunks_one({LogicalType::VARCHAR}, GitBlameHunksFunction, GitBlameHunksBind, GitBlameInitGlobal);
	hunks_one.init_local = GitBlameLocalInit;
	declare_named_params(hunks_one);
	git_blame_hunks_set.AddFunction(hunks_one);
	loader.RegisterFunction(git_blame_hunks_set);

	TableFunctionSet git_blame_set("git_blame");
	TableFunction blame_one({LogicalType::VARCHAR}, GitBlameFunction, GitBlameBind, GitBlameInitGlobal);
	blame_one.init_local = GitBlameLocalInit;
	declare_named_params(blame_one);
	git_blame_set.AddFunction(blame_one);
	loader.RegisterFunction(git_blame_set);

	auto declare_lateral_params = [](TableFunction &fn) {
		fn.named_parameters["revision"] = LogicalType::VARCHAR;
		fn.named_parameters["min_line"] = LogicalType::BIGINT;
		fn.named_parameters["max_line"] = LogicalType::BIGINT;
		fn.named_parameters["ignore_whitespace"] = LogicalType::BOOLEAN;
		fn.named_parameters["use_mailmap"] = LogicalType::BOOLEAN;
		fn.named_parameters["first_parent"] = LogicalType::BOOLEAN;
	};

	TableFunctionSet git_blame_hunks_each_set("git_blame_hunks_each");
	TableFunction hunks_each_one({LogicalType::VARCHAR}, nullptr, GitBlameHunksEachBind, GitBlameInitGlobal,
	                             GitBlameLocalInit);
	hunks_each_one.in_out_function = GitBlameHunksEachFunction;
	declare_lateral_params(hunks_each_one);
	git_blame_hunks_each_set.AddFunction(hunks_each_one);

	TableFunction hunks_each_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, GitBlameHunksEachBind,
	                             GitBlameInitGlobal, GitBlameLocalInit);
	hunks_each_two.in_out_function = GitBlameHunksEachFunction;
	declare_lateral_params(hunks_each_two);
	git_blame_hunks_each_set.AddFunction(hunks_each_two);

	loader.RegisterFunction(git_blame_hunks_each_set);

	TableFunctionSet git_blame_each_set("git_blame_each");
	TableFunction blame_each_one({LogicalType::VARCHAR}, nullptr, GitBlameEachBind, GitBlameInitGlobal,
	                             GitBlameLocalInit);
	blame_each_one.in_out_function = GitBlameEachFunction;
	declare_lateral_params(blame_each_one);
	git_blame_each_set.AddFunction(blame_each_one);

	TableFunction blame_each_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, GitBlameEachBind,
	                             GitBlameInitGlobal, GitBlameLocalInit);
	blame_each_two.in_out_function = GitBlameEachFunction;
	declare_lateral_params(blame_each_two);
	git_blame_each_set.AddFunction(blame_each_two);

	loader.RegisterFunction(git_blame_each_set);
}

} // namespace duckdb

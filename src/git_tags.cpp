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
// Helper Functions
//===--------------------------------------------------------------------===//

// Helper function to convert git OID to hex string
static string oid_to_hex(const git_oid *oid) {
	char hex[GIT_OID_HEXSZ + 1];
	git_oid_tostr(hex, sizeof(hex), oid);
	return string(hex);
}

static int tag_foreach_cb(const char *name, git_oid *oid, void *payload) {
	auto *tag_names = static_cast<vector<string> *>(payload);
	if (StringUtil::StartsWith(name, "refs/tags/")) {
		tag_names->push_back(name + 10); // Remove "refs/tags/" prefix
	}
	return 0;
}

//===--------------------------------------------------------------------===//
// Git Tags Function Data
//===--------------------------------------------------------------------===//

GitTagsFunctionData::GitTagsFunctionData(const string &repo_path, const string &resolved_repo_path)
    : repo_path(repo_path), resolved_repo_path(resolved_repo_path), ref("") {
}

GitTagsFunctionData::GitTagsFunctionData(const string &ref) : repo_path(""), resolved_repo_path(""), ref(ref) {
}

//===--------------------------------------------------------------------===//
// Local State Initialization
//===--------------------------------------------------------------------===//

unique_ptr<LocalTableFunctionState> GitTagsLocalInit(ExecutionContext &context, TableFunctionInitInput &input,
                                                     GlobalTableFunctionState *global_state) {
	return make_uniq<GitTagsLocalState>();
}

//===--------------------------------------------------------------------===//
// Git Tags Bind Function
//===--------------------------------------------------------------------===//

unique_ptr<FunctionData> GitTagsBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, vector<string> &names) {

	// Use unified parameter parsing to support both git:// URIs and filesystem paths
	auto params = ParseUnifiedGitParams(input, 1); // ref parameter at index 1 (optional)

	return_types = {
	    LogicalType::VARCHAR,   // repo_path
	    LogicalType::VARCHAR,   // tag_name
	    LogicalType::VARCHAR,   // commit_hash
	    LogicalType::VARCHAR,   // tag_hash
	    LogicalType::VARCHAR,   // tagger_name
	    LogicalType::TIMESTAMP, // tagger_date
	    LogicalType::VARCHAR,   // message
	    LogicalType::BOOLEAN    // is_annotated
	};

	names = {"repo_path",   "tag_name",    "commit_hash", "tag_hash",
	         "tagger_name", "tagger_date", "message",     "is_annotated"};

	try {
		auto ctx = GitContextManager::Instance().ProcessGitUri(params.repo_path_or_uri, params.ref);
		return make_uniq<GitTagsFunctionData>(params.repo_path_or_uri, ctx.repo_path);
	} catch (const std::exception &e) {
		throw BinderException("git_tags: %s", e.what());
	}
}

//===--------------------------------------------------------------------===//
// Git Tags Each Bind Function (Stub)
//===--------------------------------------------------------------------===//

unique_ptr<FunctionData> GitTagsEachBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {

	// For LATERAL functions, use ParseLateralGitParams to get optional ref parameter
	// The repo_path will come from runtime DataChunk, not bind time
	auto params = ParseLateralGitParams(input, 1); // ref parameter at index 1 (optional)

	// Set return schema (same as GitTagsBind)
	return_types = {
	    LogicalType::VARCHAR,   // repo_path
	    LogicalType::VARCHAR,   // tag_name
	    LogicalType::VARCHAR,   // commit_hash
	    LogicalType::VARCHAR,   // tag_hash
	    LogicalType::VARCHAR,   // tagger_name
	    LogicalType::TIMESTAMP, // tagger_date
	    LogicalType::VARCHAR,   // message
	    LogicalType::BOOLEAN    // is_annotated
	};

	names = {"repo_path",   "tag_name",    "commit_hash", "tag_hash",
	         "tagger_name", "tagger_date", "message",     "is_annotated"};

	// For LATERAL functions, store the ref parameter (defaults to "HEAD" from ParseLateralGitParams)
	return make_uniq<GitTagsFunctionData>(params.ref);
}

//===--------------------------------------------------------------------===//
// Git Tags Global Init Function
//===--------------------------------------------------------------------===//

unique_ptr<GlobalTableFunctionState> GitTagsInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<GlobalTableFunctionState>();
}

//===--------------------------------------------------------------------===//
// Git Tags Function
//===--------------------------------------------------------------------===//

void GitTagsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<GitTagsFunctionData>();
	auto &local_state = data_p.local_state->Cast<GitTagsLocalState>();

	if (!local_state.initialized) {
		int error = git_repository_open(&local_state.repo, bind_data.resolved_repo_path.c_str());
		if (error != 0) {
			const git_error *e = git_error_last();
			throw IOException("Failed to open git repository '%s': %s", bind_data.resolved_repo_path,
			                  e ? e->message : "Unknown error");
		}

		// Get all tags
		error = git_tag_foreach(local_state.repo, tag_foreach_cb, &local_state.tag_names);
		if (error != 0) {
			const git_error *e = git_error_last();
			throw IOException("Failed to list tags: %s", e ? e->message : "Unknown error");
		}

		local_state.initialized = true;
	}

	idx_t count = 0;

	while (count < STANDARD_VECTOR_SIZE && local_state.current_index < local_state.tag_names.size()) {
		const string &tag_name = local_state.tag_names[local_state.current_index];

		// Look up tag reference
		git_reference *tag_ref = nullptr;
		string full_name = "refs/tags/" + tag_name;
		int error = git_reference_lookup(&tag_ref, local_state.repo, full_name.c_str());
		if (error == 0) {
			// Set repo_path as first column
			output.SetValue(0, count, Value(bind_data.repo_path));
			output.SetValue(1, count, Value(tag_name));

			const git_oid *oid = git_reference_target(tag_ref);
			if (oid) {
				char tag_hash_str[GIT_OID_HEXSZ + 1];
				git_oid_tostr(tag_hash_str, sizeof(tag_hash_str), oid);
				output.SetValue(3, count, Value(tag_hash_str)); // tag_hash column

				// Try to get tag object for annotation info
				git_tag *tag_obj = nullptr;
				bool is_annotated = false;
				string commit_hash;

				if (git_tag_lookup(&tag_obj, local_state.repo, oid) == 0) {
					// Annotated tag - get the commit it points to
					is_annotated = true;
					const git_oid *target_oid = git_tag_target_id(tag_obj);
					if (target_oid) {
						char commit_hash_str[GIT_OID_HEXSZ + 1];
						git_oid_tostr(commit_hash_str, sizeof(commit_hash_str), target_oid);
						commit_hash = commit_hash_str;
					}

					const git_signature *tagger = git_tag_tagger(tag_obj);
					if (tagger) {
						output.SetValue(4, count, Value(tagger->name ? tagger->name : ""));
						timestamp_t tag_ts = Timestamp::FromEpochSeconds(tagger->when.time);
						output.SetValue(5, count, Value::TIMESTAMP(tag_ts));
					} else {
						output.SetValue(4, count, Value(""));
						output.SetValue(5, count, Value());
					}

					const char *message = git_tag_message(tag_obj);
					output.SetValue(6, count, Value(message ? message : ""));

					git_tag_free(tag_obj);
				} else {
					// Lightweight tag - tag_hash and commit_hash are the same
					commit_hash = tag_hash_str;
					output.SetValue(4, count, Value(""));
					output.SetValue(5, count, Value());
					output.SetValue(6, count, Value(""));
				}

				output.SetValue(2, count, Value(commit_hash)); // commit_hash column
				output.SetValue(7, count, Value::BOOLEAN(is_annotated));
			}

			git_reference_free(tag_ref);
			count++;
		}

		local_state.current_index++;
	}

	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// Helper function for LATERAL git_tags_each processing
//===--------------------------------------------------------------------===//

// Helper function for LATERAL git_tags_each processing - processes entire git tags for a repository path
// Uses cached repository from local_state for performance
static void ProcessTagsForInOut(git_repository *repo, const string &resolved_repo_path, vector<GitTagsRow> &rows) {
	// repo is from the per-thread cache in GitTagsLocalState

	vector<string> tag_names;

	// Get all tags
	int error = git_tag_foreach(repo, tag_foreach_cb, &tag_names);
	if (error != 0) {
		const git_error *e = git_error_last();
		throw IOException("Failed to list tags: %s", e ? e->message : "Unknown error");
	}

	// Process each tag
	for (const string &tag_name : tag_names) {
		GitTagsRow row;
		row.repo_path = resolved_repo_path;
		row.tag_name = tag_name;

		// Look up tag reference
		git_reference *tag_ref = nullptr;
		string full_name = "refs/tags/" + tag_name;
		error = git_reference_lookup(&tag_ref, repo, full_name.c_str());
		if (error == 0) {
			const git_oid *oid = git_reference_target(tag_ref);
			if (oid) {
				char tag_hash_str[GIT_OID_HEXSZ + 1];
				git_oid_tostr(tag_hash_str, sizeof(tag_hash_str), oid);
				row.tag_hash = tag_hash_str;

				// Try to get tag object for annotation info
				git_tag *tag_obj = nullptr;
				bool is_annotated = false;

				if (git_tag_lookup(&tag_obj, repo, oid) == 0) {
					// Annotated tag - get the commit it points to
					is_annotated = true;
					const git_oid *target_oid = git_tag_target_id(tag_obj);
					if (target_oid) {
						char commit_hash_str[GIT_OID_HEXSZ + 1];
						git_oid_tostr(commit_hash_str, sizeof(commit_hash_str), target_oid);
						row.commit_hash = commit_hash_str;
					}

					const git_signature *tagger = git_tag_tagger(tag_obj);
					if (tagger) {
						row.tagger_name = tagger->name ? tagger->name : "";
						row.tagger_date = Timestamp::FromEpochSeconds(tagger->when.time);
					} else {
						row.tagger_name = "";
						row.tagger_date = timestamp_t(0);
					}

					const char *message = git_tag_message(tag_obj);
					row.message = message ? message : "";

					git_tag_free(tag_obj);
					tag_obj = nullptr;
				} else {
					// Lightweight tag - tag_hash and commit_hash are the same
					row.commit_hash = tag_hash_str;
					row.tagger_name = "";
					row.tagger_date = timestamp_t(0);
					row.message = "";
				}

				row.is_annotated = is_annotated;
			} else {
				row.commit_hash = "";
				row.tag_hash = "";
				row.tagger_name = "";
				row.tagger_date = timestamp_t(0);
				row.message = "";
				row.is_annotated = false;
			}

			rows.push_back(row);
			git_reference_free(tag_ref);
			tag_ref = nullptr;
		}
	}
	// Note: Do NOT free repo here - it's cached in local_state
}

//===--------------------------------------------------------------------===//
// LATERAL git_tags_each function
//===--------------------------------------------------------------------===//

// LATERAL git_tags_each function - processes dynamic repository paths
static OperatorResultType GitTagsEachFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                              DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<GitTagsFunctionData>();
	auto &state = data_p.local_state->Cast<GitTagsLocalState>();

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
				throw BinderException("git_tags_each: no input columns available");
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
				throw BinderException("git_tags_each: no string data in input column");
			}

			// Get the string_t directly and convert to string
			auto string_t_value = data[state.current_input_row];
			string repo_path_or_uri(string_t_value.GetData(), string_t_value.GetSize());

			if (repo_path_or_uri.empty()) {
				throw BinderException("git_tags_each: received empty repo_path_or_uri from input");
			}

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
				// GitContextManager provides consistent error messages - skip problematic row and continue
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
					// Skip problematic repository and continue with next input row
					state.current_input_row++;
					state.initialized_row = false;
					continue;
				}

				state.cached_repo_path = resolved_repo_path;
			}

			// Process the git tags using the cached repository
			state.current_rows.clear();
			try {
				ProcessTagsForInOut(state.cached_repo, resolved_repo_path, state.current_rows);
			} catch (const std::exception &e) {
				// If ProcessTagsForInOut fails, skip this row and continue with next input
				state.current_input_row++;
				state.initialized_row = false;
				continue;
			}

			state.initialized_row = true;
			state.current_output_row = 0;
		}

		idx_t output_count = 0;
		while (output_count < STANDARD_VECTOR_SIZE && state.current_output_row < state.current_rows.size()) {

			auto &row = state.current_rows[state.current_output_row];

			output.SetValue(0, output_count, Value(resolved_repo_path));
			output.SetValue(1, output_count, Value(row.tag_name));
			output.SetValue(2, output_count, Value(row.commit_hash));
			output.SetValue(3, output_count, Value(row.tag_hash));
			output.SetValue(4, output_count, Value(row.tagger_name));
			output.SetValue(5, output_count, Value::TIMESTAMP(row.tagger_date));
			output.SetValue(6, output_count, Value(row.message));
			output.SetValue(7, output_count, Value(row.is_annotated));

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

void RegisterGitTagsFunction(ExtensionLoader &loader) {
	// Single-argument version (existing)
	TableFunction git_tags_func("git_tags", {LogicalType::VARCHAR}, GitTagsFunction, GitTagsBind, GitTagsInitGlobal);
	git_tags_func.init_local = GitTagsLocalInit;
	git_tags_func.named_parameters["repo_path"] = LogicalType::VARCHAR;
	loader.RegisterFunction(git_tags_func);

	// Zero-argument version (defaults to current directory)
	TableFunction git_tags_func_zero("git_tags", {}, GitTagsFunction, GitTagsBind, GitTagsInitGlobal);
	git_tags_func_zero.init_local = GitTagsLocalInit;
	git_tags_func_zero.named_parameters["repo_path"] = LogicalType::VARCHAR;
	loader.RegisterFunction(git_tags_func_zero);

	// LATERAL git_tags_each function (repository path comes from LATERAL context) - ONLY for dynamic input
	TableFunctionSet git_tags_each_set("git_tags_each");

	// Version that takes repository path as first parameter (for LATERAL context)
	TableFunction git_tags_each_single({LogicalType::VARCHAR}, nullptr, GitTagsEachBind, nullptr, GitTagsLocalInit);
	git_tags_each_single.in_out_function = GitTagsEachFunction;
	git_tags_each_single.named_parameters["repo_path"] = LogicalType::VARCHAR;
	git_tags_each_set.AddFunction(git_tags_each_single);

	// Two-argument version (repo_path, repo_path)
	TableFunction git_tags_each_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, GitTagsEachBind, nullptr,
	                                GitTagsLocalInit);
	git_tags_each_two.in_out_function = GitTagsEachFunction;
	git_tags_each_two.named_parameters["repo_path"] = LogicalType::VARCHAR;
	git_tags_each_set.AddFunction(git_tags_each_two);

	loader.RegisterFunction(git_tags_each_set);
}

} // namespace duckdb

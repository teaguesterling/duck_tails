#include "git_utils.hpp"
#include "git_filesystem.hpp"
#include "git_context_manager.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

// Parse parameters using new unified signature: func(repo_path_or_uri, [optional_ref], [other_params...])
UnifiedGitParams ParseUnifiedGitParams(TableFunctionBindInput &input, int ref_param_index) {
	UnifiedGitParams params;

	// First parameter is always repo_path_or_uri
	if (!input.inputs.empty()) {
		auto &first_arg = input.inputs[0];
		if (first_arg.type().id() == LogicalTypeId::VARCHAR) {
			params.repo_path_or_uri = first_arg.GetValue<string>();
		}
	}

	// Check if it's a git:// URI with embedded ref
	if (StringUtil::StartsWith(params.repo_path_or_uri, "git://")) {
		try {
			auto ctx = GitContextManager::Instance().ProcessGitUri(params.repo_path_or_uri, "HEAD");
			params.resolved_repo_path = ctx.repo_path;
			params.resolved_file_path = ctx.file_path;
			params.ref = ctx.final_ref;
			params.ref_kind = ctx.ref_kind;
			params.has_embedded_ref = !ctx.final_ref.empty() && ctx.final_ref != "HEAD";
		} catch (const std::exception &e) {
			throw BinderException("Failed to parse git:// URI '%s': %s", params.repo_path_or_uri, e.what());
		}
	} else {
		// Filesystem path - use repository discovery
		try {
			auto ctx = GitContextManager::Instance().ProcessGitUri(params.repo_path_or_uri, "HEAD");
			params.resolved_repo_path = ctx.repo_path;
			params.resolved_file_path = ctx.file_path;
			params.ref = "HEAD"; // Default for filesystem paths
			params.ref_kind = ctx.ref_kind;
			params.has_embedded_ref = false;
		} catch (const std::exception &e) {
			throw BinderException("Failed to resolve repository path '%s': %s", params.repo_path_or_uri, e.what());
		}
	}

	// Check for optional ref parameter (if not embedded in URI)
	if (input.inputs.size() > ref_param_index && !input.inputs[ref_param_index].IsNull()) {
		string explicit_ref = input.inputs[ref_param_index].GetValue<string>();

		if (params.has_embedded_ref && !explicit_ref.empty()) {
			throw BinderException(
			    "Conflicting ref specifications: git:// URI contains '@%s' but function parameter specifies '%s'",
			    params.ref, explicit_ref);
		}

		if (!params.has_embedded_ref && !explicit_ref.empty()) {
			params.ref = explicit_ref;
		}
	}

	return params;
}

// Parse parameters for LATERAL functions where repo_path comes from runtime DataChunk
// This function only processes static bind-time parameters (like ref, options)
UnifiedGitParams ParseLateralGitParams(TableFunctionBindInput &input, int ref_param_index) {
	UnifiedGitParams params; // Constructor sets ref = "HEAD" by default

	// For LATERAL functions, repo_path comes from runtime DataChunk, not bind time
	// So we only process the optional ref parameter if present
	if (input.inputs.size() > ref_param_index && !input.inputs[ref_param_index].IsNull()) {
		params.ref = input.inputs[ref_param_index].GetValue<string>();
	}
	// Note: If no ref parameter provided, params.ref remains "HEAD" from constructor

	return params;
}

string GetWorkdirRoot(const string &repo_path) {
	git_repository *repo = nullptr;
	int error = git_repository_open(&repo, repo_path.c_str());
	if (error != 0) {
		throw IOException("Failed to open repository '%s'", repo_path);
	}
	const char *workdir = git_repository_workdir(repo);
	if (!workdir) {
		git_repository_free(repo);
		throw IOException("Repository '%s' is bare (no working directory)", repo_path);
	}
	string result(workdir);
	git_repository_free(repo);
	return result;
}

string SafeWorkdirPath(const string &repo_path, const string &file_path) {
	string workdir = GetWorkdirRoot(repo_path);
	string candidate = workdir + file_path;

	// Resolve to canonical path and verify it's within the workdir
	char *resolved = realpath(candidate.c_str(), nullptr);
	if (!resolved) {
		throw IOException("File not found or inaccessible: '%s'", file_path);
	}
	string canonical(resolved);
	free(resolved);

	// Also canonicalize workdir for comparison
	char *resolved_workdir = realpath(workdir.c_str(), nullptr);
	if (!resolved_workdir) {
		throw IOException("Working directory not accessible: '%s'", workdir);
	}
	string canonical_workdir(resolved_workdir);
	free(resolved_workdir);

	// Ensure trailing slash for prefix comparison
	if (!canonical_workdir.empty() && canonical_workdir.back() != '/') {
		canonical_workdir += '/';
	}

	if (!StringUtil::StartsWith(canonical, canonical_workdir) && canonical != canonical_workdir.substr(0, canonical_workdir.size() - 1)) {
		throw IOException("Path '%s' escapes the repository working directory", file_path);
	}

	return canonical;
}

} // namespace duckdb

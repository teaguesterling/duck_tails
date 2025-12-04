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

} // namespace duckdb

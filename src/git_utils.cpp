#include "git_utils.hpp"
#include "git_filesystem.hpp"
#include "git_context_manager.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/local_file_system.hpp"

#ifdef _WIN32
#include <stdlib.h> // _fullpath
#else
#include <climits> // PATH_MAX
#include <cstdlib> // realpath
#endif

namespace duckdb {

string ApplyExplicitRepoPath(const string &uri, const string &repo_path, const string &function_name) {
	if (repo_path.empty() || !StringUtil::StartsWith(uri, "git://")) {
		return uri;
	}
	const size_t prefix_len = 6; // strlen("git://")

	// Normalize repo_path: strip trailing slashes so joining is consistent.
	string repo = repo_path;
	while (!repo.empty() && repo.back() == '/') {
		repo.pop_back();
	}
	if (repo.empty()) {
		return uri;
	}

	string rest = uri.substr(prefix_len);

	// Absolute URI path (leading '/' after "git://" -- i.e. three slashes in the source
	// form) cannot be combined with an explicit repo_path: the two are independent
	// specifications of the repository and silently preferring one would hide bugs.
	if (!rest.empty() && rest[0] == '/') {
		string prefix = function_name.empty() ? "" : function_name + ": ";
		throw InvalidInputException("%sconflicting repository paths: absolute URI '%s' cannot be combined with "
		                            "repo_path '%s'",
		                            prefix, uri, repo_path);
	}

	// Relative URI (or bare "git://@REF"): splice repo_path into the URI.
	if (rest.empty() || rest[0] == '@') {
		return "git://" + repo + rest;
	}
	return "git://" + repo + "/" + rest;
}

// "", "." and "./" all mean the repository root rather than a path inside it.
static bool IsRepoRootSpelling(const string &path) {
	return path.empty() || path == "." || path == "./";
}

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

	// The repository can also arrive as the repo_path named parameter. Every function
	// that registers it used to accept it and then never read it, so
	// git_log(repo_path := 'x') answered from whatever repository the working directory
	// sat in -- a well-formed commit list from the wrong repository, with nothing to
	// show the parameter had been dropped (#36). git_read and git_blame already honoured
	// it; these surfaces now agree with them, including how they read a positional
	// argument alongside it: as a path INSIDE the named repository.
	string explicit_repo_path;
	for (const auto &kv : input.named_parameters) {
		if (kv.first == "repo_path" && !kv.second.IsNull()) {
			explicit_repo_path = kv.second.GetValue<string>();
		}
	}
	while (!explicit_repo_path.empty() && explicit_repo_path.back() == '/') {
		explicit_repo_path.pop_back();
	}
	if (!explicit_repo_path.empty()) {
		if (IsRepoRootSpelling(params.repo_path_or_uri)) {
			// Nothing but the repository was named: resolve it directly, which also
			// keeps the repo_path output column reading as the caller wrote it.
			params.repo_path_or_uri = explicit_repo_path;
		} else {
			string uri = StringUtil::StartsWith(params.repo_path_or_uri, "git://") ? params.repo_path_or_uri
			                                                                       : "git://" + params.repo_path_or_uri;
			params.repo_path_or_uri = ApplyExplicitRepoPath(uri, explicit_repo_path);
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

// Cross-platform canonical path resolution
static bool TryResolvePath(const string &input, string &output) {
#ifdef _WIN32
	char resolved[_MAX_PATH];
	if (_fullpath(resolved, input.c_str(), _MAX_PATH) != nullptr) {
		output = string(resolved);
		return true;
	}
	return false;
#else
	char resolved[PATH_MAX];
	if (realpath(input.c_str(), resolved) != nullptr) {
		output = string(resolved);
		return true;
	}
	return false;
#endif
}

bool IsValidLFSOID(const string &oid) {
	if (oid.size() != 64) {
		return false;
	}
	for (char c : oid) {
		bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		if (!is_hex) {
			return false;
		}
	}
	return true;
}

string LexicallyNormalizePath(const string &path) {
	bool absolute = !path.empty() && (path[0] == '/' || path[0] == '\\');
	vector<string> parts;
	string cur;
	auto flush = [&]() {
		if (cur.empty()) {
			return;
		}
		if (cur == ".") {
			cur.clear();
			return;
		}
		if (cur == "..") {
			if (!parts.empty() && parts.back() != "..") {
				parts.pop_back();
			} else if (!absolute) {
				parts.push_back("..");
			}
			cur.clear();
			return;
		}
		parts.push_back(cur);
		cur.clear();
	};
	for (char c : path) {
		if (c == '/' || c == '\\') {
			flush();
		} else {
			cur.push_back(c);
		}
	}
	flush();

	string out = absolute ? "/" : "";
	for (size_t i = 0; i < parts.size(); i++) {
		if (i > 0) {
			out += "/";
		}
		out += parts[i];
	}
	if (out.empty()) {
		out = absolute ? "/" : ".";
	}
	return out;
}

string ConfineUnderDirectory(const string &root_dir, const string &candidate, const string &what) {
	string norm_root = LexicallyNormalizePath(root_dir);
	string norm_candidate = LexicallyNormalizePath(candidate);

	string root_with_sep = norm_root;
	if (root_with_sep.empty() || root_with_sep.back() != '/') {
		root_with_sep += "/";
	}

	if (norm_candidate != norm_root && !StringUtil::StartsWith(norm_candidate, root_with_sep)) {
		throw IOException("%s escapes the LFS object store (resolved to '%s')", what, norm_candidate);
	}
	return norm_candidate;
}

string SafeWorkdirPath(const string &repo_path, const string &file_path) {
	LocalFileSystem fs;
	string workdir = GetWorkdirRoot(repo_path);
	// Don't prepend workdir if file_path is already absolute (e.g. Windows drive letter paths)
	string candidate = fs.IsPathAbsolute(file_path) ? file_path : (workdir + file_path);

	// Resolve to canonical path and verify it's within the workdir
	string canonical;
	if (!TryResolvePath(candidate, canonical)) {
		throw IOException("File not found or inaccessible: '%s'", file_path);
	}

	// Also canonicalize workdir for comparison
	string canonical_workdir;
	if (!TryResolvePath(workdir, canonical_workdir)) {
		throw IOException("Working directory not accessible: '%s'", workdir);
	}

	// Ensure trailing separator for prefix comparison
	string sep = fs.PathSeparator(canonical_workdir);
	if (!canonical_workdir.empty() && canonical_workdir.back() != sep[0]) {
		canonical_workdir += sep;
	}

	if (!StringUtil::StartsWith(canonical, canonical_workdir) &&
	    canonical != canonical_workdir.substr(0, canonical_workdir.size() - 1)) {
		throw IOException("Path '%s' escapes the repository working directory", file_path);
	}

	return canonical;
}

} // namespace duckdb

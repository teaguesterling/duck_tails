#include "git_utils.hpp"
#include "git_filesystem.hpp"
#include "git_context_manager.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/error_data.hpp"

#ifdef _WIN32
#include <stdlib.h> // _fullpath
#else
#include <climits> // PATH_MAX
#include <cstdlib> // realpath
#endif

namespace duckdb {

string GitExceptionMessage(const std::exception &e) {
	return ErrorData(e).RawMessage();
}

string CombineArgumentAndNamedPath(const string &function_name, const string &argument_path, const string &named_path) {
	const string from_argument = NormalizeRepoPathSpec(argument_path);
	const string from_parameter = NormalizeRepoPathSpec(named_path);
	if (!from_argument.empty() && !from_parameter.empty() && from_argument != from_parameter) {
		throw BinderException("%s: a path was given both in the argument ('%s') and in the path parameter ('%s'). "
		                      "Pass only one -- there is no answer that is both.",
		                      function_name, from_argument, from_parameter);
	}
	return from_argument.empty() ? from_parameter : from_argument;
}

bool PathIsUnder(const string &path, const string &prefix) {
	if (prefix.empty()) {
		return true;
	}
	if (path == prefix) {
		return true;
	}
	// The '/' is the point: "src_backup/x" shares the characters of "src" but not
	// its components, and is not under it.
	return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 && path[prefix.size()] == '/';
}

//===--------------------------------------------------------------------===//
// Text classification
//===--------------------------------------------------------------------===//

namespace {

// Byte-at-a-time UTF-8 validation that can be fed in pieces, so a file too large
// to hold can still be classified. Deliberately as lax as the in-memory form it
// replaces (it does not reject overlong encodings or surrogates): the two must
// agree, and tightening one alone would put is_text back to meaning two things.
struct Utf8Validator {
	int pending = 0; // continuation bytes still expected
	bool ok = true;

	void Feed(const char *data, size_t length) {
		const unsigned char *bytes = reinterpret_cast<const unsigned char *>(data);
		for (size_t i = 0; i < length; i++) {
			if (!ok) {
				return;
			}
			const unsigned char byte = bytes[i];
			if (pending > 0) {
				if ((byte & 0xC0) != 0x80) {
					ok = false;
					return;
				}
				pending--;
				continue;
			}
			if (byte <= 0x7F) {
				continue;
			}
			if ((byte & 0xE0) == 0xC0) {
				pending = 1;
			} else if ((byte & 0xF0) == 0xE0) {
				pending = 2;
			} else if ((byte & 0xF8) == 0xF0) {
				pending = 3;
			} else {
				ok = false; // Invalid start byte
				return;
			}
		}
	}

	bool Valid() const {
		// A sequence left unfinished at the end is truncated, not valid.
		return ok && pending == 0;
	}
};

} // namespace

bool IsValidUTF8(const char *data, size_t length) {
	Utf8Validator validator;
	validator.Feed(data, length);
	return validator.Valid();
}

void ClassifyBlobText(const char *data, size_t length, bool git_binary_hint, bool &is_text, string &encoding) {
	is_text = !git_binary_hint && (length == 0 || IsValidUTF8(data, length));
	encoding = is_text ? "utf8" : "binary";
}

void ClassifyWorkdirFileText(const string &abs_path, bool &is_text, string &encoding) {
	// libgit2's heuristic: a NUL byte in the first 8000 makes the file binary.
	static constexpr size_t BINARY_SCAN_BYTES = 8000;
	static constexpr size_t CHUNK_BYTES = 64 * 1024;

	LocalFileSystem fs;
	unique_ptr<FileHandle> handle;
	try {
		handle = fs.OpenFile(abs_path, FileOpenFlags::FILE_FLAGS_READ);
	} catch (const std::exception &) {
		handle = nullptr;
	}
	if (!handle) {
		is_text = false;
		encoding = "unknown";
		return;
	}

	const int64_t file_size = fs.GetFileSize(*handle);
	if (file_size == 0) {
		// An empty file is empty text, which is what git_read says of an empty blob.
		is_text = true;
		encoding = "utf8";
		return;
	}

	Utf8Validator validator;
	auto buffer = make_unsafe_uniq_array<char>(CHUNK_BYTES);
	int64_t offset = 0;
	bool has_nul = false;

	while (offset < file_size) {
		const int64_t want = MinValue<int64_t>(static_cast<int64_t>(CHUNK_BYTES), file_size - offset);
		try {
			// Positional Read: loops and raises rather than returning short.
			fs.Read(*handle, buffer.get(), want, static_cast<idx_t>(offset));
		} catch (const std::exception &) {
			is_text = false;
			encoding = "unknown";
			return;
		}
		if (!has_nul && static_cast<size_t>(offset) < BINARY_SCAN_BYTES) {
			const size_t scan = MinValue<size_t>(static_cast<size_t>(want), BINARY_SCAN_BYTES - static_cast<size_t>(offset));
			has_nul = memchr(buffer.get(), 0, scan) != nullptr;
		}
		if (has_nul) {
			break;
		}
		validator.Feed(buffer.get(), static_cast<size_t>(want));
		if (!validator.ok) {
			break;
		}
		offset += want;
	}

	is_text = !has_nul && validator.Valid();
	encoding = is_text ? "utf8" : "binary";
}

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

// Value::GetValue<string>() renders a NULL as the four characters "NULL"
// (Value::ToString returns that literal), and the result then resolves as a path
// INSIDE whatever repository the working directory happens to sit in.
// git_log(NULL) consequently filtered the history down to commits touching a
// file named "NULL" and answered with zero rows -- a legitimate-looking empty
// history -- while git_branches(NULL), which never reads the file path, answered
// from the current directory as if nothing were wrong. The disagreement between
// the two made a NULL that arrived by accident very hard to trace back (#8).
//
// Shared so that every surface refuses in the same words. Not every one of them
// reaches ParseUnifiedGitParams: git_status and git_diff_tree read
// input.inputs[0] directly, and git_read and git_blame read their own repo_path
// named parameter, so each of those has to reject a NULL itself or it keeps
// answering from the current directory.
void RejectNullRepoPathArgument(const Value &value) {
	if (!value.IsNull()) {
		return;
	}
	throw BinderException("Repository path must not be NULL. Pass a repository path, a git:// URI, or no "
	                      "argument at all to use the current directory.");
}

// Named parameters only appear in bind when they were written out, so a NULL one
// was passed deliberately (or by a caller that meant to pass a path). Dropping it
// silently fell back to the current directory and produced a well-formed answer
// from the wrong repository -- the same class of invisible failure as the
// positional NULL above (#8).
void RejectNullRepoPathParameter(const Value &value) {
	if (!value.IsNull()) {
		return;
	}
	throw BinderException("repo_path must not be NULL. Pass a repository path, or omit the parameter to "
	                      "use the current directory.");
}

// Parse parameters using new unified signature: func(repo_path_or_uri, [optional_ref], [other_params...])
UnifiedGitParams ParseUnifiedGitParams(TableFunctionBindInput &input, int ref_param_index) {
	UnifiedGitParams params;

	// First parameter is always repo_path_or_uri
	if (!input.inputs.empty()) {
		auto &first_arg = input.inputs[0];
		RejectNullRepoPathArgument(first_arg);
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
		if (kv.first != "repo_path") {
			continue;
		}
		RejectNullRepoPathParameter(kv.second);
		explicit_repo_path = kv.second.GetValue<string>();
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
			throw BinderException("Failed to parse git:// URI '%s': %s", params.repo_path_or_uri,
			                      GitExceptionMessage(e));
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
			throw BinderException("Failed to resolve repository path '%s': %s", params.repo_path_or_uri,
			                      GitExceptionMessage(e));
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

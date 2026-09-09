#pragma once

#include <git2.h>
#include <string>
#include <memory>
#include <mutex>
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception.hpp"
#include "git_path.hpp"
#include "git_context_manager.hpp"

namespace duckdb {

// Plain text of a caught exception, safe to embed in another exception message.
//
// A DuckDB exception carries its message as a JSON document -- Exception derives
// from std::runtime_error constructed with Exception::ToJSON -- so what()
// returns {"exception_type":"IO","exception_message":"..."}. Splicing that into
// a new exception message JSON-encodes an already-encoded string, and when a
// chain of layers each does so the escaping compounds: git_read -> git_open ->
// git_parse -> git_lookup buried "the requested type does not match the type in
// the ODB" under four rounds of backslashes, unreadable to users and agents
// alike (#22). ErrorData parses the document back to the message it was built
// from; anything that is not such a document -- a non-DuckDB exception -- passes
// through unchanged.
string GitExceptionMessage(const std::exception &e);

// Unified parameters structure for git functions
struct UnifiedGitParams {
	string repo_path_or_uri;
	string resolved_repo_path;
	string resolved_file_path; // For git_read
	string ref;                // Optional ref parameter
	bool has_embedded_ref;     // True if ref came from git:// URI
	RefKind ref_kind;          // COMMIT, WORKDIR, or INDEX

	UnifiedGitParams()
	    : repo_path_or_uri("."), resolved_repo_path("."), resolved_file_path(""), ref("HEAD"), has_embedded_ref(false),
	      ref_kind(RefKind::COMMIT) {
	}
};

// Splice an explicit repo_path into a git:// URI.
// - Relative URI (or a bare "git://@REF"): the repository is spliced in, so
//   "git://src/x.py@HEAD" + repo "r" becomes "git://r/src/x.py@HEAD".
// - Absolute URI ("git:///abs/repo/..."): the URI already names a repository, so
//   combining it with repo_path is ambiguous and raises InvalidInputException
//   rather than silently preferring one of them.
// - Empty repo_path or a non-git:// string: returned unchanged.
// function_name, when given, prefixes the error message.
string ApplyExplicitRepoPath(const string &uri, const string &repo_path, const string &function_name = "");

// Parse parameters using new unified signature: func(repo_path_or_uri, [optional_ref], [other_params...])
UnifiedGitParams ParseUnifiedGitParams(TableFunctionBindInput &input, int ref_param_index = 1);

// Parse parameters for LATERAL functions where repo_path comes from runtime DataChunk
UnifiedGitParams ParseLateralGitParams(TableFunctionBindInput &input, int ref_param_index = 1);

// Raise on a NULL repository path rather than resolving the literal string "NULL"
// that Value::GetValue<string>() produces for one (#8). Every surface has to
// refuse in the same words, and not all of them go through
// ParseUnifiedGitParams: git_status and git_diff_tree read input.inputs[0]
// themselves, and git_read and git_blame read their own repo_path named
// parameter, so those call these directly.
void RejectNullRepoPathArgument(const Value &value);
void RejectNullRepoPathParameter(const Value &value);

// A repository path argument may name a path INSIDE the repository
// ('repo/src'), which scopes the answer to it. git_status and git_diff_tree
// parsed that path out of their first argument and then dropped it, so the
// answer was the whole repository's regardless of what was asked -- a
// well-formed status for a scope the caller did not request (#55).
//
// Combine the path from the argument with an explicit `path` named parameter.
// Both together are two answers to one question, so they raise rather than one
// silently winning. function_name prefixes the error.
string CombineArgumentAndNamedPath(const string &function_name, const string &argument_path, const string &named_path);

// Does `path` name `prefix` itself, or something inside it?
//
// Compares path COMPONENTS, not characters. A raw StringUtil::StartsWith says
// "src_backup/x" is under "src", which is how git_tree(untracked := true)
// returned files from a sibling directory the caller never asked about (#55).
bool PathIsUnder(const string &path, const string &prefix);

// Is this valid UTF-8?
//
// Shared because is_text has to mean the same thing everywhere: git_read and
// git_blame classify on UTF-8 validity (a DuckDB VARCHAR requires it), and
// git_tree used to classify on libgit2's binary heuristic alone. That heuristic
// looks for a NUL byte in the first 8000, so a Latin-1 file -- valid 8-bit text,
// no NUL, not valid UTF-8 -- came back is_text=true from git_tree and
// is_text=false from git_read for the very same blob (#55).
bool IsValidUTF8(const char *data, size_t length);

// The is_text/encoding pair for a blob already in memory. git_binary_hint is
// libgit2's own verdict (git_blob_is_binary); text also has to be valid UTF-8.
void ClassifyBlobText(const char *data, size_t length, bool git_binary_hint, bool &is_text, string &encoding);

// The same classification for a file on disk, which git_tree needs for untracked
// files. Reads in chunks and validates as it goes, so classifying a listing does
// not depend on being able to hold each file in memory.
void ClassifyWorkdirFileText(const string &abs_path, bool &is_text, string &encoding);

// RAII wrapper for git repository
class GitRepository {
public:
	explicit GitRepository(const std::string &path) : repo(nullptr) {
		if (git_repository_open(&repo, path.c_str()) != 0) {
			throw std::runtime_error("Failed to open repository at " + path);
		}
	}

	~GitRepository() {
		if (repo) {
			git_repository_free(repo);
		}
	}

	// Disable copy
	GitRepository(const GitRepository &) = delete;
	GitRepository &operator=(const GitRepository &) = delete;

	// Enable move
	GitRepository(GitRepository &&other) noexcept : repo(other.repo) {
		other.repo = nullptr;
	}

	GitRepository &operator=(GitRepository &&other) noexcept {
		if (this != &other) {
			if (repo) {
				git_repository_free(repo);
			}
			repo = other.repo;
			other.repo = nullptr;
		}
		return *this;
	}

	git_repository *get() const {
		return repo;
	}
	operator git_repository *() const {
		return repo;
	}

private:
	git_repository *repo;
};

// RAII wrapper for git objects
template <typename T>
class GitObject {
public:
	using FreeFunc = void (*)(T *);

	GitObject(T *obj, FreeFunc free_func) : obj(obj), free_func(free_func) {
	}

	~GitObject() {
		if (obj && free_func) {
			free_func(obj);
		}
	}

	// Disable copy
	GitObject(const GitObject &) = delete;
	GitObject &operator=(const GitObject &) = delete;

	// Enable move
	GitObject(GitObject &&other) noexcept : obj(other.obj), free_func(other.free_func) {
		other.obj = nullptr;
	}

	GitObject &operator=(GitObject &&other) noexcept {
		if (this != &other) {
			if (obj && free_func) {
				free_func(obj);
			}
			obj = other.obj;
			free_func = other.free_func;
			other.obj = nullptr;
		}
		return *this;
	}

	T *get() const {
		return obj;
	}
	operator T *() const {
		return obj;
	}
	T *release() {
		T *tmp = obj;
		obj = nullptr;
		return tmp;
	}

private:
	T *obj;
	FreeFunc free_func;
};

// Helper factory functions for common git objects
using GitCommitPtr = GitObject<git_commit>;
using GitTreePtr = GitObject<git_tree>;
using GitRevwalkPtr = GitObject<git_revwalk>;
using GitBranchIteratorPtr = GitObject<git_branch_iterator>;

inline GitCommitPtr MakeGitCommit(git_commit *commit) {
	return GitCommitPtr(commit, reinterpret_cast<void (*)(git_commit *)>(git_commit_free));
}

inline GitTreePtr MakeGitTree(git_tree *tree) {
	return GitTreePtr(tree, reinterpret_cast<void (*)(git_tree *)>(git_tree_free));
}

inline GitRevwalkPtr MakeGitRevwalk(git_revwalk *walker) {
	return GitRevwalkPtr(walker, reinterpret_cast<void (*)(git_revwalk *)>(git_revwalk_free));
}

inline GitBranchIteratorPtr MakeGitBranchIterator(git_branch_iterator *iter) {
	return GitBranchIteratorPtr(iter, reinterpret_cast<void (*)(git_branch_iterator *)>(git_branch_iterator_free));
}

using GitIndexPtr = GitObject<git_index>;
inline GitIndexPtr MakeGitIndex(git_index *idx) {
	return GitIndexPtr(idx, reinterpret_cast<void (*)(git_index *)>(git_index_free));
}

using GitBlobPtr = GitObject<git_blob>;
inline GitBlobPtr MakeGitBlob(git_blob *blob) {
	return GitBlobPtr(blob, reinterpret_cast<void (*)(git_blob *)>(git_blob_free));
}

// Safe workdir path construction — validates that file_path doesn't escape the workdir via ../
// Opens repo, gets workdir, constructs absolute path, validates containment.
// Returns the absolute path. Throws on bare repos, missing workdir, or path traversal.
string SafeWorkdirPath(const string &repo_path, const string &file_path);

// Get the workdir root for a repository (with trailing slash). Throws on bare repos.
string GetWorkdirRoot(const string &repo_path);

// Returns true iff `oid` is a valid git-lfs sha256 object id: exactly 64
// lowercase hex characters. A real LFS OID can never contain '/', '.' or '..',
// so validating this at the source closes the LFS-object path-traversal class.
bool IsValidLFSOID(const string &oid);

// Lexically normalize a path (resolve "." / ".." and collapse separators)
// WITHOUT touching the filesystem — no symlink resolution, no existence
// requirement. Used to confine LFS object paths, which may legitimately not
// exist locally yet (remote-LFS fallback).
string LexicallyNormalizePath(const string &path);

// Assert that `candidate` lexically resolves to a location under `root_dir`.
// Throws IOException("<what> escapes ...") otherwise. Returns the normalized
// candidate. Defense-in-depth for LFS object paths (primary defense is
// IsValidLFSOID at parse time).
string ConfineUnderDirectory(const string &root_dir, const string &candidate, const string &what);

// Note: libgit2 is initialized once at extension load time in duck_tails_extension.cpp
// Individual functions should NOT call git_libgit2_init() or git_libgit2_shutdown()

} // namespace duckdb

#include "git_filesystem.hpp"
#include "git_context_manager.hpp"
#include "git_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/local_file_system.hpp"
#include <algorithm>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <fstream>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Forward declarations for repository discovery functions
//===--------------------------------------------------------------------===//

static string FindGitRepository(const string &path, const string &display_path);
static bool IsGitRepository(const string &path);
static bool PathExists(const string &path);
static string GetDirectoryFromPath(const string &path);
static string GetParentDirectory(const string &path);
static string NormalizePath(const string &path);
static bool IsDirectory(const string &path);

//===--------------------------------------------------------------------===//
// Locating an input inside its repository
//===--------------------------------------------------------------------===//

// git_repository_discover for `path`, returning the .git directory it found.
// Used as an identity for "which repository is this directory in".
//
// Note: a failed call leaves libgit2's thread-local error slot set. Callers here
// read that slot only straight after a call they saw fail, so a stale entry is
// never reported.
static bool DiscoverGitDir(const string &path, string &out_git_dir) {
	git_buf buf = {0};
	if (git_repository_discover(&buf, path.c_str(), 0, nullptr) != 0) {
		git_buf_dispose(&buf);
		return false;
	}
	out_git_dir = buf.ptr ? string(buf.ptr) : string();
	git_buf_dispose(&buf);
	return true;
}

// Splits `path` off the last component, in place.
static string SplitLastComponent(const string &path) {
	size_t slash = path.find_last_of('/');
	return (slash == string::npos) ? path : path.substr(slash + 1);
}

// Give the location of `input` inside its repository, without ever comparing the
// caller's spelling of a path against libgit2's.
//
// The two need not agree textually. libgit2 resolves symlinks, and on Windows
// also drive-letter case, 8.3 short names, directory junctions, `subst` and
// mapped network drives, and `\\?\` extended-length prefixes. GitPath::Parse
// derived the in-repository path by stripping libgit2's repository root off the
// front of the input as text, and when that prefix did not match it fell back to
// treating the WHOLE input as a path filter inside the repository. Such a filter
// matches nothing, so the query returned zero rows and no error -- and an empty
// result is data, indistinguishable from an empty file or a repository with no
// history (#38).
//
// Walking up from the input with git_repository_discover and watching for the
// discovered .git directory to change locates the repository root in the
// caller's own spelling, so the remainder can be taken from the input itself and
// no text from libgit2 enters the comparison.
//
// `input` must be lexically absolute, and `expected_git_dir` the .git directory
// of the repository the caller already resolved it to. Returns false when
// `input` is not inside a repository at all, or lands in a DIFFERENT one than
// the caller resolved -- the two can disagree, because ".." is resolved
// lexically here but by the kernel during discovery, and across a symlink those
// give different directories. Placing the input in a repository other than the
// one about to be opened would produce exactly the silent empty result this
// function exists to prevent, so the caller must report it instead.
static bool RepoRelativePath(const string &input, const string &expected_git_dir, string &out_relative) {
	// Split into the deepest existing directory and a remainder. The remainder
	// may name something that exists only in history, which cannot be probed on
	// disk and does not need to be.
	string dir = input;
	string remainder;
	while (!dir.empty() && dir != "/" && !IsDirectory(dir)) {
		string leaf = SplitLastComponent(dir);
		if (!leaf.empty() && leaf != ".") {
			remainder = remainder.empty() ? leaf : leaf + "/" + remainder;
		}
		string parent = GetParentDirectory(dir);
		if (parent.empty() || parent == dir) {
			return false;
		}
		dir = parent;
	}

	string anchor;
	if (!DiscoverGitDir(dir, anchor) || anchor != expected_git_dir) {
		return false;
	}

	vector<string> components;
	string current = dir;
	while (true) {
		string parent = GetParentDirectory(current);
		if (parent.empty() || parent == current) {
			break;
		}
		string parent_anchor;
		if (!DiscoverGitDir(parent, parent_anchor) || parent_anchor != anchor) {
			// The parent belongs to a different repository, or none: `current` is
			// the repository root, spelled the way the caller spelled it.
			break;
		}
		string leaf = SplitLastComponent(current);
		if (!leaf.empty() && leaf != ".") {
			components.push_back(leaf);
		}
		current = parent;
	}

	out_relative.clear();
	for (size_t i = components.size(); i > 0; i--) {
		if (!out_relative.empty()) {
			out_relative += "/";
		}
		out_relative += components[i - 1];
	}
	if (!remainder.empty()) {
		if (!out_relative.empty()) {
			out_relative += "/";
		}
		out_relative += remainder;
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Revision / path-suffix splitting
//===--------------------------------------------------------------------===//

// git refuses '*', '?' and '[' in ref names (git-check-ref-format), so the
// first glob metacharacter in the text after '@' always belongs to a path or
// glob suffix appended after the ref -- DuckDB appends e.g. "/**/*.csv". The
// suffix starts at the last '/' before that metacharacter.
// Returns npos when there is no such suffix.
static size_t FindGlobSuffixStart(const string &revision_spec) {
	size_t glob_pos = revision_spec.find_first_of("*?[");
	if (glob_pos == string::npos) {
		return string::npos;
	}
	return revision_spec.rfind('/', glob_pos);
}

// A revision in a git:// URI may be followed by a path inside the repository
// ("git://repo@HEAD/dir/file.csv"), but '/' is also legal -- and extremely
// common -- inside ref names ("feature/foo", "refs/heads/main", "release/v1.0"),
// so the split cannot be decided lexically. Only the repository can settle it:
// keep the longest prefix that actually resolves and treat the remainder as a
// path. When nothing resolves, the ref is left exactly as written so the error
// names the ref the user asked for instead of its first component.
static void SplitRevisionFromPath(const string &repository_path, string &revision, string &path_suffix) {
	if (revision.find('/') == string::npos) {
		return; // no ambiguity to resolve
	}

	// Note: the probing below leaves libgit2's thread-local error slot set when a
	// candidate does not resolve. Every caller in this file reads that slot only
	// straight after a call it saw fail, so a stale entry is never reported.
	git_repository *repo = nullptr;
	if (git_repository_open_ext(&repo, repository_path.c_str(), 0, nullptr) != 0) {
		return; // no repository to ask -- keep the ref as written
	}

	string candidate = revision;
	while (true) {
		git_object *obj = nullptr;
		if (git_revparse_single(&obj, repo, candidate.c_str()) == 0) {
			git_object_free(obj);
			path_suffix = revision.substr(candidate.length()) + path_suffix;
			revision = candidate;
			break;
		}
		size_t last_slash = candidate.rfind('/');
		if (last_slash == string::npos || last_slash == 0) {
			break; // nothing resolved; leave the revision untouched
		}
		candidate = candidate.substr(0, last_slash);
	}

	git_repository_free(repo);
}

//===--------------------------------------------------------------------===//
// GitPath Implementation
//===--------------------------------------------------------------------===//

GitPath GitPath::Parse(const string &git_url) {
	GitPath result;

	// Remove git:// prefix
	string url = git_url;
	if (StringUtil::StartsWith(url, "git://")) {
		url = url.substr(6);
	}

	// Normalize Windows backslashes to forward slashes for URI parsing
	// (git URIs always use forward slashes as separators)
	for (auto &c : url) {
		if (c == '\\') {
			c = '/';
		}
	}

	// Find @ symbol to separate path from revision
	// We need to find the LAST @ that is NOT followed by '{' to support reflog syntax
	// Examples:
	//   git://file.csv@HEAD           -> split at @, revision = "HEAD"
	//   git://file.csv@HEAD@{0}       -> split at first @, revision = "HEAD@{0}"
	//   git://file.csv@main@{1.day.ago} -> split at first @, revision = "main@{1.day.ago}"
	//   git://file.csv@HEAD/**/*.csv  -> split at @, revision = "HEAD/**/*.csv" (glob handled below)
	size_t at_pos = string::npos;
	for (size_t i = 0; i < url.length(); ++i) {
		if (url[i] == '@') {
			// Check if this @ is followed by '{'
			if (i + 1 >= url.length() || url[i + 1] != '{') {
				at_pos = i; // This is a candidate, keep looking for the last one
			}
		}
	}

	string path_suffix = "";
	if (at_pos != string::npos) {
		result.revision = url.substr(at_pos + 1);
		url = url.substr(0, at_pos);

		// Split off a glob pattern appended after the revision (e.g. @HEAD/**/*.csv).
		// Slashes alone cannot mark the split: ref names contain them all the time
		// (feature/foo, refs/heads/main), so only a glob metacharacter -- which a ref
		// can never contain -- is decisive here. Anything else that follows the ref is
		// resolved against the repository by SplitRevisionFromPath below.
		size_t suffix_pos = FindGlobSuffixStart(result.revision);
		if (suffix_pos != string::npos) {
			path_suffix = result.revision.substr(suffix_pos);
			result.revision = result.revision.substr(0, suffix_pos);
		}
	} else {
		result.revision = "HEAD";
	}

	// Normalize url: strip trailing slashes for consistent handling
	// "./" and "." should both resolve to current directory with empty file_path
	while (!url.empty() && url.back() == '/') {
		url.pop_back();
	}

	// Parse repository path and file path - use discovery for ALL paths
	if (url.empty()) {
		result.repository_path = ".";
		// Ask the repository where the ref ends and the path begins.
		SplitRevisionFromPath(result.repository_path, result.revision, path_suffix);
		result.file_path = path_suffix.empty() ? "" : path_suffix.substr(1); // Remove leading /
	} else {
		// Use repository discovery for ALL paths (simple and complex)
		{
			// FindGitRepository reports its own failures, naming the URI as the
			// caller wrote it. It used to be wrapped in a catch that replaced every
			// one of them with "found no .git directory" -- true only for
			// GIT_ENOTFOUND, and actively misleading for the rest (#42).
			result.repository_path = FindGitRepository(url, git_url);

			// Ask the repository where the ref ends and the path begins.
			SplitRevisionFromPath(result.repository_path, result.revision, path_suffix);

			// Normalize the URL path for consistent file path calculation
			string normalized_url = NormalizePath(url);

			// Calculate file path relative to discovered repository using normalized paths
			if (result.repository_path == "/") {
				if (normalized_url.length() > 1) {
					result.file_path = normalized_url.substr(1) + path_suffix;
				} else {
					result.file_path = path_suffix.empty() ? "" : path_suffix.substr(1); // Remove leading /
				}
			} else if (result.repository_path == ".") {
				// For current directory: if url is "." itself, file_path is empty
				// Otherwise use the relative path
				if (url == ".") {
					result.file_path = path_suffix.empty() ? "" : path_suffix.substr(1);
				} else {
					result.file_path = url + path_suffix;
				}
			} else {
				// Check if url points exactly to the repo root
				if (normalized_url == result.repository_path) {
					// URL is the repo root itself, no file path
					result.file_path = path_suffix.empty() ? "" : path_suffix.substr(1);
				} else {
					// Remove repository path prefix to get relative file path
					string repo_prefix = result.repository_path;
					if (!repo_prefix.empty() && repo_prefix.back() != '/') {
						repo_prefix += "/";
					}

					if (normalized_url.length() >= repo_prefix.length() &&
					    normalized_url.substr(0, repo_prefix.length()) == repo_prefix) {
						result.file_path = normalized_url.substr(repo_prefix.length()) + path_suffix;
					} else {
						// The input's spelling is not a textual prefix of libgit2's
						// resolved repository root. Falling back to `url` here made the
						// whole input a path filter inside the repository, which matched
						// nothing: zero rows, no error, and no way for the caller to tell
						// that from an empty file or an empty history (#38). Ask the
						// repository where the input actually sits instead, and refuse to
						// answer at all when even that cannot place it.
						string relative;
						string expected_git_dir;
						if (!DiscoverGitDir(result.repository_path, expected_git_dir) ||
						    !RepoRelativePath(normalized_url, expected_git_dir, relative)) {
							throw IOException("Path '%s' was resolved to repository '%s', but its location inside that "
							                  "repository could not be determined. Refusing to answer with an empty "
							                  "result, which would be indistinguishable from a genuine one.",
							                  git_url, result.repository_path);
						}
						if (relative.empty()) {
							// The input names the repository root itself.
							result.file_path = path_suffix.empty() ? "" : path_suffix.substr(1);
						} else {
							result.file_path = relative + path_suffix;
						}
					}
				}
			}
		}
	}

	// Strip leading "./" from file_path — git tree entries use clean relative
	// paths (e.g. "README.md" not "./README.md").  This can happen when
	// repo_path is "." and gets spliced into the URI by ApplyExplicitRepoPath,
	// or when NormalizePath fails to produce a matching prefix on Windows.
	while (result.file_path.length() >= 2 && result.file_path[0] == '.' && result.file_path[1] == '/') {
		result.file_path = result.file_path.substr(2);
	}

	return result;
}

string GitPath::ToString() const {
	string result = "git://" + repository_path;
	if (!file_path.empty()) {
		result += "/" + file_path;
	}
	if (!revision.empty() && revision != "HEAD") {
		result += "@" + revision;
	}
	return result;
}

//===--------------------------------------------------------------------===//
// GitFileHandle Implementation
//===--------------------------------------------------------------------===//

GitFileHandle::GitFileHandle(FileSystem &file_system, const string &path, shared_ptr<string> content,
                             FileOpenFlags flags)
    : FileHandle(file_system, path, flags), content_(std::move(content)), position_(0) {
}

void GitFileHandle::Close() {
	// No-op for read-only git files (content is managed by shared_ptr)
}

int64_t GitFileHandle::Read(void *buffer, idx_t nr_bytes) {
	if (!content_ || position_ >= content_->size()) {
		return 0; // EOF
	}

	idx_t bytes_to_read = std::min(nr_bytes, static_cast<idx_t>(content_->size()) - position_);
	std::memcpy(buffer, content_->data() + position_, bytes_to_read);
	position_ += bytes_to_read;

	return static_cast<int64_t>(bytes_to_read);
}

void GitFileHandle::Write(void *buffer, idx_t nr_bytes) {
	throw InternalException("GitFileHandle: Write operations not supported");
}

int64_t GitFileHandle::GetFileSize() {
	return content_ ? static_cast<int64_t>(content_->size()) : 0;
}

void GitFileHandle::Seek(idx_t location) {
	if (content_) {
		position_ = std::min(location, static_cast<idx_t>(content_->size()));
	}
}

idx_t GitFileHandle::SeekPosition() {
	return position_;
}

void GitFileHandle::Reset() {
	position_ = 0;
}

//===--------------------------------------------------------------------===//
// GitFileSystem Implementation
//===--------------------------------------------------------------------===//

GitFileSystem::GitFileSystem() {
	// Initialize libgit2
	git_libgit2_init();
}

GitFileSystem::~GitFileSystem() {
	// Clean up repository cache
	for (auto &entry : repo_cache_) {
		git_repository_free(entry.second);
	}
	repo_cache_.clear();

	// Shutdown libgit2
	git_libgit2_shutdown();
}

bool GitFileSystem::CanHandleFile(const string &fpath) {
	return StringUtil::StartsWith(fpath, "git://");
}

// Check if a revision string is a pseudo-ref
static bool IsPseudoRef(const string &revision, RefKind &out_kind) {
	string upper = StringUtil::Upper(revision);
	if (upper == "WORKDIR" || upper == "WORKTREE") {
		out_kind = RefKind::WORKDIR;
		return true;
	}
	if (upper == "STAGED" || upper == "INDEX") {
		out_kind = RefKind::INDEX;
		return true;
	}
	return false;
}

// Safe workdir path — delegates to shared SafeWorkdirPath for traversal protection
// For Glob which needs the workdir root separately, use GetWorkdirRoot

// Get blob content from the git index
static string GetIndexBlobContent(const string &repo_path, const string &file_path) {
	git_repository *repo = nullptr;
	int error = git_repository_open_ext(&repo, repo_path.c_str(), GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr);
	if (error != 0) {
		throw IOException("Failed to open repository '%s'", repo_path);
	}

	git_index *index = nullptr;
	error = git_repository_index(&index, repo);
	if (error != 0) {
		git_repository_free(repo);
		throw IOException("Failed to get index for repository '%s'", repo_path);
	}

	error = git_index_read(index, 0);
	if (error != 0) {
		git_index_free(index);
		git_repository_free(repo);
		throw IOException("Failed to read index for repository '%s'", repo_path);
	}

	const git_index_entry *entry = git_index_get_bypath(index, file_path.c_str(), 0);
	if (!entry) {
		git_index_free(index);
		git_repository_free(repo);
		throw IOException("File '%s' not found in staging area", file_path);
	}

	git_blob *blob = nullptr;
	error = git_blob_lookup(&blob, repo, &entry->id);
	if (error != 0) {
		git_index_free(index);
		git_repository_free(repo);
		throw IOException("Failed to load blob from index for '%s'", file_path);
	}

	const void *content = git_blob_rawcontent(blob);
	git_off_t size = git_blob_rawsize(blob);
	string result(static_cast<const char *>(content), size);

	git_blob_free(blob);
	git_index_free(index);
	git_repository_free(repo);
	return result;
}

unique_ptr<FileHandle> GitFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                               optional_ptr<FileOpener> opener) {
	if (flags.OpenForWriting()) {
		throw InternalException("GitFileSystem: Write operations not supported");
	}

	try {
		auto git_path = GitPath::Parse(path);

		// Check for pseudo-refs
		RefKind ref_kind;
		if (IsPseudoRef(git_path.revision, ref_kind)) {
			if (ref_kind == RefKind::WORKDIR) {
				// Delegate to LocalFileSystem
				string abs_path = SafeWorkdirPath(git_path.repository_path, git_path.file_path);
				LocalFileSystem local_fs;
				auto local_handle = local_fs.OpenFile(abs_path, flags, opener);
				int64_t file_size = local_fs.GetFileSize(*local_handle);
				auto content = make_shared_ptr<string>();
				content->resize(static_cast<size_t>(file_size));
				if (file_size > 0) {
					local_fs.Read(*local_handle, const_cast<char *>(content->data()), file_size);
				}
				return make_uniq<GitFileHandle>(*this, path, content, flags);
			} else {
				// INDEX: read from staging area
				auto content = GetIndexBlobContent(git_path.repository_path, git_path.file_path);
				auto content_ptr = make_shared_ptr<string>(std::move(content));
				return make_uniq<GitFileHandle>(*this, path, content_ptr, flags);
			}
		}

		try {
			auto repo = OpenRepository(git_path.repository_path);
			auto commit_obj = ResolveRevision(repo, git_path.revision);
			auto content = GetBlobContent(repo, git_path.file_path, commit_obj);

			if (IsLFSPointer(content)) {
				auto lfs_info = ParseLFSPointer(content);
				return make_uniq<GitLFSFileHandle>(*this, path, std::move(lfs_info), flags, opener, repo);
			} else {
				auto content_ptr = make_shared_ptr<string>(std::move(content));
				return make_uniq<GitFileHandle>(*this, path, content_ptr, flags);
			}
		} catch (const std::exception &e) {
			throw IOException("Failed to open git file '%s': %s", path, GitExceptionMessage(e));
		}
	} catch (const IOException &) {
		// Every IOException GitPath::Parse raises already names the URI and the
		// underlying cause, and the inner catch above has already named the path.
		// Re-wrapping added a fourth "Failed to ..." layer that said nothing new
		// and, before GitExceptionMessage, a fourth round of JSON escaping around
		// the one message that mattered (#22).
		throw;
	} catch (const std::exception &e) {
		throw IOException("Failed to parse git path '%s': %s", path, GitExceptionMessage(e));
	}
}

vector<OpenFileInfo> GitFileSystem::Glob(const string &pattern, FileOpener *opener) {
	try {
		auto git_path = GitPath::Parse(pattern);

		RefKind ref_kind;
		if (IsPseudoRef(git_path.revision, ref_kind)) {
			vector<OpenFileInfo> results;
			if (ref_kind == RefKind::WORKDIR) {
				// Delegate glob to local filesystem within workdir
				try {
					string workdir_root = GetWorkdirRoot(git_path.repository_path);
					string abs_pattern = workdir_root + git_path.file_path;
					LocalFileSystem local_fs;
					auto local_results = local_fs.Glob(abs_pattern, opener);
					// Convert back to git:// URIs
					string workdir_prefix = workdir_root;
					for (auto &info : local_results) {
						string rel_path = info.path;
						if (StringUtil::StartsWith(rel_path, workdir_prefix)) {
							rel_path = rel_path.substr(workdir_prefix.length());
						}
						results.emplace_back(
						    OpenFileInfo {"git://" + git_path.repository_path + "/" + rel_path + "@WORKDIR"});
					}
				} catch (...) {
					// Return empty on error
				}
			} else {
				// INDEX: enumerate index entries matching pattern
				try {
					git_repository *repo_ptr = nullptr;
					int error = git_repository_open_ext(&repo_ptr, git_path.repository_path.c_str(),
					                                    GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr);
					if (error == 0) {
						git_index *index = nullptr;
						error = git_repository_index(&index, repo_ptr);
						if (error == 0) {
							if (git_index_read(index, 0) != 0) {
								git_index_free(index);
								git_repository_free(repo_ptr);
								return results;
							}
							size_t entry_count = git_index_entrycount(index);
							for (size_t i = 0; i < entry_count; i++) {
								const git_index_entry *entry = git_index_get_byindex(index, i);
								if (entry && entry->path) {
									string entry_path(entry->path);
									// Simple prefix match for now
									if (git_path.file_path.empty() ||
									    StringUtil::StartsWith(entry_path, git_path.file_path)) {
										results.emplace_back(OpenFileInfo {"git://" + git_path.repository_path + "/" +
										                                   entry_path + "@STAGED"});
									}
								}
							}
							git_index_free(index);
						}
						git_repository_free(repo_ptr);
					}
				} catch (...) {
					// Return empty on error
				}
			}
			return results;
		}

		try {
			auto repo = OpenRepository(git_path.repository_path);
			auto commit_obj = ResolveRevision(repo, git_path.revision);
			return ListFiles(repo, git_path, commit_obj);

		} catch (const std::exception &e) {
			throw IOException("Failed to glob git pattern '%s': %s", pattern, GitExceptionMessage(e));
		}
	} catch (const IOException &) {
		// Every IOException GitPath::Parse raises already names the URI and the
		// underlying cause, and the inner catch above has already named the
		// pattern. Re-wrapping added a fourth "Failed to ..." layer that said
		// nothing new and, before GitExceptionMessage, a fourth round of JSON
		// escaping around the one message that mattered (#22).
		throw;
	} catch (const std::exception &e) {
		throw IOException("Failed to parse git path '%s': %s", pattern, GitExceptionMessage(e));
	}
}

bool GitFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	try {
		auto git_path = GitPath::Parse(filename);

		RefKind ref_kind;
		if (IsPseudoRef(git_path.revision, ref_kind)) {
			if (ref_kind == RefKind::WORKDIR) {
				try {
					string abs_path = SafeWorkdirPath(git_path.repository_path, git_path.file_path);
					LocalFileSystem local_fs;
					return local_fs.FileExists(abs_path);
				} catch (...) {
					return false;
				}
			} else {
				// INDEX: check via git_index_get_bypath
				try {
					git_repository *repo = nullptr;
					int error = git_repository_open_ext(&repo, git_path.repository_path.c_str(),
					                                    GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr);
					if (error != 0) {
						return false;
					}
					git_index *index = nullptr;
					error = git_repository_index(&index, repo);
					if (error != 0) {
						git_repository_free(repo);
						return false;
					}
					if (git_index_read(index, 0) != 0) {
						git_index_free(index);
						git_repository_free(repo);
						return false;
					}
					const git_index_entry *entry = git_index_get_bypath(index, git_path.file_path.c_str(), 0);
					bool exists = (entry != nullptr);
					git_index_free(index);
					git_repository_free(repo);
					return exists;
				} catch (...) {
					return false;
				}
			}
		}

		try {
			auto repo = OpenRepository(git_path.repository_path);
			auto commit_obj = ResolveRevision(repo, git_path.revision);

			// Try to get the blob content - if it succeeds, file exists
			GetBlobContent(repo, git_path.file_path, commit_obj);
			return true;

		} catch (...) {
			return false;
		}
	} catch (const IOException &e) {
		return false;
	}
}

int64_t GitFileSystem::GetFileSize(FileHandle &handle) {
	if (auto *lfs_handle = dynamic_cast<GitLFSFileHandle *>(&handle)) {
		return lfs_handle->GetFileSize();
	} else {
		auto &git_handle = handle.Cast<GitFileHandle>();
		return git_handle.GetFileSize();
	}
}

timestamp_t GitFileSystem::GetLastModifiedTime(FileHandle &handle) {
	// For git files, return current time since git objects are immutable
	return Timestamp::GetCurrentTimestamp();
}

bool GitFileSystem::CanSeek() {
	// Git files are memory-backed, so seeking is supported
	return true;
}

bool GitFileSystem::OnDiskFile(FileHandle &handle) {
	// Git files are loaded into memory, not on-disk files
	return false;
}

bool GitFileSystem::IsPipe(const string &filename, optional_ptr<FileOpener> opener) {
	// Git files are never pipes
	return false;
}

int64_t GitFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	if (auto *lfs_handle = dynamic_cast<GitLFSFileHandle *>(&handle)) {
		return lfs_handle->Read(buffer, static_cast<idx_t>(nr_bytes));
	} else {
		auto &git_handle = handle.Cast<GitFileHandle>();
		return git_handle.Read(buffer, static_cast<idx_t>(nr_bytes));
	}
}

// Positional read. Blob content is already fully in memory (and the LFS handle
// delegates to a seekable handle), so this is a seek followed by the sequential
// read. DuckDB requires the positional form to deliver every requested byte or
// throw -- a short read here would look like a truncated file to the caller.
void GitFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	Seek(handle, location);
	int64_t bytes_read = Read(handle, buffer, nr_bytes);
	if (bytes_read != nr_bytes) {
		throw IOException("Could not read all bytes from git file '%s': requested %s bytes at offset %s, got %s",
		                  handle.GetPath(), std::to_string(nr_bytes), std::to_string(location),
		                  std::to_string(bytes_read));
	}
}

void GitFileSystem::Seek(FileHandle &handle, idx_t location) {
	if (auto *lfs_handle = dynamic_cast<GitLFSFileHandle *>(&handle)) {
		lfs_handle->Seek(location);
	} else {
		auto &git_handle = handle.Cast<GitFileHandle>();
		git_handle.Seek(location);
	}
}

idx_t GitFileSystem::SeekPosition(FileHandle &handle) {
	if (auto *lfs_handle = dynamic_cast<GitLFSFileHandle *>(&handle)) {
		return lfs_handle->SeekPosition();
	} else {
		auto &git_handle = handle.Cast<GitFileHandle>();
		return git_handle.SeekPosition();
	}
}

void GitFileSystem::Reset(FileHandle &handle) {
	if (auto *lfs_handle = dynamic_cast<GitLFSFileHandle *>(&handle)) {
		lfs_handle->Reset();
	} else {
		auto &git_handle = handle.Cast<GitFileHandle>();
		git_handle.Reset();
	}
}

//===--------------------------------------------------------------------===//
// Git Operations
//===--------------------------------------------------------------------===//

git_repository *GitFileSystem::OpenRepository(const string &repo_path) {
	// Check cache first
	auto it = repo_cache_.find(repo_path);
	if (it != repo_cache_.end()) {
		return it->second;
	}

	// Open repository
	git_repository *repo = nullptr;
	int error = git_repository_open(&repo, repo_path.c_str());
	if (error != 0) {
		const git_error *e = git_error_last();
		throw IOException("Failed to open git repository '%s': %s", repo_path, e ? e->message : "Unknown error");
	}

	// Cache and return
	repo_cache_[repo_path] = repo;
	return repo;
}

git_object *GitFileSystem::ResolveRevision(git_repository *repo, const string &revision) {
	git_object *obj = nullptr;
	int error = git_revparse_single(&obj, repo, revision.c_str());
	if (error != 0) {
		const git_error *e = git_error_last();
		throw IOException("Failed to resolve revision '%s': %s", revision, e ? e->message : "Unknown error");
	}

	// An annotated tag resolves to the tag object rather than to the commit it
	// points at, and every caller here wants the commit. Peeling is a no-op for
	// objects that are already commits.
	git_object *commit_obj = nullptr;
	if (git_object_peel(&commit_obj, obj, GIT_OBJECT_COMMIT) == 0) {
		git_object_free(obj);
		return commit_obj;
	}
	return obj;
}

string GitFileSystem::GetBlobContent(git_repository *repo, const string &file_path, git_object *commit_obj) {
	// Get the tree from the commit
	git_commit *commit = nullptr;
	int error = git_commit_lookup(&commit, repo, git_object_id(commit_obj));
	if (error != 0) {
		const git_error *e = git_error_last();
		throw IOException("Failed to lookup commit: %s", e ? e->message : "Unknown error");
	}

	git_tree *tree = nullptr;
	error = git_commit_tree(&tree, commit);
	git_commit_free(commit);
	if (error != 0) {
		const git_error *e = git_error_last();
		throw IOException("Failed to get commit tree: %s", e ? e->message : "Unknown error");
	}

	// Look up the file in the tree
	git_tree_entry *entry = nullptr;
	error = git_tree_entry_bypath(&entry, tree, file_path.c_str());
	git_tree_free(tree);
	if (error != 0) {
		const git_error *e = git_error_last();
		throw IOException("File '%s' not found in tree: %s", file_path, e ? e->message : "Unknown error");
	}

	// Get the blob
	git_blob *blob = nullptr;
	error = git_blob_lookup(&blob, repo, git_tree_entry_id(entry));
	git_tree_entry_free(entry);
	if (error != 0) {
		const git_error *e = git_error_last();
		throw IOException("Failed to lookup blob: %s", e ? e->message : "Unknown error");
	}

	// Extract content
	const void *content = git_blob_rawcontent(blob);
	git_off_t size = git_blob_rawsize(blob);
	string result(static_cast<const char *>(content), size);

	git_blob_free(blob);
	return result;
}

//===--------------------------------------------------------------------===//
// Tree globbing
//===--------------------------------------------------------------------===//

static bool PatternHasGlob(const string &pattern) {
	return pattern.find_first_of("*?[") != string::npos;
}

// Matches `c` against the character class starting at pattern[p] (a '[').
// On a well-formed class `next` is advanced past the closing ']'; on an
// unterminated '[' `next` is left at `p` so the caller can treat it literally.
static bool MatchCharacterClass(char c, const string &pattern, size_t p, size_t &next) {
	next = p;
	size_t i = p + 1;
	bool negate = false;
	if (i < pattern.size() && (pattern[i] == '!' || pattern[i] == '^')) {
		negate = true;
		i++;
	}
	bool matched = false;
	bool first = true; // a ']' in first position is a literal, as in shell globs
	while (i < pattern.size() && (pattern[i] != ']' || first)) {
		if (i + 2 < pattern.size() && pattern[i + 1] == '-' && pattern[i + 2] != ']') {
			if (c >= pattern[i] && c <= pattern[i + 2]) {
				matched = true;
			}
			i += 3;
		} else {
			if (pattern[i] == c) {
				matched = true;
			}
			i++;
		}
		first = false;
	}
	if (i >= pattern.size()) {
		return false; // unterminated '['
	}
	next = i + 1;
	return matched != negate;
}

// Matches one path component against one glob component. Wildcards never cross
// a '/' here -- spanning directories is what the '**' component does -- which
// mirrors how DuckDB globs a local path.
static bool GlobComponentMatches(const string &text, size_t t, const string &pattern, size_t p) {
	while (p < pattern.size()) {
		if (pattern[p] == '*') {
			while (p < pattern.size() && pattern[p] == '*') {
				p++;
			}
			if (p == pattern.size()) {
				return true;
			}
			for (size_t skip = t; skip <= text.size(); skip++) {
				if (GlobComponentMatches(text, skip, pattern, p)) {
					return true;
				}
			}
			return false;
		}
		if (t >= text.size()) {
			return false;
		}
		if (pattern[p] == '?') {
			p++;
			t++;
			continue;
		}
		if (pattern[p] == '[') {
			size_t next = p;
			bool matched = MatchCharacterClass(text[t], pattern, p, next);
			if (next != p) {
				if (!matched) {
					return false;
				}
				p = next;
				t++;
				continue;
			}
			// Unterminated '[' -- fall through and compare it literally.
		}
		if (pattern[p] != text[t]) {
			return false;
		}
		p++;
		t++;
	}
	return t == text.size();
}

// Matches a tree path (split on '/') against a glob pattern (also split on '/').
static bool GlobPathMatches(const vector<string> &parts, size_t pi, const vector<string> &pattern_parts, size_t qi) {
	while (qi < pattern_parts.size()) {
		if (pattern_parts[qi] == "**") {
			// '**' matches zero or more whole path components.
			for (size_t skip = pi; skip <= parts.size(); skip++) {
				if (GlobPathMatches(parts, skip, pattern_parts, qi + 1)) {
					return true;
				}
			}
			return false;
		}
		if (pi >= parts.size()) {
			return false;
		}
		if (!GlobComponentMatches(parts[pi], 0, pattern_parts[qi], 0)) {
			return false;
		}
		pi++;
		qi++;
	}
	return pi == parts.size();
}

struct TreeGlobState {
	vector<string> pattern_parts;
	vector<string> matches;
};

static int CollectMatchingBlobs(const char *root, const git_tree_entry *entry, void *payload) {
	auto &state = *reinterpret_cast<TreeGlobState *>(payload);
	if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB) {
		return 0; // keep descending into subtrees
	}
	const char *name = git_tree_entry_name(entry);
	if (!name) {
		return 0;
	}
	string full_path = string(root ? root : "") + name;
	auto parts = StringUtil::Split(full_path, '/');
	if (GlobPathMatches(parts, 0, state.pattern_parts, 0)) {
		state.matches.push_back(full_path);
	}
	return 0;
}

// Returns the tree of the commit, or nullptr if it cannot be loaded.
static git_tree *GetCommitTree(git_repository *repo, git_object *commit_obj) {
	git_commit *commit = nullptr;
	if (git_commit_lookup(&commit, repo, git_object_id(commit_obj)) != 0) {
		return nullptr;
	}
	git_tree *tree = nullptr;
	int error = git_commit_tree(&tree, commit);
	git_commit_free(commit);
	if (error != 0) {
		return nullptr;
	}
	return tree;
}

vector<OpenFileInfo> GitFileSystem::ListFiles(git_repository *repo, const GitPath &git_path, git_object *commit_obj) {
	vector<OpenFileInfo> results;
	const string &pattern = git_path.file_path;

	// DuckDB re-parses every URI returned here when it opens the file, so each
	// one has to carry the repository and the revision. Handing back a bare
	// tree path would quietly reopen the file at HEAD of whatever repository
	// the working directory happens to be in.
	auto to_uri = [&](const string &entry_path) {
		string uri = "git://" + git_path.repository_path + "/" + entry_path;
		if (!git_path.revision.empty()) {
			uri += "@" + git_path.revision;
		}
		return OpenFileInfo {uri};
	};

	if (pattern.empty()) {
		// A repository URI with no path names no file.
		return results;
	}

	git_tree *tree = GetCommitTree(repo, commit_obj);
	if (!tree) {
		return results;
	}

	if (!PatternHasGlob(pattern)) {
		// Exact path: look it up without materializing the blob.
		git_tree_entry *entry = nullptr;
		if (git_tree_entry_bypath(&entry, tree, pattern.c_str()) == 0) {
			if (git_tree_entry_type(entry) == GIT_OBJECT_BLOB) {
				results.push_back(to_uri(pattern));
			}
			git_tree_entry_free(entry);
		} else {
		}
		git_tree_free(tree);
		return results;
	}

	// Glob: walk the whole commit tree and keep the blobs that match.
	TreeGlobState state;
	state.pattern_parts = StringUtil::Split(pattern, '/');
	if (git_tree_walk(tree, GIT_TREEWALK_PRE, CollectMatchingBlobs, &state) != 0) {
	}
	git_tree_free(tree);

	// Deterministic order regardless of how the tree is stored.
	std::sort(state.matches.begin(), state.matches.end());
	for (auto &match : state.matches) {
		results.push_back(to_uri(match));
	}
	return results;
}

//===--------------------------------------------------------------------===//
// Repository Discovery
//===--------------------------------------------------------------------===//

static bool IsGitRepository(const string &path) {
	// Try to open the repository with libgit2 - most reliable method
	// Use GIT_REPOSITORY_OPEN_NO_SEARCH to prevent walking up the tree
	// This ensures we only find a repo if path IS the repo root
	git_repository *repo = nullptr;
	int error = git_repository_open_ext(&repo, path.c_str(), GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr);

	if (error == 0) {
		git_repository_free(repo);
		return true;
	}

	return false;
}

static bool PathExists(const string &path) {
	LocalFileSystem fs;
	return fs.FileExists(path) || fs.DirectoryExists(path);
}

static bool IsDirectory(const string &path) {
	LocalFileSystem fs;
	return fs.DirectoryExists(path);
}

// Normalizes a path by resolving relative components (./ and ../) and converting to absolute path
static string NormalizePath(const string &path) {
	string current_path = path;

	// Normalize backslashes to forward slashes for uniform handling (Windows)
	for (auto &c : current_path) {
		if (c == '\\') {
			c = '/';
		}
	}

	// Detect whether the path is already absolute:
	//   Unix:    /home/user/...
	//   Windows: D:/Users/... (drive letter followed by colon)
	bool is_absolute = (!current_path.empty() && current_path[0] == '/') ||
	                   (current_path.length() >= 2 && std::isalpha(current_path[0]) && current_path[1] == ':');

	// Resolve relative paths to absolute paths using DuckDB's cross-platform API
	if (!is_absolute) {
		string cwd = FileSystem::GetWorkingDirectory();
		// Normalize CWD separators as well
		for (auto &c : cwd) {
			if (c == '\\') {
				c = '/';
			}
		}
		if (!cwd.empty()) {
			current_path = cwd + "/" + current_path;
		}
	}

	// Re-check for Windows drive-letter root after possible CWD prepend
	bool has_drive_letter = (current_path.length() >= 2 && std::isalpha(current_path[0]) && current_path[1] == ':');
	string drive_prefix; // e.g. "D:"
	if (has_drive_letter) {
		drive_prefix = current_path.substr(0, 2);
		// Remove the drive prefix so the rest can be split uniformly by '/'
		current_path = current_path.substr(2);
	}

	// Normalize by resolving .. and . components
	vector<string> components;
	stringstream ss(current_path);
	string component;

	if (!current_path.empty() && current_path[0] == '/') {
		components.push_back("");
	}

	while (getline(ss, component, '/')) {
		if (component.empty() || component == ".") {
			continue;
		} else if (component == "..") {
			if (!components.empty() && components.back() != "..") {
				components.pop_back();
			}
		} else {
			components.push_back(component);
		}
	}

	// Reconstruct path
	if (components.empty() || (components.size() == 1 && components[0].empty())) {
		return drive_prefix.empty() ? "/" : (drive_prefix + "/");
	} else {
		string result = drive_prefix;
		for (const auto &comp : components) {
			if (!comp.empty()) {
				result += "/" + comp;
			}
		}
		return result.empty() ? "/" : result;
	}
}

static string GetDirectoryFromPath(const string &path) {
	// Handle empty path
	if (path.empty()) {
		return "";
	}

	// Find the last slash
	size_t last_slash = path.find_last_of('/');

	// No slash found - path is just a filename
	if (last_slash == string::npos) {
		return "";
	}

	// Root directory case
	if (last_slash == 0) {
		return "/";
	}

	// Return directory part
	return path.substr(0, last_slash);
}

static string GetParentDirectory(const string &path) {
	// Handle empty path or root
	if (path.empty() || path == "/") {
		return "";
	}

	// Remove trailing slash if present
	string clean_path = path;
	if (clean_path.back() == '/' && clean_path.length() > 1) {
		clean_path = clean_path.substr(0, clean_path.length() - 1);
	}

	// Find the last slash
	size_t last_slash = clean_path.find_last_of('/');

	// No slash found - parent is current directory
	if (last_slash == string::npos) {
		return ".";
	}

	// Root directory case
	if (last_slash == 0) {
		return "/";
	}

	// Return parent directory
	return clean_path.substr(0, last_slash);
}

// Names the libgit2 error codes worth naming, so a report carries the symbol a
// reader can search for rather than a bare integer.
static const char *GitErrorCodeName(int code) {
	switch (code) {
	case GIT_ERROR:
		return "GIT_ERROR";
	case GIT_ENOTFOUND:
		return "GIT_ENOTFOUND";
	case GIT_EEXISTS:
		return "GIT_EEXISTS";
	case GIT_EAMBIGUOUS:
		return "GIT_EAMBIGUOUS";
	case GIT_EBUFS:
		return "GIT_EBUFS";
	case GIT_EINVALIDSPEC:
		return "GIT_EINVALIDSPEC";
	case GIT_ECONFLICT:
		return "GIT_ECONFLICT";
	case GIT_ELOCKED:
		return "GIT_ELOCKED";
	case GIT_EAUTH:
		return "GIT_EAUTH";
	case GIT_ECERTIFICATE:
		return "GIT_ECERTIFICATE";
	case GIT_EDIRECTORY:
		return "GIT_EDIRECTORY";
	case GIT_EOWNER:
		return "GIT_EOWNER";
	default:
		return nullptr;
	}
}

// Renders whatever libgit2 last recorded, plus the symbolic code, for a call the
// caller has already seen fail.
static string DescribeGitError(int code) {
	const git_error *e = git_error_last();
	string detail = (e && e->message) ? string(e->message) : string("no further detail from libgit2");
	const char *name = GitErrorCodeName(code);
	if (name) {
		detail += StringUtil::Format(" (%s)", name);
	} else {
		detail += StringUtil::Format(" (libgit2 error %d)", code);
	}
	return detail;
}

// Finds the git repository root directory by walking up the directory tree from the given path
// Uses libgit2's git_repository_discover for cross-platform path handling
//
// `display_path` is the spelling to name in an error -- the URI the caller
// actually wrote, which `path` no longer is by the time it reaches here.
//
// Every failure used to leave here, and then leave GitPath::Parse, as "Searched
// up directory tree ... but found no .git directory" regardless of what libgit2
// had said. That message names a cause instead of reporting one: it is accurate
// only for GIT_ENOTFOUND, and for anything else -- GIT_EOWNER above all, where
// the repository is sitting right there and only ownership validation refused
// it -- it sends the reader looking for a directory that is not missing. Two
// separate investigations lost hours to it (#42), with the real error one
// git_error_last() call away the whole time.
static string FindGitRepository(const string &path, const string &display_path) {
	// First, try to find an existing starting point for discovery
	string start_path = path;

	// If path is relative, we need to make it work for libgit2
	// libgit2's discover function handles relative paths well

	// Walk up from non-existent paths to first existing directory
	while (!start_path.empty() && start_path != "/" && start_path != "." && !PathExists(start_path)) {
		start_path = GetParentDirectory(start_path);
	}

	// If we couldn't find any existing path, start from current directory
	if (start_path.empty() || !PathExists(start_path)) {
		start_path = ".";
	}

	// If path points to a file (not directory), start from its directory
	if (!IsDirectory(start_path)) {
		string dir = GetDirectoryFromPath(start_path);
		if (!dir.empty()) {
			start_path = dir;
		}
	}

	// Use libgit2's discovery mechanism - handles cross-platform paths correctly
	git_buf discovered_path = {0};
	int error = git_repository_discover(&discovered_path, start_path.c_str(), 0, nullptr);

	if (error == 0) {
		// Open the repository to get the proper workdir (handles submodules correctly)
		git_repository *repo = nullptr;
		string git_dir = discovered_path.ptr ? string(discovered_path.ptr) : string();
		error = git_repository_open(&repo, discovered_path.ptr);
		git_buf_dispose(&discovered_path);

		if (error != 0) {
			// Discovery succeeded, so the .git directory is there and was found.
			// Whatever went wrong happened on the way in, and saying "no .git
			// directory" here would be flatly false.
			throw IOException("Found a git directory at '%s' for path '%s' but could not open it: %s", git_dir,
			                  display_path, DescribeGitError(error));
		}

		// For worktrees and submodules, get the workdir path
		// For bare repos, use the repo path
		string result;
		const char *workdir = git_repository_workdir(repo);
		if (workdir) {
			result = workdir;
		} else {
			// Bare repository - use the repository path itself
			result = git_repository_path(repo);
		}

		git_repository_free(repo);

		// Remove trailing slashes
		while (!result.empty() && (result.back() == '/' || result.back() == '\\')) {
			result.pop_back();
		}

		return result.empty() ? "." : result;
	}

	git_buf_dispose(&discovered_path);

	if (error == GIT_ENOTFOUND) {
		// The one case where naming the cause is reporting it: libgit2 walked the
		// tree and there was genuinely nothing there.
		throw IOException("No git repository found for path '%s'. "
		                  "Searched up directory tree from '%s' but found no .git directory.",
		                  display_path, start_path);
	}

	string message = StringUtil::Format("Could not discover a git repository for path '%s' (searched from '%s'): %s",
	                                    display_path, start_path, DescribeGitError(error));
	if (error == GIT_EOWNER) {
		// GIT_EOWNER is an environment problem with one well-known remedy, and
		// the repository is present -- the reader needs to be told that, not sent
		// looking for a .git directory that is right in front of them.
		message += ". The repository exists but is owned by another user; add it to "
		           "safe.directory (git config --global --add safe.directory <path>) to allow access";
	}
	throw IOException(message);
}

//===--------------------------------------------------------------------===//
// GitLFSFileHandle Implementation
//===--------------------------------------------------------------------===//

GitLFSFileHandle::GitLFSFileHandle(FileSystem &file_system, const string &path, LFSInfo lfs_info, FileOpenFlags flags,
                                   optional_ptr<FileOpener> opener, git_repository *repo)
    : FileHandle(file_system, path, flags), lfs_info_(std::move(lfs_info)), opener_(opener), repo_(repo) {
}

void GitLFSFileHandle::Close() {
	if (remote_handle_) {
		remote_handle_->Close();
	}
}

int64_t GitLFSFileHandle::Read(void *buffer, idx_t nr_bytes) {
	EnsureRemoteHandleOpened();

	if (local_fs_) {
		return local_fs_->Read(*remote_handle_, buffer, nr_bytes);
	} else {
		return remote_handle_->Read(buffer, nr_bytes);
	}
}

void GitLFSFileHandle::Write(void *buffer, idx_t nr_bytes) {
	throw InternalException("GitLFSFileHandle: Write operations not supported");
}

int64_t GitLFSFileHandle::GetFileSize() {
	return lfs_info_.size;
}

void GitLFSFileHandle::Seek(idx_t location) {
	EnsureRemoteHandleOpened();
	remote_handle_->Seek(location);
}

idx_t GitLFSFileHandle::SeekPosition() {
	EnsureRemoteHandleOpened();
	return remote_handle_->SeekPosition();
}

void GitLFSFileHandle::Reset() {
	EnsureRemoteHandleOpened();
	remote_handle_->Reset();
}

idx_t GitLFSFileHandle::GetProgress() {
	if (!remote_handle_) {
		return 0;
	}
	return remote_handle_->GetProgress();
}

void GitLFSFileHandle::EnsureRemoteHandleOpened() {
	if (remote_handle_opened_) {
		return;
	}

	// First try local LFS cache
	string local_path = BuildLFSObjectPath(lfs_info_.oid);

	// Use simple C++ file check instead of filesystem abstraction
	std::ifstream test_file(local_path);
	if (test_file.good()) {
		test_file.close();
		// File exists locally, use LocalFileSystem to open it
		local_fs_ = make_uniq<LocalFileSystem>();
		remote_handle_ = local_fs_->OpenFile(local_path, flags, opener_);
	} else {
		// Try to get remote download URL and open via DuckDB filesystem
		download_url_ = ResolveLFSDownloadURL();
		remote_handle_ = file_system.OpenFile(download_url_, flags, opener_);
	}

	remote_handle_opened_ = true;
}

string GitLFSFileHandle::BuildLFSObjectPath(const string &oid) {
	// Strict OID validation (defense-in-depth; ParseLFSPointer already rejects
	// bad OIDs, but every OID->path site must validate so none can drift).
	if (!IsValidLFSOID(oid)) {
		throw IOException("Invalid LFS pointer OID '%s': expected exactly 64 lowercase hex characters "
		                  "(a valid git-lfs sha256 object id)",
		                  oid);
	}

	if (!repo_) {
		throw IOException("No repository context available for LFS object path");
	}

	const char *repo_path = git_repository_path(repo_);
	if (!repo_path) {
		throw IOException("Could not get repository path");
	}

	// Build path: .git/lfs/objects/ab/cd/abcd1234...
	string objects_root = string(repo_path) + "lfs/objects";
	string lfs_path = objects_root + "/" + oid.substr(0, 2) + "/" + oid.substr(2, 2) + "/" + oid;

	// Confine under the LFS object store (defense-in-depth).
	return ConfineUnderDirectory(objects_root, lfs_path, "LFS object path");
}

string GitLFSFileHandle::ResolveLFSDownloadURL() {
	// TODO: Implement the LFS Batch API so objects can be fetched on demand.
	//
	// Until then this is an error rather than a silent skip: the file's content
	// genuinely is not available here, and returning the pointer text (or an
	// empty file) would feed a query data that looks real and is not. The
	// message says which object is missing and how to get it.
	string local_path = BuildLFSObjectPath(lfs_info_.oid);
	throw IOException("Git LFS object for '%s' is not in the local LFS cache, and fetching it from the LFS "
	                  "server is not implemented yet. Run 'git lfs pull' in the repository to download it. "
	                  "(object sha256:%s, %s bytes, expected at %s)",
	                  path, lfs_info_.oid, std::to_string(lfs_info_.size), local_path);
}

LFSConfig GitLFSFileHandle::ReadLFSConfig() {
	// TODO: Implement proper LFS config reading from git repository
	// For now, return empty config
	return LFSConfig();
}

//===--------------------------------------------------------------------===//
// LFS Support Implementation
//===--------------------------------------------------------------------===//

bool GitFileSystem::IsLFSPointer(const string &content) {
	// LFS pointer files are small text files with specific format
	if (content.size() > 1024) {
		return false; // LFS pointers are typically < 200 bytes
	}

	// Check for LFS signature
	return content.find("version https://git-lfs.github.com/spec/v1") == 0 &&
	       content.find("oid sha256:") != string::npos && content.find("size ") != string::npos;
}

LFSInfo GitFileSystem::ParseLFSPointer(const string &pointer_content) {
	LFSInfo lfs_info;

	// Parse line by line
	std::istringstream stream(pointer_content);
	string line;

	while (std::getline(stream, line)) {
		if (StringUtil::StartsWith(line, "version ")) {
			lfs_info.version = line.substr(8); // Skip "version "
		} else if (StringUtil::StartsWith(line, "oid sha256:")) {
			lfs_info.oid = line.substr(11); // Skip "oid sha256:"
		} else if (StringUtil::StartsWith(line, "size ")) {
			try {
				lfs_info.size = std::stoll(line.substr(5)); // Skip "size "
			} catch (const std::exception &e) {
				throw IOException("Invalid LFS pointer: invalid size value");
			}
		}
	}

	// Validate required fields
	if (lfs_info.oid.empty() || lfs_info.size <= 0) {
		throw IOException("Invalid LFS pointer: missing required fields");
	}

	// Reject any OID that is not a real git-lfs sha256 object id. A valid OID is
	// exactly 64 lowercase hex characters and can never contain '/', '.' or
	// '..', so this closes the LFS-object path-traversal class at the source
	// (an attacker-controlled pointer cannot escape .git/lfs/objects/).
	if (!IsValidLFSOID(lfs_info.oid)) {
		throw IOException("Invalid LFS pointer OID '%s': expected exactly 64 lowercase hex characters "
		                  "(a valid git-lfs sha256 object id)",
		                  lfs_info.oid);
	}

	return lfs_info;
}

string GitFileSystem::BuildLFSObjectPath(git_repository *repo, const string &oid) {
	// Strict OID validation (defense-in-depth; see ParseLFSPointer).
	if (!IsValidLFSOID(oid)) {
		throw IOException("Invalid LFS pointer OID '%s': expected exactly 64 lowercase hex characters "
		                  "(a valid git-lfs sha256 object id)",
		                  oid);
	}

	const char *repo_path = git_repository_path(repo);
	if (!repo_path) {
		throw IOException("Could not get repository path");
	}

	// Build path: .git/lfs/objects/ab/cd/abcd1234...
	string objects_root = string(repo_path) + "lfs/objects";
	string lfs_path = objects_root + "/" + oid.substr(0, 2) + "/" + oid.substr(2, 2) + "/" + oid;

	// Confine under the LFS object store (defense-in-depth).
	return ConfineUnderDirectory(objects_root, lfs_path, "LFS object path");
}

LFSConfig GitFileSystem::ReadLFSConfig(git_repository *repo) {
	LFSConfig config;

	// Try to read .lfsconfig file in repository root
	const char *workdir = git_repository_workdir(repo);
	if (workdir) {
		string config_path = string(workdir) + ".lfsconfig";

		// For now, implement basic config reading
		// TODO: Implement proper git config parsing

		// Default: construct LFS URL from git remote
		git_remote *remote = nullptr;
		int error = git_remote_lookup(&remote, repo, "origin");
		if (error == 0) {
			const char *url = git_remote_url(remote);
			if (url) {
				string git_url(url);
				// Convert git URL to LFS URL: append .git/info/lfs
				if (StringUtil::EndsWith(git_url, ".git")) {
					config.lfs_url = git_url + "/info/lfs";
				} else {
					config.lfs_url = git_url + ".git/info/lfs";
				}
			}
			git_remote_free(remote);
		}
	}

	return config;
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void RegisterGitFileSystem(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &fs = FileSystem::GetFileSystem(db);
	fs.RegisterSubSystem(make_uniq<GitFileSystem>());
}

} // namespace duckdb

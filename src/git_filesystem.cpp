#include "git_filesystem.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception.hpp"
#include <regex>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <climits>
#include <vector>
#include <sstream>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Forward declarations for repository discovery functions
//===--------------------------------------------------------------------===//

static string FindGitRepository(const string &path);
static bool IsGitRepository(const string &path);
static bool PathExists(const string &path);
static string GetDirectoryFromPath(const string &path);
static string GetParentDirectory(const string &path);
static string NormalizePath(const string &path);

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
    
    // Find @ symbol to separate path from revision
    size_t at_pos = url.find_last_of('@');
    if (at_pos != string::npos) {
        result.revision = url.substr(at_pos + 1);
        url = url.substr(0, at_pos);
    } else {
        result.revision = "HEAD";
    }
    
    // Parse repository path and file path - use discovery for ALL paths
    if (url.empty()) {
        result.repository_path = ".";
        result.file_path = "";
    } else {
        // Use repository discovery for ALL paths (simple and complex)
        try {
            result.repository_path = FindGitRepository(url);
            
            // Normalize the URL path for consistent file path calculation
            string normalized_url = NormalizePath(url);
            
            // Calculate file path relative to discovered repository using normalized paths
            if (result.repository_path == "/") {
                if (normalized_url.length() > 1) {
                    result.file_path = normalized_url.substr(1);
                } else {
                    result.file_path = "";
                }
            } else if (result.repository_path == ".") {
                // For current directory, use original relative path
                result.file_path = url;
            } else {
                // Remove repository path prefix to get relative file path
                string repo_prefix = result.repository_path;
                if (!repo_prefix.empty() && repo_prefix.back() != '/') {
                    repo_prefix += "/";
                }
                
                if (normalized_url.length() >= repo_prefix.length() && 
                    normalized_url.substr(0, repo_prefix.length()) == repo_prefix) {
                    result.file_path = normalized_url.substr(repo_prefix.length());
                } else {
                    // Use original relative path if normalized doesn't match
                    result.file_path = url;
                }
            }
        } catch (const IOException &e) {
            // No fallback - fail fast with clear error message
            string search_path = url;
            if (url.find('/') != string::npos) {
                search_path = GetDirectoryFromPath(url);
            }
            throw IOException("No git repository found for path '%s'. "
                             "Searched up directory tree from '%s' but found no .git directory.", 
                             git_url.c_str(), search_path.c_str());
        }
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

GitFileHandle::GitFileHandle(FileSystem &file_system, const string &path, shared_ptr<string> content, FileOpenFlags flags)
    : FileHandle(file_system, path, flags), content_(std::move(content)), position_(0) {
}

void GitFileHandle::Close() {
    // No-op for read-only git files (content is managed by shared_ptr)
}

int64_t GitFileHandle::Read(void *buffer, idx_t nr_bytes) {
    if (!content_ || position_ >= content_->size()) {
        return 0;  // EOF
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

unique_ptr<FileHandle> GitFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                              optional_ptr<FileOpener> opener) {
    if (flags.OpenForWriting()) {
        throw InternalException("GitFileSystem: Write operations not supported");
    }
    
    try {
        auto git_path = GitPath::Parse(path);
        
        try {
            auto repo = OpenRepository(git_path.repository_path);
            auto commit_obj = ResolveRevision(repo, git_path.revision);
            auto content = GetBlobContent(repo, git_path.file_path, commit_obj);
            
            auto content_ptr = make_shared_ptr<string>(std::move(content));
            return make_uniq<GitFileHandle>(*this, path, content_ptr, flags);
            
        } catch (const std::exception &e) {
            throw IOException("Failed to open git file '%s': %s", path, e.what());
        }
    } catch (const IOException &e) {
        // Re-throw repository discovery errors directly without wrapping
        string error_msg = e.what();
        if (error_msg.find("No git repository found for path") != string::npos) {
            throw;
        }
        throw IOException("Failed to parse git path '%s': %s", path, e.what());
    }
}

vector<OpenFileInfo> GitFileSystem::Glob(const string &pattern, FileOpener *opener) {
    try {
        auto git_path = GitPath::Parse(pattern);
        
        try {
            auto repo = OpenRepository(git_path.repository_path);
            auto commit_obj = ResolveRevision(repo, git_path.revision);
            return ListFiles(repo, git_path.file_path, commit_obj);
            
        } catch (const std::exception &e) {
            throw IOException("Failed to glob git pattern '%s': %s", pattern, e.what());
        }
    } catch (const IOException &e) {
        // Re-throw repository discovery errors directly without wrapping
        string error_msg = e.what();
        if (error_msg.find("No git repository found for path") != string::npos) {
            throw;
        }
        throw IOException("Failed to parse git path '%s': %s", pattern, e.what());
    }
}

bool GitFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
    try {
        auto git_path = GitPath::Parse(filename);
        
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
        // For repository discovery errors, we still return false for FileExists
        // but this maintains consistency with other methods
        return false;
    }
}

int64_t GitFileSystem::GetFileSize(FileHandle &handle) {
    auto &git_handle = handle.Cast<GitFileHandle>();
    return git_handle.GetFileSize();
}

time_t GitFileSystem::GetLastModifiedTime(FileHandle &handle) {
    // For git files, return current time since git objects are immutable
    return time(nullptr);
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
    auto &git_handle = handle.Cast<GitFileHandle>();
    return git_handle.Read(buffer, static_cast<idx_t>(nr_bytes));
}

void GitFileSystem::Seek(FileHandle &handle, idx_t location) {
    auto &git_handle = handle.Cast<GitFileHandle>();
    git_handle.Seek(location);
}

idx_t GitFileSystem::SeekPosition(FileHandle &handle) {
    auto &git_handle = handle.Cast<GitFileHandle>();
    return git_handle.SeekPosition();
}

void GitFileSystem::Reset(FileHandle &handle) {
    auto &git_handle = handle.Cast<GitFileHandle>();
    git_handle.Reset();
}


//===--------------------------------------------------------------------===//
// Git Operations
//===--------------------------------------------------------------------===//

git_repository* GitFileSystem::OpenRepository(const string &repo_path) {
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
        throw IOException("Failed to open git repository '%s': %s", 
                         repo_path, e ? e->message : "Unknown error");
    }
    
    // Cache and return
    repo_cache_[repo_path] = repo;
    return repo;
}

git_object* GitFileSystem::ResolveRevision(git_repository *repo, const string &revision) {
    git_object *obj = nullptr;
    int error = git_revparse_single(&obj, repo, revision.c_str());
    if (error != 0) {
        const git_error *e = git_error_last();
        throw IOException("Failed to resolve revision '%s': %s", 
                         revision, e ? e->message : "Unknown error");
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
        throw IOException("File '%s' not found in tree: %s", 
                         file_path, e ? e->message : "Unknown error");
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
    string result(static_cast<const char*>(content), size);
    
    git_blob_free(blob);
    return result;
}

vector<OpenFileInfo> GitFileSystem::ListFiles(git_repository *repo, const string &pattern, git_object *commit_obj) {
    // For now, implement basic pattern matching
    // TODO: Implement full tree walking with glob patterns
    vector<OpenFileInfo> results;
    
    // If pattern is empty or just *, return all files (simplified for now)
    if (pattern.empty() || pattern == "*") {
        // This is a simplified implementation
        // In a full implementation, we'd walk the entire tree
        return results;
    }
    
    // For specific files, try to match exactly
    try {
        GetBlobContent(repo, pattern, commit_obj);
        results.emplace_back(OpenFileInfo{"git://" + pattern});
    } catch (...) {
        // File doesn't exist, return empty results
    }
    
    return results;
}

//===--------------------------------------------------------------------===//
// Repository Discovery
//===--------------------------------------------------------------------===//

static bool IsGitRepository(const string &path) {
    // Try to open the repository with libgit2 - most reliable method
    git_repository *repo = nullptr;
    int error = git_repository_open(&repo, path.c_str());
    
    if (error == 0) {
        git_repository_free(repo);
        return true;
    }
    
    return false;
}

static bool PathExists(const string &path) {
    struct stat path_stat;
    return stat(path.c_str(), &path_stat) == 0;
}

static bool IsDirectory(const string &path) {
    struct stat path_stat;
    if (stat(path.c_str(), &path_stat) != 0) {
        return false;
    }
    return S_ISDIR(path_stat.st_mode);
}

// Normalizes a path by resolving relative components (./ and ../) and converting to absolute path
static string NormalizePath(const string &path) {
    string current_path = path;
    
    // Resolve relative paths to absolute paths
    if (!current_path.empty() && current_path[0] != '/') {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != nullptr) {
            current_path = string(cwd) + "/" + current_path;
        }
    }
    
    // Normalize by resolving .. and . components
    vector<string> components;
    stringstream ss(current_path);
    string component;
    
    if (current_path[0] == '/') {
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
        return "/";
    } else {
        string result = "";
        for (const auto& comp : components) {
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

// Finds the git repository root directory by walking up the directory tree from the given path
// Implements Option 2: walks up from non-existent paths to first existing directory, then searches for .git
static string FindGitRepository(const string &path) {
    // Normalize the path (resolves relative paths and .. components)  
    string current_path = NormalizePath(path);
    
    // Option 2: Walk up the path until we find something that exists on disk
    while (!current_path.empty() && current_path != "/" && !PathExists(current_path)) {
        current_path = GetParentDirectory(current_path);
    }
    
    // If we couldn't find any existing path, start from current directory
    if (!PathExists(current_path)) {
        current_path = ".";
    }
    
    // If path points to a file (not directory), start from its directory
    if (!IsDirectory(current_path)) {
        string dir = GetDirectoryFromPath(current_path);
        if (!dir.empty()) {
            current_path = dir;
        }
    }
    
    // Walk up directory tree looking for .git
    while (!current_path.empty() && current_path != "/") {
        if (IsGitRepository(current_path)) {
            return current_path;
        }
        current_path = GetParentDirectory(current_path);
    }
    
    // Check root directory too
    if (current_path == "/" && IsGitRepository("/")) {
        return "/";
    }
    
    throw IOException("No git repository found for path: %s", path);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void RegisterGitFileSystem(DatabaseInstance &db) {
    auto &fs = FileSystem::GetFileSystem(db);
    fs.RegisterSubSystem(make_uniq<GitFileSystem>());
}

} // namespace duckdb
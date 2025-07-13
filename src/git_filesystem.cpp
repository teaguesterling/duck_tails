#include "git_filesystem.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception.hpp"
#include <regex>
#include <fnmatch.h>

namespace duckdb {

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
    
    // Find first slash to separate repo path from file path
    size_t slash_pos = url.find('/');
    if (slash_pos != string::npos) {
        result.repository_path = url.substr(0, slash_pos);
        result.file_path = url.substr(slash_pos + 1);
    } else {
        result.repository_path = url;
        result.file_path = "";
    }
    
    // If repo path is empty, use current directory
    if (result.repository_path.empty()) {
        result.repository_path = ".";
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
}

vector<OpenFileInfo> GitFileSystem::Glob(const string &pattern, FileOpener *opener) {
    auto git_path = GitPath::Parse(pattern);
    
    try {
        auto repo = OpenRepository(git_path.repository_path);
        auto commit_obj = ResolveRevision(repo, git_path.revision);
        return ListFiles(repo, git_path.file_path, commit_obj);
        
    } catch (const std::exception &e) {
        throw IOException("Failed to glob git pattern '%s': %s", pattern, e.what());
    }
}

bool GitFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
    try {
        auto git_path = GitPath::Parse(filename);
        auto repo = OpenRepository(git_path.repository_path);
        auto commit_obj = ResolveRevision(repo, git_path.revision);
        
        // Try to get the blob content - if it succeeds, file exists
        GetBlobContent(repo, git_path.file_path, commit_obj);
        return true;
        
    } catch (...) {
        return false;
    }
}

int64_t GitFileSystem::GetFileSize(FileHandle &handle) {
    return handle.GetFileSize();
}

time_t GitFileSystem::GetLastModifiedTime(FileHandle &handle) {
    // For git files, return current time since git objects are immutable
    return time(nullptr);
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
// Registration
//===--------------------------------------------------------------------===//

void RegisterGitFileSystem(DatabaseInstance &db) {
    auto git_fs = make_uniq<GitFileSystem>();
    db.GetFileSystem().RegisterSubSystem(std::move(git_fs));
}

} // namespace duckdb
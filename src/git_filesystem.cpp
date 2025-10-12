#include "git_filesystem.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception.hpp"
#include <regex>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <fstream>
#include <iostream>

namespace duckdb {

//===--------------------------------------------------------------------===//
// GitPath Implementation
//===--------------------------------------------------------------------===//

GitPath GitPath::Parse(const string &git_url) {
    GitPath result;
    
    printf("[DEBUG] GitPath::Parse input: '%s'\n", git_url.c_str());
    fflush(stdout);
    
    // Remove git:// prefix
    string url = git_url;
    if (StringUtil::StartsWith(url, "git://")) {
        url = url.substr(6);
    }
    printf("[DEBUG] After removing git:// prefix: '%s'\n", url.c_str());
    fflush(stdout);
    
    // Find @ symbol to separate path from revision
    size_t at_pos = url.find_last_of('@');
    printf("[DEBUG] Found @ at position: %zu (npos=%zu)\n", at_pos, string::npos);
    fflush(stdout);
    
    if (at_pos != string::npos) {
        result.revision = url.substr(at_pos + 1);
        url = url.substr(0, at_pos);
        printf("[DEBUG] Extracted revision: '%s', remaining url: '%s'\n", 
               result.revision.c_str(), url.c_str());
        fflush(stdout);
    } else {
        result.revision = "HEAD";
        printf("[DEBUG] No @ found, defaulting to HEAD\n");
        fflush(stdout);
    }
    
    // For now, assume the entire remaining path is a file path relative to current directory
    // TODO: In future, implement smarter repository detection
    result.repository_path = ".";
    result.file_path = url;
    
    printf("[DEBUG] Final GitPath: repo='%s', file='%s', revision='%s'\n", 
           result.repository_path.c_str(), result.file_path.c_str(), result.revision.c_str());
    fflush(stdout);
    
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
    
    auto git_path = GitPath::Parse(path);
    printf("[DEBUG] OpenFile called with path: '%s'\n", path.c_str());
    printf("[DEBUG] GitPath parsed: repo=%s, file=%s, revision=%s\n", 
           git_path.repository_path.c_str(), git_path.file_path.c_str(), git_path.revision.c_str());
    fflush(stdout);
    
    try {
        auto repo = OpenRepository(git_path.repository_path);
        printf("[DEBUG] About to resolve revision: '%s'\n", git_path.revision.c_str());
        fflush(stdout);
        auto commit_obj = ResolveRevision(repo, git_path.revision);
        printf("[DEBUG] Commit resolved for revision: %s\n", git_path.revision.c_str());
        fflush(stdout);
        auto content = GetBlobContent(repo, git_path.file_path, commit_obj);
        
        // Check if this is an LFS pointer file
        if (IsLFSPointer(content)) {
            auto lfs_info = ParseLFSPointer(content);
            printf("[DEBUG] LFS file detected: path=%s, oid=%s, size=%ld\n", 
                   path.c_str(), lfs_info.oid.c_str(), lfs_info.size);
            fflush(stdout);
            
            // Use GitLFSFileHandle for streaming support (Phase 2)
            return make_uniq<GitLFSFileHandle>(*this, path, std::move(lfs_info), flags, opener, repo);
        }
        
        // Regular git file - use existing flow
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
    printf("[DEBUG] FileExists called with filename: '%s'\n", filename.c_str());
    fflush(stdout);
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
    if (auto *lfs_handle = dynamic_cast<GitLFSFileHandle*>(&handle)) {
        return lfs_handle->GetFileSize();
    } else {
        auto &git_handle = handle.Cast<GitFileHandle>();
        return git_handle.GetFileSize();
    }
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
    if (auto *lfs_handle = dynamic_cast<GitLFSFileHandle*>(&handle)) {
        return lfs_handle->Read(buffer, static_cast<idx_t>(nr_bytes));
    } else {
        auto &git_handle = handle.Cast<GitFileHandle>();
        return git_handle.Read(buffer, static_cast<idx_t>(nr_bytes));
    }
}

void GitFileSystem::Seek(FileHandle &handle, idx_t location) {
    if (auto *lfs_handle = dynamic_cast<GitLFSFileHandle*>(&handle)) {
        lfs_handle->Seek(location);
    } else {
        auto &git_handle = handle.Cast<GitFileHandle>();
        git_handle.Seek(location);
    }
}

idx_t GitFileSystem::SeekPosition(FileHandle &handle) {
    if (auto *lfs_handle = dynamic_cast<GitLFSFileHandle*>(&handle)) {
        return lfs_handle->SeekPosition();
    } else {
        auto &git_handle = handle.Cast<GitFileHandle>();
        return git_handle.SeekPosition();
    }
}

void GitFileSystem::Reset(FileHandle &handle) {
    if (auto *lfs_handle = dynamic_cast<GitLFSFileHandle*>(&handle)) {
        lfs_handle->Reset();
    } else {
        auto &git_handle = handle.Cast<GitFileHandle>();
        git_handle.Reset();
    }
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
// GitLFSFileHandle Implementation
//===--------------------------------------------------------------------===//

GitLFSFileHandle::GitLFSFileHandle(FileSystem &file_system, const string &path, LFSInfo lfs_info, 
                                   FileOpenFlags flags, optional_ptr<FileOpener> opener, git_repository *repo)
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
    printf("[DEBUG] EnsureRemoteHandleOpened: oid=%s, local_path=%s\n", 
           lfs_info_.oid.c_str(), local_path.c_str());
    fflush(stdout);
    
    // Use simple C++ file check instead of filesystem abstraction
    std::ifstream test_file(local_path);
    if (test_file.good()) {
        test_file.close();
        printf("[DEBUG] Local LFS file found, opening: %s\n", local_path.c_str());
        fflush(stdout);
        // File exists locally, use LocalFileSystem to open it  
        local_fs_ = make_uniq<LocalFileSystem>();
        remote_handle_ = local_fs_->OpenFile(local_path, flags, opener_);
    } else {
        printf("[DEBUG] Local LFS file NOT found: %s\n", local_path.c_str());
        fflush(stdout);
        // Try to get remote download URL and open via DuckDB filesystem
        download_url_ = ResolveLFSDownloadURL();
        remote_handle_ = file_system.OpenFile(download_url_, flags, opener_);
    }
    
    remote_handle_opened_ = true;
}

string GitLFSFileHandle::BuildLFSObjectPath(const string &oid) {
    if (oid.length() < 4) {
        throw IOException("Invalid LFS OID: too short");
    }
    
    if (!repo_) {
        throw IOException("No repository context available for LFS object path");
    }
    
    const char* repo_path = git_repository_path(repo_);
    if (!repo_path) {
        throw IOException("Could not get repository path");
    }
    
    // Build path: .git/lfs/objects/ab/cd/abcd1234...
    string lfs_path = string(repo_path) + "lfs/objects/" + 
                      oid.substr(0, 2) + "/" + 
                      oid.substr(2, 2) + "/" + 
                      oid;
    
    return lfs_path;
}

string GitLFSFileHandle::ResolveLFSDownloadURL() {
    // TODO: Implement LFS Batch API call
    // For now, throw an error directing users to use local LFS
    string local_path = BuildLFSObjectPath(lfs_info_.oid);
    throw IOException("Remote LFS not yet implemented. Run 'git lfs pull' to download LFS objects locally. Tried local path: %s", local_path);
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
        return false;  // LFS pointers are typically < 200 bytes
    }
    
    // Check for LFS signature
    return content.find("version https://git-lfs.github.com/spec/v1") == 0 &&
           content.find("oid sha256:") != string::npos &&
           content.find("size ") != string::npos;
}

LFSInfo GitFileSystem::ParseLFSPointer(const string &pointer_content) {
    LFSInfo lfs_info;
    
    // Parse line by line
    std::istringstream stream(pointer_content);
    string line;
    
    while (std::getline(stream, line)) {
        if (StringUtil::StartsWith(line, "version ")) {
            lfs_info.version = line.substr(8);  // Skip "version "
        } else if (StringUtil::StartsWith(line, "oid sha256:")) {
            lfs_info.oid = line.substr(11);  // Skip "oid sha256:"
        } else if (StringUtil::StartsWith(line, "size ")) {
            try {
                lfs_info.size = std::stoll(line.substr(5));  // Skip "size "
            } catch (const std::exception &e) {
                throw IOException("Invalid LFS pointer: invalid size value");
            }
        }
    }
    
    // Validate required fields  
    if (lfs_info.oid.empty() || lfs_info.size <= 0) {
        throw IOException("Invalid LFS pointer: missing required fields");
    }
    
    return lfs_info;
}

string GitFileSystem::BuildLFSObjectPath(git_repository *repo, const string &oid) {
    if (oid.length() < 4) {
        throw IOException("Invalid LFS OID: too short");
    }
    
    const char* repo_path = git_repository_path(repo);
    if (!repo_path) {
        throw IOException("Could not get repository path");
    }
    
    // Build path: .git/lfs/objects/ab/cd/abcd1234...
    string lfs_path = string(repo_path) + "lfs/objects/" + 
                      oid.substr(0, 2) + "/" + 
                      oid.substr(2, 2) + "/" + 
                      oid;
    
    return lfs_path;
}

LFSConfig GitFileSystem::ReadLFSConfig(git_repository *repo) {
    LFSConfig config;
    
    // Try to read .lfsconfig file in repository root
    const char* workdir = git_repository_workdir(repo);
    if (workdir) {
        string config_path = string(workdir) + ".lfsconfig";
        
        // For now, implement basic config reading
        // TODO: Implement proper git config parsing
        
        // Default: construct LFS URL from git remote
        git_remote* remote = nullptr;
        int error = git_remote_lookup(&remote, repo, "origin");
        if (error == 0) {
            const char* url = git_remote_url(remote);
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

void RegisterGitFileSystem(DatabaseInstance &db) {
    auto &fs = FileSystem::GetFileSystem(db);
    fs.RegisterSubSystem(make_uniq<GitFileSystem>());
}

} // namespace duckdb
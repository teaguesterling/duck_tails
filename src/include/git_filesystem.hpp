#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/exception.hpp"
#include <git2.h>
#include <memory>
#include <string>
#include <fstream>

namespace duckdb {

//===--------------------------------------------------------------------===//
// LFS Support Structures
//===--------------------------------------------------------------------===//

struct LFSInfo {
    string oid;        // SHA256 hash of the object
    int64_t size;      // Size in bytes
    string version;    // LFS spec version (usually "https://git-lfs.github.com/spec/v1")
    
    LFSInfo() : size(0) {}
    LFSInfo(const string &oid, int64_t size, const string &version = "https://git-lfs.github.com/spec/v1")
        : oid(oid), size(size), version(version) {}
};

struct LFSConfig {
    string lfs_url;              // Base LFS server URL
    string access_token;         // Optional access token
    std::unordered_map<string, string> headers;  // Additional headers
    
    LFSConfig() = default;
    LFSConfig(const string &url) : lfs_url(url) {}
};

struct LFSBatchRequest {
    string operation = "download";           // "download" or "upload"  
    vector<string> transfers = {"basic"};    // Transfer adapters
    vector<LFSInfo> objects;                 // Objects to transfer
};

struct LFSAction {
    string href;                                    // Download/upload URL
    std::unordered_map<string, string> header;     // HTTP headers
    int64_t expires_in = 0;                        // Expiration in seconds
};

struct LFSObjectResponse {
    string oid;
    int64_t size;
    std::unordered_map<string, LFSAction> actions;  // "download", "upload", "verify"
    bool authenticated = false;
    // Error handling
    int error_code = 0;
    string error_message;
};

struct LFSBatchResponse {
    string transfer = "basic";
    vector<LFSObjectResponse> objects;
    string message;  // Optional server message
};

//===--------------------------------------------------------------------===//
// GitPath Implementation
//===--------------------------------------------------------------------===//

struct GitPath {
    string repository_path;  // Local repo path or remote URL
    string file_path;       // Path within repo (can include glob patterns)
    string revision;        // Branch, tag, commit hash, or range
    
    static GitPath Parse(const string &git_url);
    string ToString() const;
};

class GitFileHandle : public FileHandle {
public:
    GitFileHandle(FileSystem &file_system, const string &path, shared_ptr<string> content, FileOpenFlags flags);
    ~GitFileHandle() override = default;
    
    void Close() override;
    
    // FileHandle interface methods for git files
    int64_t Read(void *buffer, idx_t nr_bytes);
    void Write(void *buffer, idx_t nr_bytes);
    int64_t GetFileSize();
    void Seek(idx_t location);
    idx_t SeekPosition();
    void Reset();
    
    
    // Public accessors for GitFileSystem to use
    const string& GetContent() const { return *content_; }
    idx_t GetPosition() const { return position_; }
    void SetPosition(idx_t pos) { position_ = pos; }
    
private:
    shared_ptr<string> content_;
    idx_t position_;
};

//===--------------------------------------------------------------------===//
// GitLFSFileHandle - Streaming LFS File Support
//===--------------------------------------------------------------------===//

class GitLFSFileHandle : public FileHandle {
public:
    GitLFSFileHandle(FileSystem &file_system, const string &path, LFSInfo lfs_info, 
                     FileOpenFlags flags, optional_ptr<FileOpener> opener, git_repository *repo);
    ~GitLFSFileHandle() override = default;
    
    void Close() override;
    
    // FileHandle interface methods for streaming LFS files
    int64_t Read(void *buffer, idx_t nr_bytes);
    void Write(void *buffer, idx_t nr_bytes);
    int64_t GetFileSize();
    void Seek(idx_t location);
    idx_t SeekPosition();
    void Reset();
    
    // Progress reporting for large downloads
    idx_t GetProgress() override;
    
private:
    void EnsureRemoteHandleOpened();
    string ResolveLFSDownloadURL();
    LFSConfig ReadLFSConfig();
    string BuildLFSObjectPath(const string &oid);
    
    LFSInfo lfs_info_;
    unique_ptr<FileHandle> remote_handle_;
    unique_ptr<LocalFileSystem> local_fs_;
    optional_ptr<FileOpener> opener_;
    git_repository *repo_;
    bool remote_handle_opened_ = false;
    string download_url_;
};

class GitFileSystem : public FileSystem {
public:
    GitFileSystem();
    ~GitFileSystem() override;
    
    // Required FileSystem interface
    string GetName() const override { return "GitFileSystem"; }
    bool CanHandleFile(const string &fpath) override;
    
    // Core FileSystem interface
    unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
                                   optional_ptr<FileOpener> opener = nullptr) override;
    
    vector<OpenFileInfo> Glob(const string &pattern, FileOpener *opener = nullptr) override;
    
    bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
    
    int64_t GetFileSize(FileHandle &handle) override;
    
    time_t GetLastModifiedTime(FileHandle &handle) override;
    
    // File handle properties required by DuckDB
    bool CanSeek() override;
    bool OnDiskFile(FileHandle &handle) override;
    bool IsPipe(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
    
    // File operations delegation to GitFileHandle
    int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
    void Seek(FileHandle &handle, idx_t location) override;
    idx_t SeekPosition(FileHandle &handle) override;
    void Reset(FileHandle &handle) override;
    
private:
    // Git repository management
    git_repository* OpenRepository(const string &repo_path);
    git_object* ResolveRevision(git_repository *repo, const string &revision);
    string GetBlobContent(git_repository *repo, const string &file_path, git_object *commit_obj);
    vector<OpenFileInfo> ListFiles(git_repository *repo, const string &pattern, git_object *commit_obj);
    
    // LFS support methods
    bool IsLFSPointer(const string &content);
    LFSInfo ParseLFSPointer(const string &pointer_content);
    LFSConfig ReadLFSConfig(git_repository *repo);
    string BuildLFSObjectPath(git_repository *repo, const string &oid);
    LFSBatchResponse CallLFSBatchAPI(const LFSConfig &config, const LFSInfo &lfs_info);
    
    // Cache for opened repositories
    std::unordered_map<string, git_repository*> repo_cache_;
};

void RegisterGitFileSystem(DatabaseInstance &db);

} // namespace duckdb
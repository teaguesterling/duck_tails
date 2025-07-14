#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/exception.hpp"
#include <git2.h>
#include <memory>
#include <string>

namespace duckdb {

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
    
    // Cache for opened repositories
    std::unordered_map<string, git_repository*> repo_cache_;
};

void RegisterGitFileSystem(DatabaseInstance &db);

} // namespace duckdb
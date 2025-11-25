#pragma once

#include <git2.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <thread>

namespace duckdb {

// Thread-local repository pool to avoid repeated repository opens
class GitRepoPool {
public:
    struct RepoHandle {
        git_repository *repo;
        std::string path;
        
        RepoHandle(git_repository *r, const std::string &p) : repo(r), path(p) {}
        ~RepoHandle() {
            if (repo) {
                git_repository_free(repo);
            }
        }
    };
    
    // Get or open a repository for the current thread
    static git_repository* GetRepository(const std::string &path) {
        // Thread-local storage for repository handles
        thread_local std::unordered_map<std::string, std::unique_ptr<RepoHandle>> thread_repos;
        
        auto it = thread_repos.find(path);
        if (it != thread_repos.end()) {
            return it->second->repo;
        }
        
        // Open new repository
        git_repository *repo = nullptr;
        if (git_repository_open(&repo, path.c_str()) != 0) {
            return nullptr;
        }
        
        // Store in thread-local cache
        thread_repos[path] = std::make_unique<RepoHandle>(repo, path);
        return repo;
    }
    
    // Clear thread-local cache (optional, called at thread exit)
    static void ClearThreadCache() {
        thread_local std::unordered_map<std::string, std::unique_ptr<RepoHandle>> thread_repos;
        thread_repos.clear();
    }
    
    // Global repository cache with mutex protection (for shared read-only access)
    class SharedPool {
    public:
        static SharedPool& Instance() {
            static SharedPool instance;
            return instance;
        }
        
        git_repository* GetSharedRepository(const std::string &path) {
            std::lock_guard<std::mutex> lock(mutex_);
            
            auto it = repos_.find(path);
            if (it != repos_.end()) {
                return it->second->repo;
            }
            
            // Open new repository
            git_repository *repo = nullptr;
            if (git_repository_open(&repo, path.c_str()) != 0) {
                return nullptr;
            }
            
            // Store in shared cache
            repos_[path] = std::make_unique<RepoHandle>(repo, path);
            return repo;
        }
        
        void Clear() {
            std::lock_guard<std::mutex> lock(mutex_);
            repos_.clear();
        }
        
    private:
        SharedPool() = default;
        ~SharedPool() {
            Clear();
        }
        
        std::mutex mutex_;
        std::unordered_map<std::string, std::unique_ptr<RepoHandle>> repos_;
    };
};

// Helper RAII class for temporary repository access
class ScopedGitRepo {
public:
    explicit ScopedGitRepo(const std::string &path) : repo_(nullptr), owned_(false) {
        // Try thread-local cache first
        repo_ = GitRepoPool::GetRepository(path);
        if (!repo_) {
            // Fallback to opening directly
            if (git_repository_open(&repo_, path.c_str()) == 0) {
                owned_ = true;  // We own this repo and must free it
            }
        }
    }
    
    ~ScopedGitRepo() {
        if (repo_ && owned_) {
            git_repository_free(repo_);
        }
    }
    
    git_repository* get() const { return repo_; }
    operator git_repository*() const { return repo_; }
    bool is_valid() const { return repo_ != nullptr; }
    
private:
    git_repository *repo_;
    bool owned_;  // True if we need to free the repo
};

} // namespace duckdb
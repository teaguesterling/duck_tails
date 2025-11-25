#pragma once

#include <git2.h>
#include <string>
#include <memory>
#include <mutex>
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception.hpp"
#include "git_path.hpp"

namespace duckdb {

// Unified parameters structure for git functions
struct UnifiedGitParams {
    string repo_path_or_uri;
    string resolved_repo_path;
    string resolved_file_path;  // For git_read
    string ref;                 // Optional ref parameter
    bool has_embedded_ref;      // True if ref came from git:// URI
    
    UnifiedGitParams() : repo_path_or_uri("."), resolved_repo_path("."), resolved_file_path(""), ref("HEAD"), has_embedded_ref(false) {}
};

// Parse parameters using new unified signature: func(repo_path_or_uri, [optional_ref], [other_params...])
UnifiedGitParams ParseUnifiedGitParams(TableFunctionBindInput &input, int ref_param_index = 1);

// Parse parameters for LATERAL functions where repo_path comes from runtime DataChunk
UnifiedGitParams ParseLateralGitParams(TableFunctionBindInput &input, int ref_param_index = 1);

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
    GitRepository(const GitRepository&) = delete;
    GitRepository& operator=(const GitRepository&) = delete;
    
    // Enable move
    GitRepository(GitRepository&& other) noexcept : repo(other.repo) {
        other.repo = nullptr;
    }
    
    GitRepository& operator=(GitRepository&& other) noexcept {
        if (this != &other) {
            if (repo) {
                git_repository_free(repo);
            }
            repo = other.repo;
            other.repo = nullptr;
        }
        return *this;
    }
    
    git_repository* get() const { return repo; }
    operator git_repository*() const { return repo; }
    
private:
    git_repository *repo;
};

// RAII wrapper for git objects
template<typename T>
class GitObject {
public:
    using FreeFunc = void(*)(T*);
    
    GitObject(T* obj, FreeFunc free_func) : obj(obj), free_func(free_func) {}
    
    ~GitObject() {
        if (obj && free_func) {
            free_func(obj);
        }
    }
    
    // Disable copy
    GitObject(const GitObject&) = delete;
    GitObject& operator=(const GitObject&) = delete;
    
    // Enable move
    GitObject(GitObject&& other) noexcept : obj(other.obj), free_func(other.free_func) {
        other.obj = nullptr;
    }
    
    GitObject& operator=(GitObject&& other) noexcept {
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
    
    T* get() const { return obj; }
    operator T*() const { return obj; }
    T* release() { 
        T* tmp = obj;
        obj = nullptr;
        return tmp;
    }
    
private:
    T* obj;
    FreeFunc free_func;
};

// Helper factory functions for common git objects
using GitCommitPtr = GitObject<git_commit>;
using GitTreePtr = GitObject<git_tree>;
using GitRevwalkPtr = GitObject<git_revwalk>;
using GitBranchIteratorPtr = GitObject<git_branch_iterator>;

inline GitCommitPtr MakeGitCommit(git_commit* commit) {
    return GitCommitPtr(commit, reinterpret_cast<void(*)(git_commit*)>(git_commit_free));
}

inline GitTreePtr MakeGitTree(git_tree* tree) {
    return GitTreePtr(tree, reinterpret_cast<void(*)(git_tree*)>(git_tree_free));
}

inline GitRevwalkPtr MakeGitRevwalk(git_revwalk* walker) {
    return GitRevwalkPtr(walker, reinterpret_cast<void(*)(git_revwalk*)>(git_revwalk_free));
}

inline GitBranchIteratorPtr MakeGitBranchIterator(git_branch_iterator* iter) {
    return GitBranchIteratorPtr(iter, reinterpret_cast<void(*)(git_branch_iterator*)>(git_branch_iterator_free));
}

// Note: libgit2 is initialized once at extension load time in duck_tails_extension.cpp
// Individual functions should NOT call git_libgit2_init() or git_libgit2_shutdown()

} // namespace duckdb
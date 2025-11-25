#include "git_history.hpp"
#include "duckdb.hpp"

namespace duckdb {

static bool PathExistsInTree(git_tree *tree, const std::string &path) {
    git_tree_entry *e = nullptr;
    const bool exists = (git_tree_entry_bypath(&e, tree, path.c_str()) == 0);
    if (e) git_tree_entry_free(e);
    return exists;
}

bool FileChangedInCommit(git_repository *repo, git_commit *commit, const std::string &path) {
    if (!repo || !commit) {
        throw std::runtime_error("FileChangedInCommit: Invalid repository or commit");
    }
    if (path.empty()) {
        return false;  // Expected case: no file path means no filtering
    }

    git_tree *tree = nullptr;
    if (git_commit_tree(&tree, commit) != 0 || !tree) {
        throw std::runtime_error("FileChangedInCommit: Failed to get commit tree");
    }

    const unsigned nparents = git_commit_parentcount(commit);

    // Root commit: first appearance counts as a change
    if (nparents == 0) {
        const bool exists = PathExistsInTree(tree, path);
        git_tree_free(tree);
        return exists;
    }

    // Path-restricted diff options
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    const char *p = path.c_str();
    git_strarray ps{};
    ps.count = 1;
    ps.strings = const_cast<char **>(&p); // libgit2 won't mutate memory
    opts.pathspec = ps;

    auto changed_vs = [&](unsigned idx) -> bool {
        git_commit *parent = nullptr; 
        git_tree *pt = nullptr; 
        git_diff *d = nullptr;
        bool changed = false;
        
        if (git_commit_parent(&parent, commit, idx) == 0 &&
            git_commit_tree(&pt, parent) == 0 &&
            git_diff_tree_to_tree(&d, repo, pt, tree, &opts) == 0) {
            changed = git_diff_num_deltas(d) > 0;
        }
        
        if (d) git_diff_free(d);
        if (pt) git_tree_free(pt);
        if (parent) git_commit_free(parent);
        return changed;
    };

    if (nparents == 1) {
        bool changed = changed_vs(0);
        git_tree_free(tree);
        return changed;
    }

    // Merge: include only if changed vs *all* parents (FIXES THE BUG)
    bool all_changed = true;
    for (unsigned i = 0; i < nparents; ++i) {
        if (!changed_vs(i)) { 
            all_changed = false; 
            break; 
        }
    }
    git_tree_free(tree);
    return all_changed;
}

} // namespace duckdb
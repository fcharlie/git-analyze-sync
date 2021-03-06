////////
#include "executor.hpp"
#include <git2.h>
#include <console.hpp>

struct commit_t {
  commit_t() = default;
  ~commit_t() {
    if (tree != nullptr) {
      git_tree_free(tree);
    }
    if (commit != nullptr) {
      git_commit_free(commit);
    }
  }
  git_commit *commit{nullptr};
  git_tree *tree{nullptr};
  bool lookup(git_repository *r, std::string_view rev) {
    git_oid id;
    if (git_oid_fromstrn(&id, rev.data(), rev.size()) != 0) {
      return false;
    }
    git_object *obj = nullptr;
    if (git_object_lookup(&obj, r, &id, GIT_OBJECT_ANY) != 0) {
      return false;
    }
    switch (git_object_type(obj)) {
    case GIT_OBJECT_COMMIT:
      commit = reinterpret_cast<git_commit *>(obj);
      break;
    case GIT_OBJECT_TAG: {
      git_object *po = nullptr;
      if (git_object_peel(&po, obj, GIT_OBJECT_COMMIT) != 0) {
        git_object_free(obj);
        return false;
      }
      git_object_free(obj);
      commit = reinterpret_cast<git_commit *>(po);
    } break;
    default:
      git_object_free(obj);
      return false;
    }
    return git_commit_tree(&tree, commit) == 0;
  }
};

class executor_base {
public:
  executor_base() {
    //
    git_libgit2_init();
  }
  ~executor_base() {
    if (repo_ != nullptr) {
      git_repository_free(repo_);
    }
    git_libgit2_shutdown();
  }
  bool open(std::string_view path) {
    return git_repository_open(&repo_, path.data()) == 0;
  }
  git_repository *repo() { return repo_; }

private:
  git_repository *repo_{nullptr};
};

Executor::Executor() {
  // initialzie todo
  base = new executor_base();
}
Executor::~Executor() {
  // delete
  delete base;
}

bool Executor::InitializeRules(std::string_view sv, std::string_view ref) {
  if (ref.compare(0, sizeof("refs/heads/") - 1, "refs/heads/") != 0) {
    return true;
  }
  auto branch = ref.substr(sizeof("refs/heads/") - 1);
  return engine.PreInitialize(sv, branch);
}

// The entries will be traversed in the specified order, children subtrees will
// be automatically loaded as required, and the callback will be called once per
// entry with the current (relative) root for the entry and the entry data
// itself.
// If the callback returns a positive value, the passed entry will be skipped on
// the traversal (in pre mode). A negative value stops the walk.
// https://libgit2.org/libgit2/#HEAD/group/tree/git_tree_walk
int git_treewalk_impl(const char *root, const git_tree_entry *entry,
                      void *payload) {
  auto e = reinterpret_cast<Executor *>(payload);
  std::string name = root;
  name.append(git_tree_entry_name(entry));
  if (e->FullMatch(name)) {
    fprintf(stderr, "Path %s is readonly\n", name.c_str());
    return -1;
  }
  return 0;
}
bool Executor::ExecuteTreeWalk(std::string_view rev) {
  commit_t commit;
  if (!commit.lookup(base->repo(), rev)) {
    aze::FPrintF(stderr, "unable open ref: %s\n", rev);
    return false;
  }
  return (git_tree_walk(commit.tree, GIT_TREEWALK_PRE, git_treewalk_impl,
                        this) == 0);
}

// This will iterate through all of the files described in a diff. You should
// provide a file callback to learn about each file.

// The "hunk" and "line" callbacks are optional, and the text diff of the files
// will only be calculated if they are not NULL. Of course, these callbacks will
// not be invoked for binary files on the diff or for files whose only changed
// is a file mode change.

// Returning a non-zero value from any of the callbacks will terminate the
// iteration and return the value to the user.
// https://libgit2.org/libgit2/#HEAD/group/diff/git_diff_foreach
int git_diff_callback(const git_diff_delta *delta, float progress,
                      void *payload) {
  (void)progress;
  auto e = reinterpret_cast<Executor *>(payload);
  switch (delta->status) {
  case GIT_DELTA_ADDED:
    /*fallthrough*/
  case GIT_DELTA_MODIFIED:
  /*fallthrough*/
  case GIT_DELTA_COPIED: // copy to new path
    if (e->FullMatch(delta->new_file.path)) {
      fprintf(stderr, "Path '%s' is readonly\n", delta->old_file.path);
      return 1;
    }
    break;
  case GIT_DELTA_DELETED:
    if (e->FullMatch(delta->old_file.path)) {
      fprintf(stderr, "Path '%s' is readonly\n", delta->old_file.path);
      return 1;
    }
    break;
  default:
    // ex. GIT_DELTA_RENAMED
    if (e->FullMatch(delta->new_file.path)) {
      fprintf(stderr, "Path '%s' is readonly\n", delta->old_file.path);
      return 1;
    }
    if (e->FullMatch(delta->old_file.path)) {
      fprintf(stderr, "Path '%s' is readonly\n", delta->old_file.path);
      return 1;
    }
    break;
  }
  return 0;
}

struct diff_t {
  ~diff_t() {
    if (p != nullptr) {
      git_diff_free(p);
    }
  }
  git_diff *p{nullptr};
};

bool Executor::Execute(std::string_view path, std::string_view oldrev,
                       std::string_view newrev) {
  if (engine.Empty()) {
    // Engine rules empty so return
    return true;
  }
  if (!base->open(path)) {
    return false;
  }
  constexpr const char zerooid[] = "0000000000000000000000000000000000000000";
  if (oldrev == zerooid) {
    return ExecuteTreeWalk(newrev);
  }
  if (newrev == zerooid) {
    return ExecuteTreeWalk(oldrev);
  }
  commit_t oldcommit, newcommit;
  if (!oldcommit.lookup(base->repo(), oldrev)) {
    return false;
  }
  if (!newcommit.lookup(base->repo(), newrev)) {
    aze::FPrintF(stderr, "unable resolve ref: %s\n", newrev);
    return false;
  }
  diff_t diff;
  git_diff_options opts;
  git_diff_init_options(&opts, GIT_DIFF_OPTIONS_VERSION);
  if (git_diff_tree_to_tree(&diff.p, base->repo(), oldcommit.tree,
                            newcommit.tree, &opts) != 0) {
    return false;
  }
  return git_diff_foreach(diff.p, git_diff_callback, nullptr, nullptr, nullptr,
                          this) == 0;
}

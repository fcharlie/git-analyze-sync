/*
* driver.cc
* git-rollback
* author: Force.Charlie
* Date: 2016.08
* Copyright (C) 2016. OSChina.NET. All Rights Reserved.
*/
#include <Argv.hpp>
#include "rollback.hpp"

/*
* git-rollback --git-dir=/path/to/repo --backrev=7
* git-rollback --git-dir /path/to/repo --backrev 7
* git-rollback --git-dir=/path/to/repo --backid=a59a9bb06a
*/

void RollbackUsage() {
  const char *kUsage =
      R"(OVERVIEW: GIT rollback tools
Usage: git-rollback <options>...] [--] [<pathspec>...] [<refs|branches> ...]
OPTIONS:
  -h [--help]      print usage and exit
  --git-dir        set rollback repository path
  --backid       set rollback commit id
  --backrev        set rollback current back X rev
  --refname        set rollback current reference name
  --force          force gc prune
)";
  printf("%s\n", kUsage);
}

int ResolveInteger(const char *cstr) {
  char *c;
  auto i = strtol(cstr, &c, 10);
  if (i > 0) {
    return static_cast<int>(i);
  }
  return 0;
}

int ProcessArgs(int Argc, char **Argv, RollbackTaskArgs &taskArgs) {
  const char *va{nullptr};
  for (int i = 1; i < Argc; i++) {
    if (IsArg(Argv[i], "--git-dir", sizeof("--git-dir") - 1, &va)) {
      if (va) {
        taskArgs.gitdir.assign(va);
      } else if (++i < Argc) {
        taskArgs.gitdir.assign(Argv[i]);
      }
    } else if (IsArg(Argv[i], "--backrev", sizeof("--backrev") - 1, &va)) {
      if (va) {
        taskArgs.rev = ResolveInteger(va);
      } else if (++i < Argc) {
        taskArgs.rev = ResolveInteger(Argv[i]);
      }
    } else if (IsArg(Argv[i], "--backid", sizeof("--backid") - 1, &va)) {
      if (va) {
        taskArgs.hexid.assign(va);
      } else if (++i < Argc) {
        taskArgs.hexid.assign(Argv[i]);
      }
    } else if (IsArg(Argv[i], "--refname", sizeof("--refname") - 1, &va)) {
      if (va) {
        taskArgs.refname.assign(va);
      } else if (++i < Argc) {
        taskArgs.refname.assign(Argv[i]);
      }
    } else if (IsArg(Argv[i], "--force")) {

    } else if (IsArg(Argv[i], "-h", "--help")) {
      RollbackUsage();
      exit(0);
    } else {
      /// do some things
    }
  }
  return 0;
}

int main(int argc, char **argv) {
  RollbackTaskArgs taskArgs;
  RollbackDriver driver;
  bool result = false;
  if (taskArgs.hexid.size() > 0) {
    result = driver.RollbackWithCommit(taskArgs.gitdir.c_str(),
                                       taskArgs.refname.c_str(),
                                       taskArgs.hexid.c_str(), taskArgs.forced);
  } else {
    result = driver.RollbackWithRev(taskArgs.gitdir.c_str(),
                                    taskArgs.refname.c_str(), taskArgs.rev,
                                    taskArgs.forced);
  }
  if (result) {
    printf("git-rollback: Operation completed !\n");
  } else {
    fprintf(stderr, "git-rollback: Operation aborted !\n");
  }
  //
  return 0;
}

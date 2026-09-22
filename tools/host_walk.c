/* host_walk -- see host_walk.h for why this is its own translation unit. */
#include "host_walk.h"

#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>

#define HOST_WALK_PATH_MAX 512

/* BACKLOG #179 A3 review D10: a provable upper bound on the recursion (golden rule 2)
 * -- matches tools/vsd_img.c's own list_walk() depth cap and rationale (the FAT
 * short-path practical ceiling). A template tree nested deeper than this is a setup
 * error, not silently truncated. */
#define HOST_WALK_MAX_DEPTH 16

static int walk_rec(const char* root, const char* relprefix, host_walk_cb cb,
                     void* userdata, int depth) {
  if (depth > HOST_WALK_MAX_DEPTH) {
    fprintf(stderr, "host_walk: directory nesting too deep under %s (> %d)\n", root,
            HOST_WALK_MAX_DEPTH);
    return -1;
  }
  char abspath[HOST_WALK_PATH_MAX];
  int n = snprintf(abspath, sizeof abspath, "%s/%s", root, relprefix);
  if (n < 0 || (size_t)n >= sizeof abspath) return -1;

  DIR* d = opendir(abspath);
  if (!d) return -1;

  struct dirent* de;
  int rc = 0;
  while (rc == 0 && (de = readdir(d)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

    char relpath[HOST_WALK_PATH_MAX];
    if (relprefix[0])
      n = snprintf(relpath, sizeof relpath, "%s/%s", relprefix, de->d_name);
    else
      n = snprintf(relpath, sizeof relpath, "%s", de->d_name);
    if (n < 0 || (size_t)n >= sizeof relpath) { rc = -1; break; }

    char childabs[HOST_WALK_PATH_MAX];
    n = snprintf(childabs, sizeof childabs, "%s/%s", root, relpath);
    if (n < 0 || (size_t)n >= sizeof childabs) { rc = -1; break; }

    struct stat st;
    /* BACKLOG #179 A3 review D10: lstat, not stat -- stat() FOLLOWS a symlink, so a
     * symlink pointing at a regular file or directory was silently INCLUDED (its
     * target's own S_ISREG/S_ISDIR), the opposite of host_walk.h's own documented
     * "symlinks are skipped" contract. lstat() reports the link itself (S_ISLNK,
     * matched by neither branch below), which is what actually gets skipped. */
    if (lstat(childabs, &st) != 0) { rc = -1; break; }

    if (S_ISDIR(st.st_mode)) {
      rc = cb(relpath, 1, userdata);
      if (rc == 0) rc = walk_rec(root, relpath, cb, userdata, depth + 1);
    } else if (S_ISREG(st.st_mode)) {
      rc = cb(relpath, 0, userdata);
    }
    /* symlinks / device nodes / etc: silently skipped, see host_walk.h */
  }
  closedir(d);
  return rc;
}

int host_walk_tree(const char* root, host_walk_cb cb, void* userdata) {
  return walk_rec(root, "", cb, userdata, 0);
}

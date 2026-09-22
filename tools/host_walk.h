#ifndef HOST_WALK_H
#define HOST_WALK_H
/*
 * host_walk -- recursive HOST filesystem walk, kept in its own translation unit for
 * exactly one reason: FatFs' ff.h declares a struct named `DIR` (the directory object)
 * and POSIX <dirent.h> declares an unrelated `DIR` for the SAME name -- the two cannot
 * be included together. tools/vsd_img.c needs both "walk a directory on the Mac" and
 * "walk a directory inside the FAT image"; splitting them into two .c files is the fix,
 * not a macro rename of either DIR (BACKLOG #179 Phase A step A2).
 */

/* Called once per entry strictly beneath `root` (root itself is never passed).
 * `relpath` uses '/' separators, relative to root, no leading slash. `is_dir` is 1 for
 * a directory (visited BEFORE its contents) and 0 for a regular file. Returning
 * nonzero aborts the walk immediately (host_walk_tree then returns that value). */
typedef int (*host_walk_cb)(const char* relpath, int is_dir, void* userdata);

/* Returns 0 on success, the first nonzero value `cb` returned, or -1 if `root` could
 * not be opened at all. Symlinks and anything that is neither a regular file nor a
 * directory are skipped (image-worthy fixtures never need them). */
int host_walk_tree(const char* root, host_walk_cb cb, void* userdata);

#endif /* HOST_WALK_H */

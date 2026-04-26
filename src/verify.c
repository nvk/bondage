#include <CommonCrypto/CommonDigest.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "verify.h"

#define BONDAGE_SHA256_STRLEN ((sizeof("sha256:") - 1) + (CC_SHA256_DIGEST_LENGTH * 2) + 1)

static void
bondage_set_error(char *errbuf, size_t errbufsz, const char *fmt, ...)
{
  va_list ap;

  if (errbuf == NULL || errbufsz == 0) return;

  va_start(ap, fmt);
  vsnprintf(errbuf, errbufsz, fmt, ap);
  va_end(ap);
}

static int
bondage_is_absolute_path(const char *path)
{
  return path != NULL && path[0] == '/';
}

static int
bondage_path_is_within_root(const char *root, const char *path)
{
  size_t root_len;

  root_len = strlen(root);
  if (strncmp(root, path, root_len) != 0) return 0;
  return path[root_len] == '\0' || path[root_len] == '/';
}

static char *
bondage_join_path(const char *left, const char *right)
{
  size_t left_len;
  size_t right_len;
  char *joined;

  left_len = strlen(left);
  right_len = strlen(right);
  joined = malloc(left_len + 1 + right_len + 1);
  if (joined == NULL) return NULL;

  memcpy(joined, left, left_len);
  joined[left_len] = '/';
  memcpy(joined + left_len + 1, right, right_len + 1);
  return joined;
}

static int
bondage_sha256_string(unsigned char digest[CC_SHA256_DIGEST_LENGTH],
                      char *out,
                      size_t outsz)
{
  size_t i;

  if (outsz < BONDAGE_SHA256_STRLEN) return 0;

  memcpy(out, "sha256:", sizeof("sha256:") - 1);
  for (i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
    snprintf(out + (sizeof("sha256:") - 1) + (i * 2), 3, "%02x", digest[i]);
  }
  out[BONDAGE_SHA256_STRLEN - 1] = '\0';
  return 1;
}

static int
bondage_sha256_fd(int fd, char *out, size_t outsz)
{
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  unsigned char buf[8192];
  CC_SHA256_CTX ctx;
  ssize_t nread;

  if (lseek(fd, 0, SEEK_SET) < 0) return 0;

  CC_SHA256_Init(&ctx);
  for (;;) {
    nread = read(fd, buf, sizeof(buf));
    if (nread == 0) break;
    if (nread < 0) return 0;
    CC_SHA256_Update(&ctx, buf, (CC_LONG)nread);
  }
  CC_SHA256_Final(digest, &ctx);

  return bondage_sha256_string(digest, out, outsz);
}

static int
bondage_hash_open_regular_file(const char *path,
                               char *out,
                               size_t outsz,
                               char *errbuf,
                               size_t errbufsz)
{
  struct stat st;
  int fd;
  int ok = 0;

  fd = open(path, O_RDONLY);
  if (fd < 0) {
    bondage_set_error(errbuf, errbufsz, "open failed for %s: %s",
                      path, strerror(errno));
    return 0;
  }

  if (fstat(fd, &st) < 0) {
    bondage_set_error(errbuf, errbufsz, "fstat failed for %s: %s",
                      path, strerror(errno));
    goto cleanup;
  }
  if (!S_ISREG(st.st_mode)) {
    bondage_set_error(errbuf, errbufsz, "not a regular file: %s", path);
    goto cleanup;
  }

  if (!bondage_sha256_fd(fd, out, outsz)) {
    bondage_set_error(errbuf, errbufsz, "hashing failed for %s", path);
    goto cleanup;
  }

  ok = 1;

cleanup:
  close(fd);
  return ok;
}

int
bondage_hash_file_path(const char *path,
                       int require_exec,
                       char *out,
                       size_t outsz,
                       char *errbuf,
                       size_t errbufsz)
{
  char *resolved = NULL;
  int ok = 0;

  if (!bondage_is_absolute_path(path)) {
    bondage_set_error(errbuf, errbufsz, "path must be absolute: %s", path);
    return 0;
  }

  resolved = realpath(path, NULL);
  if (resolved == NULL) {
    bondage_set_error(errbuf, errbufsz, "realpath failed for %s: %s",
                      path, strerror(errno));
    return 0;
  }

  if (strcmp(resolved, path) != 0) {
    bondage_set_error(errbuf, errbufsz, "path mismatch: configured=%s resolved=%s",
                      path, resolved);
    goto cleanup;
  }

  if (require_exec && access(path, X_OK) != 0) {
    bondage_set_error(errbuf, errbufsz, "not executable: %s", path);
    goto cleanup;
  }

  if (!bondage_hash_open_regular_file(path, out, outsz, errbuf, errbufsz)) {
    goto cleanup;
  }

  ok = 1;

cleanup:
  free(resolved);
  return ok;
}

static void
bondage_tree_hash_update_string(CC_SHA256_CTX *ctx, const char *value)
{
  CC_SHA256_Update(ctx, value, (CC_LONG)strlen(value));
  CC_SHA256_Update(ctx, "\0", 1);
}

static int
bondage_strcmp_ptr(const void *lhs, const void *rhs)
{
  const char *const *left = lhs;
  const char *const *right = rhs;
  return strcmp(*left, *right);
}

static void
bondage_free_string_list(char **items, size_t count)
{
  size_t i;

  for (i = 0; i < count; i++) {
    free(items[i]);
  }
  free(items);
}

static int
bondage_collect_directory_names(DIR *dir,
                                char ***items_out,
                                size_t *count_out,
                                char *errbuf,
                                size_t errbufsz)
{
  struct dirent *entry;
  char **items = NULL;
  size_t count = 0;
  int ok = 0;

  errno = 0;
  while ((entry = readdir(dir)) != NULL) {
    char **grown;
    char *copy;

    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    copy = strdup(entry->d_name);
    if (copy == NULL) {
      bondage_set_error(errbuf, errbufsz, "out of memory");
      goto cleanup;
    }

    grown = realloc(items, sizeof(char *) * (count + 1));
    if (grown == NULL) {
      free(copy);
      bondage_set_error(errbuf, errbufsz, "out of memory");
      goto cleanup;
    }

    items = grown;
    items[count++] = copy;
  }

  if (errno != 0) {
    bondage_set_error(errbuf, errbufsz, "readdir failed: %s", strerror(errno));
    goto cleanup;
  }

  qsort(items, count, sizeof(char *), bondage_strcmp_ptr);
  *items_out = items;
  *count_out = count;
  ok = 1;

cleanup:
  if (!ok) bondage_free_string_list(items, count);
  return ok;
}

static int
bondage_hash_tree_walk(const char *root,
                       const char *rel_dir,
                       CC_SHA256_CTX *ctx,
                       char *errbuf,
                       size_t errbufsz)
{
  char *current_dir = NULL;
  DIR *dir = NULL;
  char **items = NULL;
  size_t item_count = 0;
  size_t i;
  int ok = 0;

  if (rel_dir == NULL) {
    current_dir = strdup(root);
  }
  else {
    current_dir = bondage_join_path(root, rel_dir);
  }
  if (current_dir == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  dir = opendir(current_dir);
  if (dir == NULL) {
    bondage_set_error(errbuf, errbufsz, "opendir failed for %s: %s",
                      current_dir, strerror(errno));
    goto cleanup;
  }

  if (!bondage_collect_directory_names(dir, &items, &item_count, errbuf, errbufsz)) {
    goto cleanup;
  }

  for (i = 0; i < item_count; i++) {
    char *relpath = NULL;
    char *fullpath = NULL;
    struct stat st;

    if (rel_dir == NULL) {
      relpath = strdup(items[i]);
    }
    else {
      relpath = bondage_join_path(rel_dir, items[i]);
    }
    fullpath = bondage_join_path(root, relpath);
    if (relpath == NULL || fullpath == NULL) {
      free(relpath);
      free(fullpath);
      bondage_set_error(errbuf, errbufsz, "out of memory");
      goto cleanup;
    }

    if (lstat(fullpath, &st) < 0) {
      bondage_set_error(errbuf, errbufsz, "lstat failed for %s: %s",
                        fullpath, strerror(errno));
      free(relpath);
      free(fullpath);
      goto cleanup;
    }

    if (S_ISLNK(st.st_mode)) {
      char link_target[PATH_MAX + 1];
      ssize_t link_len;
      char *resolved_target;

      link_len = readlink(fullpath, link_target, sizeof(link_target) - 1);
      if (link_len < 0) {
        bondage_set_error(errbuf, errbufsz, "readlink failed for %s: %s",
                          fullpath, strerror(errno));
        free(relpath);
        free(fullpath);
        goto cleanup;
      }
      link_target[link_len] = '\0';

      resolved_target = realpath(fullpath, NULL);
      if (resolved_target == NULL) {
        bondage_set_error(errbuf, errbufsz, "realpath failed for symlink %s: %s",
                          fullpath, strerror(errno));
        free(relpath);
        free(fullpath);
        goto cleanup;
      }
      if (!bondage_path_is_within_root(root, resolved_target)) {
        bondage_set_error(errbuf, errbufsz,
                          "symlink escapes package root: %s -> %s",
                          fullpath, resolved_target);
        free(resolved_target);
        free(relpath);
        free(fullpath);
        goto cleanup;
      }

      bondage_tree_hash_update_string(ctx, "symlink");
      bondage_tree_hash_update_string(ctx, relpath);
      bondage_tree_hash_update_string(ctx, link_target);
      free(resolved_target);
    }
    else if (S_ISDIR(st.st_mode)) {
      bondage_tree_hash_update_string(ctx, "dir");
      bondage_tree_hash_update_string(ctx, relpath);
      if (!bondage_hash_tree_walk(root, relpath, ctx, errbuf, errbufsz)) {
        free(relpath);
        free(fullpath);
        goto cleanup;
      }
    }
    else if (S_ISREG(st.st_mode)) {
      char file_fp[BONDAGE_SHA256_STRLEN];

      if (!bondage_hash_open_regular_file(fullpath, file_fp, sizeof(file_fp),
                                          errbuf, errbufsz)) {
        free(relpath);
        free(fullpath);
        goto cleanup;
      }

      bondage_tree_hash_update_string(ctx, "file");
      bondage_tree_hash_update_string(ctx, relpath);
      bondage_tree_hash_update_string(ctx, file_fp);
    }
    else {
      bondage_set_error(errbuf, errbufsz, "unsupported file type in package tree: %s",
                        fullpath);
      free(relpath);
      free(fullpath);
      goto cleanup;
    }

    free(relpath);
    free(fullpath);
  }

  ok = 1;

cleanup:
  bondage_free_string_list(items, item_count);
  if (dir != NULL) closedir(dir);
  free(current_dir);
  return ok;
}

int
bondage_hash_tree_path(const char *path,
                       char *out,
                       size_t outsz,
                       char *errbuf,
                       size_t errbufsz)
{
  char *resolved = NULL;
  struct stat st;
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256_CTX ctx;
  int ok = 0;

  if (!bondage_is_absolute_path(path)) {
    bondage_set_error(errbuf, errbufsz, "path must be absolute: %s", path);
    return 0;
  }

  resolved = realpath(path, NULL);
  if (resolved == NULL) {
    bondage_set_error(errbuf, errbufsz, "realpath failed for %s: %s",
                      path, strerror(errno));
    return 0;
  }

  if (strcmp(resolved, path) != 0) {
    bondage_set_error(errbuf, errbufsz, "path mismatch: configured=%s resolved=%s",
                      path, resolved);
    goto cleanup;
  }

  if (lstat(path, &st) < 0) {
    bondage_set_error(errbuf, errbufsz, "lstat failed for %s: %s",
                      path, strerror(errno));
    goto cleanup;
  }
  if (!S_ISDIR(st.st_mode)) {
    bondage_set_error(errbuf, errbufsz, "not a directory: %s", path);
    goto cleanup;
  }

  CC_SHA256_Init(&ctx);
  if (!bondage_hash_tree_walk(path, NULL, &ctx, errbuf, errbufsz)) {
    goto cleanup;
  }
  CC_SHA256_Final(digest, &ctx);

  if (!bondage_sha256_string(digest, out, outsz)) {
    bondage_set_error(errbuf, errbufsz, "output buffer too small");
    goto cleanup;
  }

  ok = 1;

cleanup:
  free(resolved);
  return ok;
}

static int
bondage_verify_file(const char *label,
                    const char *path,
                    const char *expected_fp,
                    int require_exec,
                    FILE *stream,
                    char *errbuf,
                    size_t errbufsz)
{
  char actual_fp[BONDAGE_SHA256_STRLEN];

  if (!bondage_hash_file_path(path, require_exec, actual_fp, sizeof(actual_fp),
                              errbuf, errbufsz)) {
    bondage_set_error(errbuf, errbufsz, "%s %s", label, errbuf);
    return 0;
  }

  if (strcmp(expected_fp, actual_fp) != 0) {
    bondage_set_error(errbuf, errbufsz,
                      "%s fingerprint mismatch: expected=%s actual=%s",
                      label, expected_fp, actual_fp);
    return 0;
  }

  if (stream != NULL) {
    fprintf(stream, "ok: %s %s\n", label, path);
  }
  return 1;
}

static int
bondage_verify_tree(const char *label,
                    const char *path,
                    const char *expected_fp,
                    FILE *stream,
                    char *errbuf,
                    size_t errbufsz)
{
  char actual_fp[BONDAGE_SHA256_STRLEN];

  if (!bondage_hash_tree_path(path, actual_fp, sizeof(actual_fp), errbuf, errbufsz)) {
    bondage_set_error(errbuf, errbufsz, "%s %s", label, errbuf);
    return 0;
  }

  if (strcmp(expected_fp, actual_fp) != 0) {
    bondage_set_error(errbuf, errbufsz,
                      "%s fingerprint mismatch: expected=%s actual=%s",
                      label, expected_fp, actual_fp);
    return 0;
  }

  if (stream != NULL) {
    fprintf(stream, "ok: %s %s\n", label, path);
  }
  return 1;
}

int
bondage_verify_profile(const struct bondage_config *config,
                       const struct bondage_profile *profile,
                       FILE *stream,
                       char *errbuf,
                       size_t errbufsz)
{
  if (profile->use_envchain) {
    if (!bondage_verify_file("envchain", config->global.envchain,
                             config->global.envchain_fp, 1,
                             stream, errbuf, errbufsz)) {
      return 0;
    }
  }

  if (profile->use_nono) {
    if (!bondage_verify_file("nono", config->global.nono,
                             config->global.nono_fp, 1,
                             stream, errbuf, errbufsz)) {
      return 0;
    }
  }

  if (strcmp(profile->touch_policy, "prompt") == 0) {
    if (!bondage_verify_file("touchid", config->global.touchid,
                             config->global.touchid_fp, 1,
                             stream, errbuf, errbufsz)) {
      return 0;
    }
  }

  if (strcmp(profile->target_kind, "native") == 0) {
    if (!bondage_verify_file("target", profile->target, profile->target_fp, 1,
                             stream, errbuf, errbufsz)) {
      return 0;
    }
  }
  else if (strcmp(profile->target_kind, "script") == 0) {
    if (!bondage_verify_file("target", profile->target, profile->target_fp, 0,
                             stream, errbuf, errbufsz)) {
      return 0;
    }
    if (!bondage_verify_file("interpreter", profile->interpreter,
                             profile->interpreter_fp, 1,
                             stream, errbuf, errbufsz)) {
      return 0;
    }
    if (profile->package_root != NULL && profile->package_tree_fp != NULL) {
      if (!bondage_verify_tree("package_root", profile->package_root,
                               profile->package_tree_fp,
                               stream, errbuf, errbufsz)) {
        return 0;
      }
    }
  }
  else {
    bondage_set_error(errbuf, errbufsz, "unknown target_kind '%s'",
                      profile->target_kind);
    return 0;
  }

  return 1;
}

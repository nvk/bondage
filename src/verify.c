#include <CommonCrypto/CommonDigest.h>

#include <ctype.h>
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

static char *
bondage_xstrndup(const char *value, size_t len)
{
  char *copy = malloc(len + 1);
  if (copy == NULL) return NULL;
  memcpy(copy, value, len);
  copy[len] = '\0';
  return copy;
}

static int
bondage_version_cmp(const char *a, const char *b)
{
  while (*a != '\0' || *b != '\0') {
    if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
      unsigned long na = 0;
      unsigned long nb = 0;

      while (*a == '0') a++;
      while (*b == '0') b++;

      while (isdigit((unsigned char)*a)) {
        na = (na * 10UL) + (unsigned long)(*a - '0');
        a++;
      }
      while (isdigit((unsigned char)*b)) {
        nb = (nb * 10UL) + (unsigned long)(*b - '0');
        b++;
      }

      if (na < nb) return -1;
      if (na > nb) return 1;
      continue;
    }

    if (*a == *b) {
      if (*a == '\0') return 0;
      a++;
      b++;
      continue;
    }

    if (*a == '\0') return -1;
    if (*b == '\0') return 1;
    return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
  }

  return 0;
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
bondage_parse_brew_path(const char *path,
                        char **formula_out,
                        char **version_out,
                        const char **suffix_out)
{
  const char *marker = strstr(path, "/Caskroom/");
  size_t marker_len = strlen("/Caskroom/");
  const char *cursor;
  const char *formula_end;
  const char *version_end;

  if (marker == NULL) {
    marker = strstr(path, "/Cellar/");
    marker_len = strlen("/Cellar/");
  }
  if (marker == NULL) return 0;

  cursor = marker + marker_len;
  formula_end = strchr(cursor, '/');
  if (formula_end == NULL) return 0;

  version_end = strchr(formula_end + 1, '/');
  if (version_end == NULL) version_end = path + strlen(path);

  *formula_out = bondage_xstrndup(cursor, (size_t)(formula_end - cursor));
  *version_out = bondage_xstrndup(formula_end + 1,
                                  (size_t)(version_end - (formula_end + 1)));
  if (*formula_out == NULL || *version_out == NULL) {
    free(*formula_out);
    free(*version_out);
    *formula_out = NULL;
    *version_out = NULL;
    return 0;
  }
  *suffix_out = version_end;
  return 1;
}

static int
bondage_find_brew_replacement(const char *path, char **replacement_out)
{
  const char *marker = strstr(path, "/Caskroom/");
  size_t marker_len = strlen("/Caskroom/");
  const char *cursor;
  const char *formula_end;
  const char *version_end;
  char *root = NULL;
  DIR *dir = NULL;
  struct dirent *entry;
  char *best_version = NULL;
  char *best_path = NULL;
  int ok = 0;

  if (marker == NULL) {
    marker = strstr(path, "/Cellar/");
    marker_len = strlen("/Cellar/");
  }
  if (marker == NULL) return 0;

  cursor = marker + marker_len;
  formula_end = strchr(cursor, '/');
  if (formula_end == NULL) return 0;

  version_end = strchr(formula_end + 1, '/');
  if (version_end == NULL) version_end = path + strlen(path);

  root = bondage_xstrndup(path, (size_t)(formula_end - path));
  if (root == NULL) goto cleanup;

  dir = opendir(root);
  if (dir == NULL) goto cleanup;

  while ((entry = readdir(dir)) != NULL) {
    char *candidate = NULL;

    if (entry->d_name[0] == '.') continue;
    candidate = bondage_join_path(root, entry->d_name);
    if (candidate == NULL) goto cleanup;

    if (version_end[0] != '\0') {
      char *grown;
      size_t candidate_len = strlen(candidate);
      size_t rest_len = strlen(version_end);

      grown = realloc(candidate, candidate_len + rest_len + 1);
      if (grown == NULL) {
        free(candidate);
        goto cleanup;
      }
      candidate = grown;
      memcpy(candidate + candidate_len, version_end, rest_len + 1);
    }

    if (access(candidate, F_OK) == 0) {
      if (best_version == NULL ||
          bondage_version_cmp(entry->d_name, best_version) > 0) {
        free(best_version);
        free(best_path);
        best_version = strdup(entry->d_name);
        best_path = candidate;
        if (best_version == NULL) goto cleanup;
        continue;
      }
    }

    free(candidate);
  }

  if (best_path != NULL) {
    *replacement_out = best_path;
    best_path = NULL;
    ok = 1;
  }

cleanup:
  if (dir != NULL) closedir(dir);
  free(root);
  free(best_version);
  free(best_path);
  return ok;
}

static const char *
bondage_configured_path_for_label(const struct bondage_config *config,
                                  const struct bondage_profile *profile,
                                  const char *label,
                                  int *is_global_out)
{
  *is_global_out = 0;

  if (strcmp(label, "envchain") == 0) {
    *is_global_out = 1;
    return config->global.envchain;
  }
  if (strcmp(label, "nono") == 0) {
    *is_global_out = 1;
    return config->global.nono;
  }
  if (strcmp(label, "touchid") == 0) {
    *is_global_out = 1;
    return config->global.touchid;
  }
  if (strcmp(label, "target") == 0) return profile->target;
  if (strcmp(label, "interpreter") == 0) return profile->interpreter;
  if (strcmp(label, "package_root") == 0) return profile->package_root;
  return NULL;
}

static int
bondage_extract_error_label(const char *base_error, char *label, size_t labelsz)
{
  const char *space = strchr(base_error, ' ');
  size_t len;

  if (space == NULL || space == base_error) return 0;
  len = (size_t)(space - base_error);
  if (len + 1 > labelsz) return 0;
  memcpy(label, base_error, len);
  label[len] = '\0';
  return 1;
}

static char *
bondage_extract_resolved_path_from_error(const char *base_error)
{
  const char *marker = strstr(base_error, "resolved=");
  const char *end;

  if (marker == NULL) return NULL;
  marker += strlen("resolved=");
  end = marker;
  while (*end != '\0' && !isspace((unsigned char)*end)) end++;
  return bondage_xstrndup(marker, (size_t)(end - marker));
}

static void
bondage_describe_brew_version_change(const char *configured_path,
                                     const char *current_path,
                                     char *buf,
                                     size_t bufsz)
{
  char *configured_formula = NULL;
  char *configured_version = NULL;
  char *current_formula = NULL;
  char *current_version = NULL;
  const char *configured_suffix = NULL;
  const char *current_suffix = NULL;
  int cmp;

  if (!bondage_parse_brew_path(configured_path, &configured_formula,
                               &configured_version, &configured_suffix) ||
      !bondage_parse_brew_path(current_path, &current_formula,
                               &current_version, &current_suffix)) {
    goto cleanup;
  }

  if (strcmp(configured_formula, current_formula) != 0) goto cleanup;
  if (strcmp(configured_version, current_version) == 0) goto cleanup;
  if (strcmp(configured_suffix, current_suffix) != 0) goto cleanup;

  cmp = bondage_version_cmp(configured_version, current_version);
  if (cmp < 0) {
    snprintf(buf, bufsz, "detected Homebrew upgrade for %s: %s -> %s",
             configured_formula, configured_version, current_version);
  }
  else if (cmp > 0) {
    snprintf(buf, bufsz, "detected Homebrew downgrade for %s: %s -> %s",
             configured_formula, configured_version, current_version);
  }

cleanup:
  free(configured_formula);
  free(configured_version);
  free(current_formula);
  free(current_version);
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
  char detail[256];

  if (!bondage_hash_file_path(path, require_exec, actual_fp, sizeof(actual_fp),
                              detail, sizeof(detail))) {
    bondage_set_error(errbuf, errbufsz, "%s %s", label, detail);
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
  char detail[256];

  if (!bondage_hash_tree_path(path, actual_fp, sizeof(actual_fp),
                              detail, sizeof(detail))) {
    bondage_set_error(errbuf, errbufsz, "%s %s", label, detail);
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

void
bondage_format_verify_failure(const struct bondage_config *config,
                              const struct bondage_profile *profile,
                              const char *config_path,
                              const char *base_error,
                              char *out,
                              size_t outsz)
{
  char label[64];
  char version_note[256];
  char hint[512];
  char *resolved = NULL;
  char *replacement = NULL;
  const char *configured_path;
  const char *current_path = NULL;
  int is_global = 0;

  if (out == NULL || outsz == 0) return;

  snprintf(out, outsz, "%s", base_error);
  if (!bondage_extract_error_label(base_error, label, sizeof(label))) return;

  configured_path = bondage_configured_path_for_label(config, profile, label, &is_global);
  if (configured_path == NULL) return;

  resolved = bondage_extract_resolved_path_from_error(base_error);
  if (resolved != NULL) {
    current_path = resolved;
  }
  else if (bondage_find_brew_replacement(configured_path, &replacement)) {
    current_path = replacement;
  }
  else if (access(configured_path, F_OK) == 0) {
    current_path = configured_path;
  }

  version_note[0] = '\0';
  hint[0] = '\0';

  if (current_path != NULL && configured_path != NULL) {
    bondage_describe_brew_version_change(configured_path, current_path,
                                         version_note, sizeof(version_note));
  }

  if (is_global) {
    snprintf(hint, sizeof(hint),
             "Run: bondage repin-globals %s",
             config_path);
  }
  else {
    snprintf(hint, sizeof(hint),
             "Run: bondage repin %s %s",
             profile->name, config_path);
  }

  if (version_note[0] != '\0') {
    snprintf(out, outsz, "%s. %s. %s",
             base_error, version_note, hint);
  }
  else if (current_path != NULL && current_path != configured_path &&
           strstr(base_error, "path mismatch: configured=") != NULL) {
    snprintf(out, outsz, "%s. Current installed path: %s. %s",
             base_error, current_path, hint);
  }
  else if (strstr(base_error, "fingerprint mismatch:") != NULL) {
    snprintf(out, outsz,
             "%s. If you intentionally upgraded, downgraded, or reinstalled this artifact, %s",
             base_error, hint);
  }
  else if (strstr(base_error, "realpath failed for") != NULL) {
    snprintf(out, outsz,
             "%s. The pinned path may have moved. %s",
             base_error, hint);
  }

  free(resolved);
  free(replacement);
}

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "repin.h"
#include "verify.h"

enum bondage_repin_section {
  BONDAGE_REPIN_SECTION_NONE = 0,
  BONDAGE_REPIN_SECTION_GLOBAL,
  BONDAGE_REPIN_SECTION_DEFAULTS,
  BONDAGE_REPIN_SECTION_PROFILE
};

struct bondage_named_list {
  char **items;
  size_t count;
};

struct bondage_value_update {
  int active;
  char *value;
};

struct bondage_profile_field_update {
  int active;
  char *old_path;
  char *new_path;
  char *new_fp;
  struct bondage_field_owner path_owner;
  struct bondage_field_owner fp_owner;
  struct bondage_named_list path_profiles;
  struct bondage_named_list fp_profiles;
};

struct bondage_repin_plan {
  char *selected_profile;
  struct bondage_value_update global_nono;
  struct bondage_value_update global_nono_fp;
  struct bondage_value_update global_envchain;
  struct bondage_value_update global_envchain_fp;
  struct bondage_value_update global_touchid;
  struct bondage_value_update global_touchid_fp;
  struct bondage_profile_field_update target;
  struct bondage_profile_field_update interpreter;
  struct bondage_profile_field_update package_root;
};

static void
bondage_set_error(char *errbuf, size_t errbufsz, const char *fmt, ...)
{
  va_list ap;

  if (errbuf == NULL || errbufsz == 0) return;

  va_start(ap, fmt);
  vsnprintf(errbuf, errbufsz, fmt, ap);
  va_end(ap);
}

static char *
bondage_xstrdup(const char *value)
{
  size_t len;
  char *copy;

  if (value == NULL) return NULL;
  len = strlen(value);
  copy = malloc(len + 1);
  if (copy == NULL) return NULL;
  memcpy(copy, value, len + 1);
  return copy;
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

static void
bondage_named_list_free(struct bondage_named_list *list)
{
  size_t i;

  for (i = 0; i < list->count; i++) {
    free(list->items[i]);
  }
  free(list->items);
  list->items = NULL;
  list->count = 0;
}

static int
bondage_named_list_append(struct bondage_named_list *list, const char *value,
                          char *errbuf, size_t errbufsz)
{
  char **grown;
  char *copy = bondage_xstrdup(value);

  if (copy == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  grown = realloc(list->items, sizeof(char *) * (list->count + 1));
  if (grown == NULL) {
    free(copy);
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  list->items = grown;
  list->items[list->count++] = copy;
  return 1;
}

static int
bondage_named_list_contains(const struct bondage_named_list *list, const char *value)
{
  size_t i;

  for (i = 0; i < list->count; i++) {
    if (strcmp(list->items[i], value) == 0) return 1;
  }

  return 0;
}

static void
bondage_free_field_owner(struct bondage_field_owner *owner)
{
  free(owner->name);
  owner->name = NULL;
  owner->kind = BONDAGE_OWNER_NONE;
}

static int
bondage_copy_field_owner(struct bondage_field_owner *dst,
                         const struct bondage_field_owner *src,
                         char *errbuf,
                         size_t errbufsz)
{
  char *copy = NULL;

  if (src->name != NULL) {
    copy = bondage_xstrdup(src->name);
    if (copy == NULL) {
      bondage_set_error(errbuf, errbufsz, "out of memory");
      return 0;
    }
  }

  bondage_free_field_owner(dst);
  dst->kind = src->kind;
  dst->name = copy;
  return 1;
}

static int
bondage_field_owner_matches(const struct bondage_field_owner *owner,
                            enum bondage_owner_kind kind,
                            const char *name)
{
  if (owner->kind != kind) return 0;
  if (kind == BONDAGE_OWNER_PROFILE) return 1;
  if (owner->name == NULL || name == NULL) return 0;
  return strcmp(owner->name, name) == 0;
}

static const char *
bondage_owner_label(const struct bondage_field_owner *owner)
{
  if (owner->kind == BONDAGE_OWNER_DEFAULT) return "defaults";
  if (owner->kind == BONDAGE_OWNER_PROFILE) return "profile";
  return "unknown";
}

static void
bondage_profile_field_update_free(struct bondage_profile_field_update *update)
{
  free(update->old_path);
  free(update->new_path);
  free(update->new_fp);
  bondage_free_field_owner(&update->path_owner);
  bondage_free_field_owner(&update->fp_owner);
  bondage_named_list_free(&update->path_profiles);
  bondage_named_list_free(&update->fp_profiles);
  memset(update, 0, sizeof(*update));
}

static void
bondage_repin_plan_free(struct bondage_repin_plan *plan)
{
  free(plan->selected_profile);
  free(plan->global_nono.value);
  free(plan->global_nono_fp.value);
  free(plan->global_envchain.value);
  free(plan->global_envchain_fp.value);
  free(plan->global_touchid.value);
  free(plan->global_touchid_fp.value);
  bondage_profile_field_update_free(&plan->target);
  bondage_profile_field_update_free(&plan->interpreter);
  bondage_profile_field_update_free(&plan->package_root);
  memset(plan, 0, sizeof(*plan));
}

static char *
bondage_trim(char *value)
{
  char *end;

  while (*value != '\0' && isspace((unsigned char)*value)) value++;
  if (*value == '\0') return value;

  end = value + strlen(value) - 1;
  while (end > value && isspace((unsigned char)*end)) {
    *end = '\0';
    end--;
  }

  return value;
}

static int
bondage_parse_defaults_section(char *namebuf, char **defaults_name)
{
  const char prefix[] = "defaults \"";
  size_t len = strlen(namebuf);

  if (strncmp(namebuf, prefix, sizeof(prefix) - 1) != 0) return 0;
  if (len < sizeof(prefix) || namebuf[len - 1] != '"') return 0;

  namebuf[len - 1] = '\0';
  *defaults_name = namebuf + (sizeof(prefix) - 1);
  return **defaults_name != '\0';
}

static int
bondage_parse_profile_section(char *namebuf, char **profile_name)
{
  const char prefix[] = "profile \"";
  size_t len = strlen(namebuf);

  if (strncmp(namebuf, prefix, sizeof(prefix) - 1) != 0) return 0;
  if (len < sizeof(prefix) || namebuf[len - 1] != '"') return 0;

  namebuf[len - 1] = '\0';
  *profile_name = namebuf + (sizeof(prefix) - 1);
  return **profile_name != '\0';
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
bondage_join_path2(const char *left, const char *right, char **out,
                   char *errbuf, size_t errbufsz)
{
  size_t left_len = strlen(left);
  size_t right_len = strlen(right);
  int needs_slash = left_len > 0 && left[left_len - 1] != '/';
  char *value = malloc(left_len + right_len + (size_t)needs_slash + 1);

  if (value == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  memcpy(value, left, left_len);
  if (needs_slash) value[left_len++] = '/';
  memcpy(value + left_len, right, right_len);
  value[left_len + right_len] = '\0';
  *out = value;
  return 1;
}

static int
bondage_resolve_brew_version_move(const char *path, char **resolved,
                                  char *errbuf, size_t errbufsz)
{
  const char *marker = strstr(path, "/Caskroom/");
  size_t marker_len = strlen("/Caskroom/");
  const char *cursor;
  const char *formula_end;
  const char *version_end;
  char *root = NULL;
  char *best_version = NULL;
  char *best_path = NULL;
  DIR *dir = NULL;
  struct dirent *entry;

  if (marker == NULL) {
    marker = strstr(path, "/Cellar/");
    marker_len = strlen("/Cellar/");
  }
  if (marker == NULL) {
    bondage_set_error(errbuf, errbufsz,
                      "cannot resolve moved path outside Homebrew tree: %s", path);
    return 0;
  }

  cursor = marker + marker_len;
  formula_end = strchr(cursor, '/');
  if (formula_end == NULL) {
    bondage_set_error(errbuf, errbufsz,
                      "cannot parse Homebrew formula path: %s", path);
    return 0;
  }

  version_end = strchr(formula_end + 1, '/');
  if (version_end == NULL) version_end = path + strlen(path);

  root = bondage_xstrndup(path, (size_t)(formula_end - path));
  if (root == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  dir = opendir(root);
  if (dir == NULL) {
    bondage_set_error(errbuf, errbufsz, "cannot open %s: %s", root, strerror(errno));
    free(root);
    return 0;
  }

  while ((entry = readdir(dir)) != NULL) {
    char *candidate = NULL;

    if (entry->d_name[0] == '.') continue;
    if (!bondage_join_path2(root, entry->d_name, &candidate, errbuf, errbufsz)) {
      closedir(dir);
      free(root);
      free(best_version);
      free(best_path);
      return 0;
    }

    if (version_end[0] != '\0') {
      char *grown = NULL;
      size_t candidate_len = strlen(candidate);
      size_t rest_len = strlen(version_end);

      grown = realloc(candidate, candidate_len + rest_len + 1);
      if (grown == NULL) {
        free(candidate);
        closedir(dir);
        free(root);
        free(best_version);
        free(best_path);
        bondage_set_error(errbuf, errbufsz, "out of memory");
        return 0;
      }
      candidate = grown;
      memcpy(candidate + candidate_len, version_end, rest_len + 1);
    }

    if (access(candidate, F_OK) == 0) {
      if (best_version == NULL ||
          bondage_version_cmp(entry->d_name, best_version) > 0) {
        free(best_version);
        free(best_path);
        best_version = bondage_xstrdup(entry->d_name);
        best_path = candidate;
        if (best_version == NULL) {
          free(best_path);
          closedir(dir);
          free(root);
          bondage_set_error(errbuf, errbufsz, "out of memory");
          return 0;
        }
        continue;
      }
    }

    free(candidate);
  }

  closedir(dir);
  free(root);

  if (best_path == NULL) {
    bondage_set_error(errbuf, errbufsz,
                      "cannot resolve replacement path for %s", path);
    free(best_version);
    return 0;
  }

  *resolved = best_path;
  free(best_version);
  return 1;
}

static int
bondage_resolve_path(const char *path, char **resolved, char *errbuf, size_t errbufsz)
{
  char *canonical;

  if (access(path, F_OK) == 0) {
    canonical = realpath(path, NULL);
    if (canonical == NULL) {
      bondage_set_error(errbuf, errbufsz, "realpath failed for %s: %s",
                        path, strerror(errno));
      return 0;
    }
    *resolved = canonical;
    return 1;
  }

  return bondage_resolve_brew_version_move(path, resolved, errbuf, errbufsz);
}

static int
bondage_hash_file_string(const char *path, int require_exec, char **out,
                         char *errbuf, size_t errbufsz)
{
  char fp[256];

  if (!bondage_hash_file_path(path, require_exec, fp, sizeof(fp), errbuf, errbufsz)) {
    return 0;
  }

  *out = bondage_xstrdup(fp);
  if (*out == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }
  return 1;
}

static int
bondage_hash_tree_string(const char *path, char **out, char *errbuf, size_t errbufsz)
{
  char fp[256];

  if (!bondage_hash_tree_path(path, fp, sizeof(fp), errbuf, errbufsz)) {
    return 0;
  }

  *out = bondage_xstrdup(fp);
  if (*out == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }
  return 1;
}

static int
bondage_prepare_global_update(struct bondage_value_update *path_update,
                              struct bondage_value_update *fp_update,
                              const char *path,
                              int require_exec,
                              char *errbuf, size_t errbufsz)
{
  char *resolved = NULL;
  char *fp = NULL;

  if (path == NULL) return 1;

  if (!bondage_resolve_path(path, &resolved, errbuf, errbufsz)) return 0;
  if (!bondage_hash_file_string(resolved, require_exec, &fp, errbuf, errbufsz)) {
    free(resolved);
    return 0;
  }

  path_update->active = 1;
  path_update->value = resolved;
  fp_update->active = 1;
  fp_update->value = fp;
  return 1;
}

static int
bondage_collect_matching_profiles(struct bondage_named_list *out,
                                  const struct bondage_config *config,
                                  const char *path_value,
                                  int which,
                                  int owner_is_fp,
                                  const struct bondage_field_owner *owner,
                                  char *errbuf,
                                  size_t errbufsz)
{
  size_t i;

  if (path_value == NULL || owner->kind == BONDAGE_OWNER_NONE) return 1;

  for (i = 0; i < config->profile_count; i++) {
    const struct bondage_profile *profile = &config->profiles[i];
    const char *candidate = NULL;
    const struct bondage_field_owner *candidate_owner = NULL;

    if (which == 0) {
      candidate = profile->target;
      candidate_owner = owner_is_fp ? &profile->target_fp_owner
                                    : &profile->target_owner;
    }
    else if (which == 1) {
      candidate = profile->interpreter;
      candidate_owner = owner_is_fp ? &profile->interpreter_fp_owner
                                    : &profile->interpreter_owner;
    }
    else {
      candidate = profile->package_root;
      candidate_owner = owner_is_fp ? &profile->package_tree_fp_owner
                                    : &profile->package_root_owner;
    }

    if (candidate != NULL && strcmp(candidate, path_value) == 0 &&
        bondage_field_owner_matches(candidate_owner, owner->kind, owner->name)) {
      if (!bondage_named_list_append(out, profile->name, errbuf, errbufsz)) {
        return 0;
      }
    }
  }

  return 1;
}

static int
bondage_prepare_profile_update(struct bondage_profile_field_update *update,
                               const struct bondage_config *config,
                               const struct bondage_profile *selected,
                               int which,
                               char *errbuf,
                               size_t errbufsz)
{
  const char *source_path = NULL;
  const struct bondage_field_owner *path_owner = NULL;
  const struct bondage_field_owner *fp_owner = NULL;
  int require_exec = 0;

  if (which == 0) {
    source_path = selected->target;
    path_owner = &selected->target_owner;
    fp_owner = &selected->target_fp_owner;
    require_exec = strcmp(selected->target_kind, "native") == 0;
  }
  else if (which == 1) {
    source_path = selected->interpreter;
    path_owner = &selected->interpreter_owner;
    fp_owner = &selected->interpreter_fp_owner;
    require_exec = 1;
  }
  else {
    source_path = selected->package_root;
    path_owner = &selected->package_root_owner;
    fp_owner = &selected->package_tree_fp_owner;
  }

  if (source_path == NULL) return 1;

  update->active = 1;
  update->old_path = bondage_xstrdup(source_path);
  if (update->old_path == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  if (!bondage_copy_field_owner(&update->path_owner, path_owner,
                                errbuf, errbufsz)) {
    return 0;
  }
  if (!bondage_copy_field_owner(&update->fp_owner, fp_owner,
                                errbuf, errbufsz)) {
    return 0;
  }

  if (!bondage_collect_matching_profiles(&update->path_profiles, config,
                                         source_path, which, 0,
                                         &update->path_owner,
                                         errbuf, errbufsz)) return 0;
  if (!bondage_collect_matching_profiles(&update->fp_profiles, config,
                                         source_path, which, 1,
                                         &update->fp_owner,
                                         errbuf, errbufsz)) return 0;

  if (!bondage_resolve_path(source_path, &update->new_path, errbuf, errbufsz)) {
    return 0;
  }

  if (which == 2) {
    if (!bondage_hash_tree_string(update->new_path, &update->new_fp, errbuf, errbufsz)) {
      return 0;
    }
  }
  else {
    if (!bondage_hash_file_string(update->new_path, require_exec, &update->new_fp,
                                  errbuf, errbufsz)) {
      return 0;
    }
  }

  return 1;
}

static int
bondage_build_repin_plan(const struct bondage_config *config,
                         const struct bondage_profile *profile,
                         struct bondage_repin_plan *plan,
                         char *errbuf, size_t errbufsz)
{
  plan->selected_profile = bondage_xstrdup(profile->name);
  if (plan->selected_profile == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  if (profile->use_nono &&
      !bondage_prepare_global_update(&plan->global_nono, &plan->global_nono_fp,
                                     config->global.nono, 1, errbuf, errbufsz)) {
    return 0;
  }

  if (profile->use_envchain &&
      !bondage_prepare_global_update(&plan->global_envchain,
                                     &plan->global_envchain_fp,
                                     config->global.envchain, 1,
                                     errbuf, errbufsz)) {
    return 0;
  }

  if (strcmp(profile->touch_policy, "prompt") == 0 &&
      !bondage_prepare_global_update(&plan->global_touchid,
                                     &plan->global_touchid_fp,
                                     config->global.touchid, 1,
                                     errbuf, errbufsz)) {
    return 0;
  }

  if (!bondage_prepare_profile_update(&plan->target, config, profile, 0,
                                      errbuf, errbufsz)) {
    return 0;
  }
  if (!bondage_prepare_profile_update(&plan->interpreter, config, profile, 1,
                                      errbuf, errbufsz)) {
    return 0;
  }
  if (!bondage_prepare_profile_update(&plan->package_root, config, profile, 2,
                                      errbuf, errbufsz)) {
    return 0;
  }

  return 1;
}

static int
bondage_build_global_plan(const struct bondage_config *config,
                          struct bondage_repin_plan *plan,
                          char *errbuf, size_t errbufsz)
{
  if (!bondage_prepare_global_update(&plan->global_nono, &plan->global_nono_fp,
                                     config->global.nono, 1, errbuf, errbufsz)) {
    return 0;
  }

  if (!bondage_prepare_global_update(&plan->global_envchain,
                                     &plan->global_envchain_fp,
                                     config->global.envchain, 1,
                                     errbuf, errbufsz)) {
    return 0;
  }

  if (!bondage_prepare_global_update(&plan->global_touchid,
                                     &plan->global_touchid_fp,
                                     config->global.touchid, 1,
                                     errbuf, errbufsz)) {
    return 0;
  }

  return 1;
}

static int
bondage_value_update_needed(const struct bondage_value_update *update,
                            const char *configured)
{
  if (!update->active) return 0;
  if (configured == NULL) return 1;
  return strcmp(configured, update->value) != 0;
}

static int
bondage_profile_field_update_needed(const struct bondage_profile_field_update *update,
                                    const char *configured_path,
                                    const char *configured_fp)
{
  if (!update->active) return 0;
  if (configured_path == NULL || configured_fp == NULL) return 1;
  if (strcmp(configured_path, update->new_path) != 0) return 1;
  return strcmp(configured_fp, update->new_fp) != 0;
}

static const char *
bondage_profile_field_replacement(const struct bondage_repin_plan *plan,
                                  enum bondage_repin_section section,
                                  const char *section_name,
                                  const char *key)
{
  const struct bondage_profile_field_update *update = NULL;
  const struct bondage_field_owner *owner = NULL;
  const struct bondage_named_list *profiles = NULL;
  int path_key = 0;

  if (strcmp(key, "target") == 0 || strcmp(key, "target_fp") == 0) {
    update = &plan->target;
  }
  else if (strcmp(key, "interpreter") == 0 || strcmp(key, "interpreter_fp") == 0) {
    update = &plan->interpreter;
  }
  else if (strcmp(key, "package_root") == 0 || strcmp(key, "package_tree_fp") == 0) {
    update = &plan->package_root;
  }
  else {
    return NULL;
  }

  if (!update->active) {
    return NULL;
  }

  if (strcmp(key, "target") == 0 || strcmp(key, "interpreter") == 0 ||
      strcmp(key, "package_root") == 0) {
    owner = &update->path_owner;
    profiles = &update->path_profiles;
    path_key = 1;
  }
  else {
    owner = &update->fp_owner;
    profiles = &update->fp_profiles;
  }

  if (section == BONDAGE_REPIN_SECTION_DEFAULTS) {
    if (owner->kind != BONDAGE_OWNER_DEFAULT || owner->name == NULL ||
        section_name == NULL || strcmp(owner->name, section_name) != 0) {
      return NULL;
    }
  }
  else if (section == BONDAGE_REPIN_SECTION_PROFILE) {
    if (owner->kind != BONDAGE_OWNER_PROFILE ||
        !bondage_named_list_contains(profiles, section_name)) {
      return NULL;
    }
  }
  else {
    return NULL;
  }

  if (path_key) {
    return update->new_path;
  }

  return update->new_fp;
}

static const char *
bondage_global_replacement(const struct bondage_repin_plan *plan, const char *key)
{
  if (strcmp(key, "nono") == 0 && plan->global_nono.active) {
    return plan->global_nono.value;
  }
  if (strcmp(key, "nono_fp") == 0 && plan->global_nono_fp.active) {
    return plan->global_nono_fp.value;
  }
  if (strcmp(key, "envchain") == 0 && plan->global_envchain.active) {
    return plan->global_envchain.value;
  }
  if (strcmp(key, "envchain_fp") == 0 && plan->global_envchain_fp.active) {
    return plan->global_envchain_fp.value;
  }
  if (strcmp(key, "touchid") == 0 && plan->global_touchid.active) {
    return plan->global_touchid.value;
  }
  if (strcmp(key, "touchid_fp") == 0 && plan->global_touchid_fp.active) {
    return plan->global_touchid_fp.value;
  }

  return NULL;
}

static int
bondage_write_line(FILE *out, const char *key, const char *value,
                   char *errbuf, size_t errbufsz)
{
  if (fprintf(out, "%s = %s\n", key, value) < 0) {
    bondage_set_error(errbuf, errbufsz, "write failed: %s", strerror(errno));
    return 0;
  }
  return 1;
}

static int
bondage_rewrite_config(const char *config_path, const struct bondage_repin_plan *plan,
                       char *errbuf, size_t errbufsz)
{
  FILE *in = NULL;
  FILE *out = NULL;
  char *tmp_path = NULL;
  char current_section_name[512];
  char line[4096];
  enum bondage_repin_section section = BONDAGE_REPIN_SECTION_NONE;
  int fd = -1;
  struct stat st;
  int ok = 0;

  current_section_name[0] = '\0';

  in = fopen(config_path, "r");
  if (in == NULL) {
    bondage_set_error(errbuf, errbufsz, "cannot open %s: %s",
                      config_path, strerror(errno));
    goto cleanup;
  }

  if (stat(config_path, &st) != 0) {
    bondage_set_error(errbuf, errbufsz, "cannot stat %s: %s",
                      config_path, strerror(errno));
    goto cleanup;
  }

  tmp_path = malloc(strlen(config_path) + 16);
  if (tmp_path == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    goto cleanup;
  }
  sprintf(tmp_path, "%s.tmpXXXXXX", config_path);

  fd = mkstemp(tmp_path);
  if (fd < 0) {
    bondage_set_error(errbuf, errbufsz, "mkstemp failed for %s: %s",
                      tmp_path, strerror(errno));
    goto cleanup;
  }

  if (fchmod(fd, st.st_mode & 0777) != 0) {
    bondage_set_error(errbuf, errbufsz, "cannot chmod temp file: %s",
                      strerror(errno));
    goto cleanup;
  }

  out = fdopen(fd, "w");
  if (out == NULL) {
    bondage_set_error(errbuf, errbufsz, "fdopen failed: %s", strerror(errno));
    goto cleanup;
  }
  fd = -1;

  while (fgets(line, sizeof(line), in) != NULL) {
    char section_copy[4096];
    char line_copy[4096];
    char *trimmed;
    char *eq;
    char *key;
    const char *replacement = NULL;

    memcpy(section_copy, line, strlen(line) + 1);
    trimmed = bondage_trim(section_copy);

    if (*trimmed == '[') {
      size_t len = strlen(trimmed);

      current_section_name[0] = '\0';
      if (len >= 3 && trimmed[len - 1] == ']') {
        char *namebuf;
        char *defaults_name = NULL;
        char *profile_name = NULL;

        trimmed[len - 1] = '\0';
        namebuf = bondage_trim(trimmed + 1);
        if (strcmp(namebuf, "global") == 0) {
          section = BONDAGE_REPIN_SECTION_GLOBAL;
        }
        else if (bondage_parse_defaults_section(namebuf, &defaults_name)) {
          section = BONDAGE_REPIN_SECTION_DEFAULTS;
          snprintf(current_section_name, sizeof(current_section_name),
                   "%s", defaults_name);
        }
        else if (bondage_parse_profile_section(namebuf, &profile_name)) {
          section = BONDAGE_REPIN_SECTION_PROFILE;
          snprintf(current_section_name, sizeof(current_section_name),
                   "%s", profile_name);
        }
        else {
          section = BONDAGE_REPIN_SECTION_NONE;
        }
      }

      if (fputs(line, out) == EOF) {
        bondage_set_error(errbuf, errbufsz, "write failed: %s", strerror(errno));
        goto cleanup;
      }
      continue;
    }

    if (*trimmed == '\0' || *trimmed == '#') {
      if (fputs(line, out) == EOF) {
        bondage_set_error(errbuf, errbufsz, "write failed: %s", strerror(errno));
        goto cleanup;
      }
      continue;
    }

    memcpy(line_copy, line, strlen(line) + 1);
    eq = strchr(line_copy, '=');
    if (eq == NULL) {
      if (fputs(line, out) == EOF) {
        bondage_set_error(errbuf, errbufsz, "write failed: %s", strerror(errno));
        goto cleanup;
      }
      continue;
    }

    *eq = '\0';
    key = bondage_trim(line_copy);

    if (section == BONDAGE_REPIN_SECTION_GLOBAL) {
      replacement = bondage_global_replacement(plan, key);
    }
    else if ((section == BONDAGE_REPIN_SECTION_DEFAULTS ||
              section == BONDAGE_REPIN_SECTION_PROFILE) &&
             current_section_name[0] != '\0') {
      replacement = bondage_profile_field_replacement(plan, section,
                                                      current_section_name,
                                                      key);
    }

    if (replacement != NULL) {
      if (!bondage_write_line(out, key, replacement, errbuf, errbufsz)) {
        goto cleanup;
      }
      continue;
    }

    if (fputs(line, out) == EOF) {
      bondage_set_error(errbuf, errbufsz, "write failed: %s", strerror(errno));
      goto cleanup;
    }
  }

  if (fflush(out) != 0) {
    bondage_set_error(errbuf, errbufsz, "flush failed: %s", strerror(errno));
    goto cleanup;
  }

  if (fclose(out) != 0) {
    out = NULL;
    bondage_set_error(errbuf, errbufsz, "close failed: %s", strerror(errno));
    goto cleanup;
  }
  out = NULL;

  if (fclose(in) != 0) {
    in = NULL;
    bondage_set_error(errbuf, errbufsz, "close failed: %s", strerror(errno));
    goto cleanup;
  }
  in = NULL;

  if (rename(tmp_path, config_path) != 0) {
    bondage_set_error(errbuf, errbufsz, "rename failed: %s", strerror(errno));
    goto cleanup;
  }

  ok = 1;

cleanup:
  if (in != NULL) fclose(in);
  if (out != NULL) fclose(out);
  if (fd >= 0) close(fd);
  if (!ok && tmp_path != NULL) unlink(tmp_path);
  free(tmp_path);
  return ok;
}

static void
bondage_print_named_list(const struct bondage_named_list *list)
{
  size_t i;

  for (i = 0; i < list->count; i++) {
    printf("%s%s", i == 0 ? " " : ", ", list->items[i]);
  }
}

static void
bondage_print_update_owner(const char *key,
                           const struct bondage_field_owner *owner,
                           const struct bondage_named_list *profiles)
{
  if (owner->kind == BONDAGE_OWNER_DEFAULT && owner->name != NULL) {
    printf("updated %s in defaults \"%s\"", key, owner->name);
  }
  else if (owner->kind == BONDAGE_OWNER_PROFILE) {
    printf("updated %s for profiles:", key);
    bondage_print_named_list(profiles);
  }
  else {
    printf("updated %s in unknown owner", key);
  }
  printf("\n");
}

static void
bondage_print_repin_summary(const struct bondage_repin_plan *plan)
{
  if (plan->global_nono.active) {
    printf("updated global nono: %s\n", plan->global_nono.value);
  }
  if (plan->global_envchain.active) {
    printf("updated global envchain: %s\n", plan->global_envchain.value);
  }
  if (plan->global_touchid.active) {
    printf("updated global touchid: %s\n", plan->global_touchid.value);
  }

  if (plan->target.active) {
    bondage_print_update_owner("target", &plan->target.path_owner,
                               &plan->target.path_profiles);
    bondage_print_update_owner("target_fp", &plan->target.fp_owner,
                               &plan->target.fp_profiles);
    printf("target: %s\n", plan->target.new_path);
  }

  if (plan->interpreter.active) {
    bondage_print_update_owner("interpreter", &plan->interpreter.path_owner,
                               &plan->interpreter.path_profiles);
    bondage_print_update_owner("interpreter_fp", &plan->interpreter.fp_owner,
                               &plan->interpreter.fp_profiles);
    printf("interpreter: %s\n", plan->interpreter.new_path);
  }

  if (plan->package_root.active) {
    bondage_print_update_owner("package_root", &plan->package_root.path_owner,
                               &plan->package_root.path_profiles);
    bondage_print_update_owner("package_tree_fp", &plan->package_root.fp_owner,
                               &plan->package_root.fp_profiles);
    printf("package_root: %s\n", plan->package_root.new_path);
  }
}

static int
bondage_plan_has_global_changes(const struct bondage_config *config,
                                const struct bondage_repin_plan *plan)
{
  if (bondage_value_update_needed(&plan->global_nono, config->global.nono)) return 1;
  if (bondage_value_update_needed(&plan->global_nono_fp, config->global.nono_fp)) return 1;
  if (bondage_value_update_needed(&plan->global_envchain, config->global.envchain)) return 1;
  if (bondage_value_update_needed(&plan->global_envchain_fp, config->global.envchain_fp)) return 1;
  if (bondage_value_update_needed(&plan->global_touchid, config->global.touchid)) return 1;
  if (bondage_value_update_needed(&plan->global_touchid_fp, config->global.touchid_fp)) return 1;
  return 0;
}

static int
bondage_profile_has_changes(const struct bondage_profile *profile,
                            const struct bondage_repin_plan *plan)
{
  if (bondage_profile_field_update_needed(&plan->target,
                                          profile->target,
                                          profile->target_fp)) {
    return 1;
  }

  if (bondage_profile_field_update_needed(&plan->interpreter,
                                          profile->interpreter,
                                          profile->interpreter_fp)) {
    return 1;
  }

  if (bondage_profile_field_update_needed(&plan->package_root,
                                          profile->package_root,
                                          profile->package_tree_fp)) {
    return 1;
  }

  return 0;
}

static void
bondage_print_doctor_global_issue(const char *label,
                                  const char *configured_value,
                                  const char *configured_fp,
                                  const struct bondage_value_update *value_update,
                                  const struct bondage_value_update *fp_update)
{
  if (!bondage_value_update_needed(value_update, configured_value) &&
      !bondage_value_update_needed(fp_update, configured_fp)) {
    return;
  }

  printf("global %s: stale\n", label);
  if (configured_value != NULL && value_update->active &&
      strcmp(configured_value, value_update->value) != 0) {
    printf("  configured: %s\n", configured_value);
    printf("  current:    %s\n", value_update->value);
  }
  if (configured_fp != NULL && fp_update->active &&
      strcmp(configured_fp, fp_update->value) != 0) {
    printf("  configured_fp: %s\n", configured_fp);
    printf("  current_fp:    %s\n", fp_update->value);
  }
}

static void
bondage_print_doctor_owner_line(const char *label,
                                const struct bondage_field_owner *owner)
{
  if (owner->kind == BONDAGE_OWNER_DEFAULT && owner->name != NULL) {
    printf("  %s via defaults \"%s\"\n", label, owner->name);
  }
  else if (owner->kind == BONDAGE_OWNER_PROFILE && owner->name != NULL) {
    printf("  %s via profile \"%s\"\n", label, owner->name);
  }
  else {
    printf("  %s via %s\n", label, bondage_owner_label(owner));
  }
}

static void
bondage_print_doctor_profile_issue(const struct bondage_profile *profile,
                                   const struct bondage_repin_plan *plan)
{
  int header = 0;

  if (bondage_profile_field_update_needed(&plan->target,
                                          profile->target,
                                          profile->target_fp)) {
    if (!header) {
      printf("profile %s: stale\n", profile->name);
      header = 1;
    }
    bondage_print_doctor_owner_line("target", &plan->target.path_owner);
    if (plan->target.fp_owner.kind != plan->target.path_owner.kind ||
        ((plan->target.fp_owner.name == NULL) !=
         (plan->target.path_owner.name == NULL)) ||
        (plan->target.fp_owner.name != NULL &&
         strcmp(plan->target.fp_owner.name, plan->target.path_owner.name) != 0)) {
      bondage_print_doctor_owner_line("target_fp", &plan->target.fp_owner);
    }
    if (profile->target != NULL && strcmp(profile->target, plan->target.new_path) != 0) {
      printf("    configured: %s\n", profile->target);
      printf("    current:    %s\n", plan->target.new_path);
    }
    if (profile->target_fp != NULL && strcmp(profile->target_fp, plan->target.new_fp) != 0) {
      printf("    configured_fp: %s\n", profile->target_fp);
      printf("    current_fp:    %s\n", plan->target.new_fp);
    }
  }

  if (bondage_profile_field_update_needed(&plan->interpreter,
                                          profile->interpreter,
                                          profile->interpreter_fp)) {
    if (!header) {
      printf("profile %s: stale\n", profile->name);
      header = 1;
    }
    bondage_print_doctor_owner_line("interpreter", &plan->interpreter.path_owner);
    if (plan->interpreter.fp_owner.kind != plan->interpreter.path_owner.kind ||
        ((plan->interpreter.fp_owner.name == NULL) !=
         (plan->interpreter.path_owner.name == NULL)) ||
        (plan->interpreter.fp_owner.name != NULL &&
         strcmp(plan->interpreter.fp_owner.name,
                plan->interpreter.path_owner.name) != 0)) {
      bondage_print_doctor_owner_line("interpreter_fp", &plan->interpreter.fp_owner);
    }
    if (profile->interpreter != NULL &&
        strcmp(profile->interpreter, plan->interpreter.new_path) != 0) {
      printf("    configured: %s\n", profile->interpreter);
      printf("    current:    %s\n", plan->interpreter.new_path);
    }
    if (profile->interpreter_fp != NULL &&
        strcmp(profile->interpreter_fp, plan->interpreter.new_fp) != 0) {
      printf("    configured_fp: %s\n", profile->interpreter_fp);
      printf("    current_fp:    %s\n", plan->interpreter.new_fp);
    }
  }

  if (bondage_profile_field_update_needed(&plan->package_root,
                                          profile->package_root,
                                          profile->package_tree_fp)) {
    if (!header) {
      printf("profile %s: stale\n", profile->name);
      header = 1;
    }
    bondage_print_doctor_owner_line("package_root",
                                    &plan->package_root.path_owner);
    if (plan->package_root.fp_owner.kind != plan->package_root.path_owner.kind ||
        ((plan->package_root.fp_owner.name == NULL) !=
         (plan->package_root.path_owner.name == NULL)) ||
        (plan->package_root.fp_owner.name != NULL &&
         strcmp(plan->package_root.fp_owner.name,
                plan->package_root.path_owner.name) != 0)) {
      bondage_print_doctor_owner_line("package_tree_fp",
                                      &plan->package_root.fp_owner);
    }
    if (profile->package_root != NULL &&
        strcmp(profile->package_root, plan->package_root.new_path) != 0) {
      printf("    configured: %s\n", profile->package_root);
      printf("    current:    %s\n", plan->package_root.new_path);
    }
    if (profile->package_tree_fp != NULL &&
        strcmp(profile->package_tree_fp, plan->package_root.new_fp) != 0) {
      printf("    configured_fp: %s\n", profile->package_tree_fp);
      printf("    current_fp:    %s\n", plan->package_root.new_fp);
    }
  }
}

int
bondage_repin_globals(const char *config_path)
{
  struct bondage_config config;
  struct bondage_config verify_config;
  struct bondage_repin_plan plan;
  char errbuf[256];

  memset(&plan, 0, sizeof(plan));
  bondage_config_init(&config);
  bondage_config_init(&verify_config);

  if (!bondage_config_load(config_path, &config, errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: %s\n", errbuf);
    return 1;
  }

  if (!bondage_build_global_plan(&config, &plan, errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: %s\n", errbuf);
    bondage_repin_plan_free(&plan);
    bondage_config_free(&config);
    return 1;
  }

  if (!bondage_plan_has_global_changes(&config, &plan)) {
    printf("status: clean\n");
    bondage_repin_plan_free(&plan);
    bondage_config_free(&config);
    return 0;
  }

  if (!bondage_rewrite_config(config_path, &plan, errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: %s\n", errbuf);
    bondage_repin_plan_free(&plan);
    bondage_config_free(&config);
    return 1;
  }

  if (!bondage_config_load(config_path, &verify_config, errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: repin-globals wrote invalid config: %s\n", errbuf);
    bondage_repin_plan_free(&plan);
    bondage_config_free(&config);
    bondage_config_free(&verify_config);
    return 1;
  }

  if (bondage_plan_has_global_changes(&verify_config, &plan)) {
    fprintf(stderr, "bondage: repin-globals verification failed\n");
    bondage_repin_plan_free(&plan);
    bondage_config_free(&config);
    bondage_config_free(&verify_config);
    return 1;
  }

  bondage_print_repin_summary(&plan);
  printf("status: repinned\n");

  bondage_repin_plan_free(&plan);
  bondage_config_free(&config);
  bondage_config_free(&verify_config);
  return 0;
}

int
bondage_doctor(const char *config_path)
{
  struct bondage_config config;
  struct bondage_repin_plan global_plan;
  struct bondage_named_list suggested_profiles;
  char errbuf[256];
  size_t i;
  int stale = 0;

  memset(&global_plan, 0, sizeof(global_plan));
  memset(&suggested_profiles, 0, sizeof(suggested_profiles));
  bondage_config_init(&config);

  if (!bondage_config_load(config_path, &config, errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: %s\n", errbuf);
    return 1;
  }

  if (!bondage_build_global_plan(&config, &global_plan, errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: %s\n", errbuf);
    bondage_repin_plan_free(&global_plan);
    bondage_named_list_free(&suggested_profiles);
    bondage_config_free(&config);
    return 1;
  }

  bondage_print_doctor_global_issue("nono",
                                    config.global.nono,
                                    config.global.nono_fp,
                                    &global_plan.global_nono,
                                    &global_plan.global_nono_fp);
  bondage_print_doctor_global_issue("envchain",
                                    config.global.envchain,
                                    config.global.envchain_fp,
                                    &global_plan.global_envchain,
                                    &global_plan.global_envchain_fp);
  bondage_print_doctor_global_issue("touchid",
                                    config.global.touchid,
                                    config.global.touchid_fp,
                                    &global_plan.global_touchid,
                                    &global_plan.global_touchid_fp);
  if (bondage_plan_has_global_changes(&config, &global_plan)) stale = 1;

  for (i = 0; i < config.profile_count; i++) {
    const struct bondage_profile *profile = &config.profiles[i];
    struct bondage_repin_plan plan;

    memset(&plan, 0, sizeof(plan));
    if (!bondage_build_repin_plan(&config, profile, &plan, errbuf, sizeof(errbuf))) {
      printf("profile %s: broken\n", profile->name);
      printf("  error: %s\n", errbuf);
      stale = 1;
      if (!bondage_named_list_contains(&suggested_profiles, profile->name) &&
          !bondage_named_list_append(&suggested_profiles, profile->name,
                                     errbuf, sizeof(errbuf))) {
        fprintf(stderr, "bondage: %s\n", errbuf);
        bondage_repin_plan_free(&plan);
        bondage_repin_plan_free(&global_plan);
        bondage_named_list_free(&suggested_profiles);
        bondage_config_free(&config);
        return 1;
      }
      bondage_repin_plan_free(&plan);
      continue;
    }

    if (bondage_profile_has_changes(profile, &plan)) {
      stale = 1;
      bondage_print_doctor_profile_issue(profile, &plan);
      if (!bondage_named_list_contains(&suggested_profiles, profile->name) &&
          !bondage_named_list_append(&suggested_profiles, profile->name,
                                     errbuf, sizeof(errbuf))) {
        fprintf(stderr, "bondage: %s\n", errbuf);
        bondage_repin_plan_free(&plan);
        bondage_repin_plan_free(&global_plan);
        bondage_named_list_free(&suggested_profiles);
        bondage_config_free(&config);
        return 1;
      }
    }

    bondage_repin_plan_free(&plan);
  }

  if (!stale) {
    printf("status: clean\n");
    bondage_repin_plan_free(&global_plan);
    bondage_named_list_free(&suggested_profiles);
    bondage_config_free(&config);
    return 0;
  }

  if (bondage_plan_has_global_changes(&config, &global_plan)) {
    printf("suggest: bondage repin-globals %s\n", config_path);
  }
  for (i = 0; i < suggested_profiles.count; i++) {
    printf("suggest: bondage repin %s %s\n", suggested_profiles.items[i], config_path);
  }
  printf("status: stale\n");

  bondage_repin_plan_free(&global_plan);
  bondage_named_list_free(&suggested_profiles);
  bondage_config_free(&config);
  return 1;
}

int
bondage_repin(const char *profile_name, const char *config_path)
{
  struct bondage_config config;
  const struct bondage_profile *profile;
  struct bondage_repin_plan plan;
  struct bondage_config verify_config;
  const struct bondage_profile *verify_profile;
  char errbuf[256];

  memset(&plan, 0, sizeof(plan));
  bondage_config_init(&config);
  bondage_config_init(&verify_config);

  if (!bondage_config_load(config_path, &config, errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: %s\n", errbuf);
    bondage_repin_plan_free(&plan);
    bondage_config_free(&config);
    return 1;
  }

  profile = bondage_config_find_profile(&config, profile_name);
  if (profile == NULL) {
    fprintf(stderr, "bondage: unknown profile '%s'\n", profile_name);
    bondage_repin_plan_free(&plan);
    bondage_config_free(&config);
    return 1;
  }

  if (!bondage_build_repin_plan(&config, profile, &plan, errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: %s\n", errbuf);
    bondage_repin_plan_free(&plan);
    bondage_config_free(&config);
    return 1;
  }

  if (!bondage_rewrite_config(config_path, &plan, errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: %s\n", errbuf);
    bondage_repin_plan_free(&plan);
    bondage_config_free(&config);
    return 1;
  }

  if (!bondage_config_load(config_path, &verify_config, errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: repin wrote invalid config: %s\n", errbuf);
    bondage_repin_plan_free(&plan);
    bondage_config_free(&config);
    bondage_config_free(&verify_config);
    return 1;
  }

  verify_profile = bondage_config_find_profile(&verify_config, profile_name);
  if (verify_profile == NULL ||
      !bondage_verify_profile(&verify_config, verify_profile, NULL,
                              errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: repin verification failed: %s\n", errbuf);
    bondage_repin_plan_free(&plan);
    bondage_config_free(&config);
    bondage_config_free(&verify_config);
    return 1;
  }

  bondage_print_repin_summary(&plan);
  printf("status: repinned\n");

  bondage_repin_plan_free(&plan);
  bondage_config_free(&config);
  bondage_config_free(&verify_config);
  return 0;
}

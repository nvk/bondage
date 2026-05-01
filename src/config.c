#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

enum bondage_section {
  BONDAGE_SECTION_NONE = 0,
  BONDAGE_SECTION_GLOBAL,
  BONDAGE_SECTION_DEFAULTS,
  BONDAGE_SECTION_PROFILE
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
bondage_trim(char *value);

static void
bondage_free_string_list(struct bondage_string_list *list)
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
bondage_string_list_append(struct bondage_string_list *list, const char *value,
                           char *errbuf, size_t errbufsz)
{
  char **grown;
  char *copy;

  copy = bondage_xstrdup(value);
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
bondage_string_list_contains(const struct bondage_string_list *list,
                             const char *value)
{
  size_t i;

  for (i = 0; i < list->count; i++) {
    if (strcmp(list->items[i], value) == 0) return 1;
  }

  return 0;
}

static int
bondage_string_list_append_csv(struct bondage_string_list *list,
                               const char *value,
                               char *errbuf,
                               size_t errbufsz)
{
  char *copy = bondage_xstrdup(value);
  char *cursor;

  if (copy == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  cursor = copy;
  while (cursor != NULL) {
    char *comma = strchr(cursor, ',');
    char *item;

    if (comma != NULL) {
      *comma = '\0';
    }

    item = bondage_trim(cursor);
    if (*item == '\0') {
      free(copy);
      bondage_set_error(errbuf, errbufsz, "empty inherited defaults name");
      return 0;
    }

    if (bondage_string_list_contains(list, item)) {
      bondage_set_error(errbuf, errbufsz,
                        "duplicate inherited defaults '%s'", item);
      free(copy);
      return 0;
    }

    if (!bondage_string_list_append(list, item, errbuf, errbufsz)) {
      free(copy);
      return 0;
    }

    cursor = comma == NULL ? NULL : comma + 1;
  }

  free(copy);
  return 1;
}

static int
bondage_parse_bool(const char *value, int *out, char *errbuf, size_t errbufsz)
{
  if (strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 ||
      strcmp(value, "1") == 0) {
    *out = 1;
    return 1;
  }
  if (strcmp(value, "false") == 0 || strcmp(value, "no") == 0 ||
      strcmp(value, "0") == 0) {
    *out = 0;
    return 1;
  }

  bondage_set_error(errbuf, errbufsz, "invalid boolean '%s'", value);
  return 0;
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

static void
bondage_free_field_owner(struct bondage_field_owner *owner)
{
  free(owner->name);
  owner->name = NULL;
  owner->kind = BONDAGE_OWNER_NONE;
}

static int
bondage_replace_field_owner(struct bondage_field_owner *owner,
                            enum bondage_owner_kind kind,
                            const char *name,
                            char *errbuf,
                            size_t errbufsz)
{
  char *copy = NULL;

  if (name != NULL) {
    copy = bondage_xstrdup(name);
    if (copy == NULL) {
      bondage_set_error(errbuf, errbufsz, "out of memory");
      return 0;
    }
  }

  bondage_free_field_owner(owner);
  owner->kind = kind;
  owner->name = copy;
  return 1;
}

static void
bondage_free_profile(struct bondage_profile *profile)
{
  free(profile->name);
  bondage_free_string_list(&profile->inherits);
  free(profile->namespace_name);
  free(profile->nono_profile);
  free(profile->touch_policy);
  free(profile->target_kind);
  free(profile->target);
  free(profile->target_fp);
  free(profile->interpreter);
  free(profile->interpreter_fp);
  free(profile->package_root);
  free(profile->package_tree_fp);
  bondage_free_string_list(&profile->nono_allow_dirs);
  bondage_free_string_list(&profile->nono_read_dirs);
  bondage_free_string_list(&profile->nono_allow_files);
  bondage_free_string_list(&profile->nono_read_files);
  bondage_free_string_list(&profile->target_args);
  bondage_free_string_list(&profile->env_set);
  bondage_free_string_list(&profile->env_command);
  bondage_free_string_list(&profile->ensure_dir);
  bondage_free_field_owner(&profile->target_owner);
  bondage_free_field_owner(&profile->target_fp_owner);
  bondage_free_field_owner(&profile->interpreter_owner);
  bondage_free_field_owner(&profile->interpreter_fp_owner);
  bondage_free_field_owner(&profile->package_root_owner);
  bondage_free_field_owner(&profile->package_tree_fp_owner);
  memset(profile, 0, sizeof(*profile));
}

static int
bondage_replace_string(char **slot, const char *value, char *errbuf,
                       size_t errbufsz)
{
  char *copy = bondage_xstrdup(value);
  if (copy == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  free(*slot);
  *slot = copy;
  return 1;
}

static int
bondage_parse_value(char *raw, char **out, char *errbuf, size_t errbufsz)
{
  char *value = bondage_trim(raw);
  size_t len;

  if (*value == '\0') {
    bondage_set_error(errbuf, errbufsz, "empty value");
    return 0;
  }

  len = strlen(value);
  if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
    value[len - 1] = '\0';
    value++;
  }

  *out = value;
  return 1;
}

static int
bondage_parse_named_section(char *namebuf, const char *section_name,
                            char **value_name)
{
  char prefix[64];
  size_t len = strlen(namebuf);
  size_t prefix_len;

  snprintf(prefix, sizeof(prefix), "%s \"", section_name);
  prefix_len = strlen(prefix);

  if (strncmp(namebuf, prefix, prefix_len) != 0) return 0;
  if (len <= prefix_len || namebuf[len - 1] != '"') return 0;

  namebuf[len - 1] = '\0';
  *value_name = namebuf + prefix_len;
  return **value_name != '\0';
}

static int
bondage_parse_defaults_section(char *namebuf, char **defaults_name)
{
  return bondage_parse_named_section(namebuf, "defaults", defaults_name);
}

static int
bondage_parse_profile_section(char *namebuf, char **profile_name)
{
  return bondage_parse_named_section(namebuf, "profile", profile_name);
}

static const struct bondage_profile *
bondage_find_defaults(const struct bondage_config *config, const char *name)
{
  size_t i;

  for (i = 0; i < config->default_count; i++) {
    if (config->defaults[i].name != NULL &&
        strcmp(config->defaults[i].name, name) == 0) {
      return &config->defaults[i];
    }
  }

  return NULL;
}

static int
bondage_push_defaults(struct bondage_config *config, const char *name,
                      struct bondage_profile **out, char *errbuf, size_t errbufsz)
{
  struct bondage_profile *defaults;
  struct bondage_profile *block;

  if (bondage_find_defaults(config, name) != NULL) {
    bondage_set_error(errbuf, errbufsz, "duplicate defaults '%s'", name);
    return 0;
  }

  defaults = realloc(config->defaults,
                     sizeof(struct bondage_profile) * (config->default_count + 1));
  if (defaults == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  config->defaults = defaults;
  block = &config->defaults[config->default_count];
  memset(block, 0, sizeof(*block));
  config->default_count++;

  block->name = bondage_xstrdup(name);
  if (block->name == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  *out = block;
  return 1;
}

static int
bondage_push_profile(struct bondage_config *config, const char *name,
                     struct bondage_profile **out, char *errbuf, size_t errbufsz)
{
  struct bondage_profile *profiles;
  struct bondage_profile *profile;

  profiles = realloc(config->profiles,
                     sizeof(struct bondage_profile) * (config->profile_count + 1));
  if (profiles == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  config->profiles = profiles;
  profile = &config->profiles[config->profile_count];
  memset(profile, 0, sizeof(*profile));
  config->profile_count++;

  profile->name = bondage_xstrdup(name);
  if (profile->name == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  *out = profile;
  return 1;
}

static int
bondage_assign_global(struct bondage_global *global, const char *key,
                      const char *value, char *errbuf, size_t errbufsz)
{
  if (strcmp(key, "envchain") == 0) {
    return bondage_replace_string(&global->envchain, value, errbuf, errbufsz);
  }
  if (strcmp(key, "envchain_fp") == 0) {
    return bondage_replace_string(&global->envchain_fp, value, errbuf, errbufsz);
  }
  if (strcmp(key, "nono") == 0) {
    return bondage_replace_string(&global->nono, value, errbuf, errbufsz);
  }
  if (strcmp(key, "nono_fp") == 0) {
    return bondage_replace_string(&global->nono_fp, value, errbuf, errbufsz);
  }
  if (strcmp(key, "nono_profile_root") == 0) {
    return bondage_replace_string(&global->nono_profile_root, value, errbuf, errbufsz);
  }
  if (strcmp(key, "touchid") == 0) {
    return bondage_replace_string(&global->touchid, value, errbuf, errbufsz);
  }
  if (strcmp(key, "touchid_fp") == 0) {
    return bondage_replace_string(&global->touchid_fp, value, errbuf, errbufsz);
  }
  if (strcmp(key, "tool_root") == 0) {
    return bondage_replace_string(&global->tool_root, value, errbuf, errbufsz);
  }

  bondage_set_error(errbuf, errbufsz, "unknown global key '%s'", key);
  return 0;
}

static int
bondage_assign_profile(struct bondage_profile *profile, const char *key,
                       const char *value, int allow_inherits,
                       char *errbuf, size_t errbufsz)
{
  if (strcmp(key, "use_envchain") == 0) {
    if (!bondage_parse_bool(value, &profile->use_envchain, errbuf, errbufsz)) {
      return 0;
    }
    profile->use_envchain_set = 1;
    return 1;
  }
  if (strcmp(key, "use_nono") == 0) {
    if (!bondage_parse_bool(value, &profile->use_nono, errbuf, errbufsz)) {
      return 0;
    }
    profile->use_nono_set = 1;
    return 1;
  }
  if (strcmp(key, "inherits") == 0) {
    if (!allow_inherits) {
      bondage_set_error(errbuf, errbufsz, "inherits is only valid in profile sections");
      return 0;
    }
    return bondage_string_list_append_csv(&profile->inherits, value,
                                          errbuf, errbufsz);
  }
  if (strcmp(key, "namespace") == 0) {
    return bondage_replace_string(&profile->namespace_name, value, errbuf, errbufsz);
  }
  if (strcmp(key, "nono_profile") == 0) {
    return bondage_replace_string(&profile->nono_profile, value, errbuf, errbufsz);
  }
  if (strcmp(key, "touch_policy") == 0) {
    return bondage_replace_string(&profile->touch_policy, value, errbuf, errbufsz);
  }
  if (strcmp(key, "target_kind") == 0) {
    return bondage_replace_string(&profile->target_kind, value, errbuf, errbufsz);
  }
  if (strcmp(key, "target") == 0) {
    return bondage_replace_string(&profile->target, value, errbuf, errbufsz);
  }
  if (strcmp(key, "target_fp") == 0) {
    return bondage_replace_string(&profile->target_fp, value, errbuf, errbufsz);
  }
  if (strcmp(key, "interpreter") == 0) {
    return bondage_replace_string(&profile->interpreter, value, errbuf, errbufsz);
  }
  if (strcmp(key, "interpreter_fp") == 0) {
    return bondage_replace_string(&profile->interpreter_fp, value, errbuf, errbufsz);
  }
  if (strcmp(key, "package_root") == 0) {
    return bondage_replace_string(&profile->package_root, value, errbuf, errbufsz);
  }
  if (strcmp(key, "package_tree_fp") == 0) {
    return bondage_replace_string(&profile->package_tree_fp, value, errbuf, errbufsz);
  }
  if (strcmp(key, "nono_allow_cwd") == 0) {
    if (!bondage_parse_bool(value, &profile->nono_allow_cwd, errbuf, errbufsz)) {
      return 0;
    }
    profile->nono_allow_cwd_set = 1;
    return 1;
  }
  if (strcmp(key, "nono_allow_file") == 0) {
    return bondage_string_list_append(&profile->nono_allow_files, value,
                                      errbuf, errbufsz);
  }
  if (strcmp(key, "nono_read_file") == 0) {
    return bondage_string_list_append(&profile->nono_read_files, value,
                                      errbuf, errbufsz);
  }
  if (strcmp(key, "nono_allow_dir") == 0) {
    return bondage_string_list_append(&profile->nono_allow_dirs, value,
                                      errbuf, errbufsz);
  }
  if (strcmp(key, "nono_read_dir") == 0) {
    return bondage_string_list_append(&profile->nono_read_dirs, value,
                                      errbuf, errbufsz);
  }
  if (strcmp(key, "target_arg") == 0) {
    return bondage_string_list_append(&profile->target_args, value,
                                      errbuf, errbufsz);
  }
  if (strcmp(key, "env_set") == 0) {
    return bondage_string_list_append(&profile->env_set, value, errbuf, errbufsz);
  }
  if (strcmp(key, "env_command") == 0) {
    return bondage_string_list_append(&profile->env_command, value, errbuf, errbufsz);
  }
  if (strcmp(key, "ensure_dir") == 0) {
    return bondage_string_list_append(&profile->ensure_dir, value, errbuf, errbufsz);
  }

  bondage_set_error(errbuf, errbufsz, "unknown profile key '%s'", key);
  return 0;
}

static int
bondage_copy_string_list(struct bondage_string_list *dst,
                         const struct bondage_string_list *src,
                         char *errbuf,
                         size_t errbufsz)
{
  size_t i;

  for (i = 0; i < src->count; i++) {
    if (!bondage_string_list_append(dst, src->items[i], errbuf, errbufsz)) {
      return 0;
    }
  }

  return 1;
}

static int
bondage_apply_string(char **dst,
                     const char *src,
                     char *errbuf,
                     size_t errbufsz)
{
  if (src == NULL) return 1;
  return bondage_replace_string(dst, src, errbuf, errbufsz);
}

static int
bondage_apply_owned_string(char **dst,
                           struct bondage_field_owner *owner,
                           const char *src,
                           enum bondage_owner_kind owner_kind,
                           const char *owner_name,
                           char *errbuf,
                           size_t errbufsz)
{
  if (src == NULL) return 1;
  if (!bondage_replace_string(dst, src, errbuf, errbufsz)) return 0;
  return bondage_replace_field_owner(owner, owner_kind, owner_name,
                                     errbuf, errbufsz);
}

static int
bondage_apply_profile_values(struct bondage_profile *dst,
                             const struct bondage_profile *src,
                             enum bondage_owner_kind owner_kind,
                             const char *owner_name,
                             char *errbuf,
                             size_t errbufsz)
{
  if (src->use_envchain_set) {
    dst->use_envchain = src->use_envchain;
    dst->use_envchain_set = 1;
  }
  if (src->use_nono_set) {
    dst->use_nono = src->use_nono;
    dst->use_nono_set = 1;
  }
  if (src->nono_allow_cwd_set) {
    dst->nono_allow_cwd = src->nono_allow_cwd;
    dst->nono_allow_cwd_set = 1;
  }

  if (!bondage_apply_string(&dst->namespace_name, src->namespace_name,
                            errbuf, errbufsz)) return 0;
  if (!bondage_apply_string(&dst->nono_profile, src->nono_profile,
                            errbuf, errbufsz)) return 0;
  if (!bondage_apply_string(&dst->touch_policy, src->touch_policy,
                            errbuf, errbufsz)) return 0;
  if (!bondage_apply_string(&dst->target_kind, src->target_kind,
                            errbuf, errbufsz)) return 0;
  if (!bondage_apply_owned_string(&dst->target, &dst->target_owner,
                                  src->target, owner_kind, owner_name,
                                  errbuf, errbufsz)) return 0;
  if (!bondage_apply_owned_string(&dst->target_fp, &dst->target_fp_owner,
                                  src->target_fp, owner_kind, owner_name,
                                  errbuf, errbufsz)) return 0;
  if (!bondage_apply_owned_string(&dst->interpreter, &dst->interpreter_owner,
                                  src->interpreter, owner_kind, owner_name,
                                  errbuf, errbufsz)) return 0;
  if (!bondage_apply_owned_string(&dst->interpreter_fp,
                                  &dst->interpreter_fp_owner,
                                  src->interpreter_fp, owner_kind, owner_name,
                                  errbuf, errbufsz)) return 0;
  if (!bondage_apply_owned_string(&dst->package_root,
                                  &dst->package_root_owner,
                                  src->package_root, owner_kind, owner_name,
                                  errbuf, errbufsz)) return 0;
  if (!bondage_apply_owned_string(&dst->package_tree_fp,
                                  &dst->package_tree_fp_owner,
                                  src->package_tree_fp, owner_kind, owner_name,
                                  errbuf, errbufsz)) return 0;

  if (!bondage_copy_string_list(&dst->nono_allow_dirs, &src->nono_allow_dirs,
                                errbuf, errbufsz)) return 0;
  if (!bondage_copy_string_list(&dst->nono_read_dirs, &src->nono_read_dirs,
                                errbuf, errbufsz)) return 0;
  if (!bondage_copy_string_list(&dst->nono_allow_files, &src->nono_allow_files,
                                errbuf, errbufsz)) return 0;
  if (!bondage_copy_string_list(&dst->nono_read_files, &src->nono_read_files,
                                errbuf, errbufsz)) return 0;
  if (!bondage_copy_string_list(&dst->target_args, &src->target_args,
                                errbuf, errbufsz)) return 0;
  if (!bondage_copy_string_list(&dst->env_set, &src->env_set,
                                errbuf, errbufsz)) return 0;
  if (!bondage_copy_string_list(&dst->env_command, &src->env_command,
                                errbuf, errbufsz)) return 0;
  if (!bondage_copy_string_list(&dst->ensure_dir, &src->ensure_dir,
                                errbuf, errbufsz)) return 0;

  return 1;
}

static int
bondage_resolve_profile(const struct bondage_config *config,
                        const struct bondage_profile *raw,
                        struct bondage_profile *resolved,
                        char *errbuf,
                        size_t errbufsz)
{
  size_t i;

  memset(resolved, 0, sizeof(*resolved));
  resolved->use_envchain = 1;
  resolved->use_nono = 1;

  resolved->name = bondage_xstrdup(raw->name);
  if (resolved->name == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  if (!bondage_copy_string_list(&resolved->inherits, &raw->inherits,
                                errbuf, errbufsz)) {
    return 0;
  }

  for (i = 0; i < raw->inherits.count; i++) {
    const char *defaults_name = raw->inherits.items[i];
    const struct bondage_profile *defaults_block =
      bondage_find_defaults(config, defaults_name);

    if (defaults_block == NULL) {
      bondage_set_error(errbuf, errbufsz,
                        "profile '%s' inherits unknown defaults '%s'",
                        raw->name, defaults_name);
      return 0;
    }

    if (!bondage_apply_profile_values(resolved, defaults_block,
                                      BONDAGE_OWNER_DEFAULT, defaults_name,
                                      errbuf, errbufsz)) {
      return 0;
    }
  }

  return bondage_apply_profile_values(resolved, raw,
                                      BONDAGE_OWNER_PROFILE, raw->name,
                                      errbuf, errbufsz);
}

static int
bondage_resolve_profiles(struct bondage_config *config,
                         char *errbuf,
                         size_t errbufsz)
{
  size_t i;

  for (i = 0; i < config->profile_count; i++) {
    struct bondage_profile raw = config->profiles[i];
    struct bondage_profile resolved;

    memset(&config->profiles[i], 0, sizeof(config->profiles[i]));
    if (!bondage_resolve_profile(config, &raw, &resolved, errbuf, errbufsz)) {
      bondage_free_profile(&resolved);
      bondage_free_profile(&raw);
      return 0;
    }

    bondage_free_profile(&raw);
    config->profiles[i] = resolved;
  }

  return 1;
}

static int
bondage_string_has_key_value(const char *value)
{
  const char *eq = strchr(value, '=');
  return eq != NULL && eq != value;
}

static int
bondage_validate_string_list_paths(const struct bondage_string_list *list,
                                   const char *label,
                                   const char *profile_name,
                                   char *errbuf,
                                   size_t errbufsz)
{
  size_t i;

  for (i = 0; i < list->count; i++) {
    if (list->items[i] == NULL || list->items[i][0] != '/') {
      bondage_set_error(errbuf, errbufsz,
                        "profile '%s' %s must be an absolute path",
                        profile_name, label);
      return 0;
    }
  }

  return 1;
}

static int
bondage_validate_profile(const struct bondage_profile *profile, char *errbuf,
                         size_t errbufsz)
{
  size_t root_len;
  size_t i;

  if (profile->touch_policy == NULL || profile->target_kind == NULL ||
      profile->target == NULL || profile->target_fp == NULL) {
    bondage_set_error(errbuf, errbufsz,
                      "profile '%s' is missing required keys",
                      profile->name ? profile->name : "<unknown>");
    return 0;
  }

  if (strcmp(profile->touch_policy, "none") != 0 &&
      strcmp(profile->touch_policy, "prompt") != 0) {
    bondage_set_error(errbuf, errbufsz,
                      "profile '%s' has unknown touch_policy '%s'",
                      profile->name, profile->touch_policy);
    return 0;
  }

  if (profile->use_envchain && profile->namespace_name == NULL) {
    bondage_set_error(errbuf, errbufsz,
                      "profile '%s' requires namespace when use_envchain=true",
                      profile->name);
    return 0;
  }

  if (profile->use_nono && profile->nono_profile == NULL) {
    bondage_set_error(errbuf, errbufsz,
                      "profile '%s' requires nono_profile when use_nono=true",
                      profile->name);
    return 0;
  }

  if (!profile->use_nono &&
      (profile->nono_profile != NULL || profile->nono_allow_cwd ||
       profile->nono_allow_dirs.count != 0 || profile->nono_read_dirs.count != 0 ||
       profile->nono_allow_files.count != 0 || profile->nono_read_files.count != 0)) {
    bondage_set_error(errbuf, errbufsz,
                      "profile '%s' has nono settings but use_nono=false",
                      profile->name);
    return 0;
  }

  if (strcmp(profile->target_kind, "script") == 0) {
    if (profile->interpreter == NULL || profile->interpreter_fp == NULL) {
      bondage_set_error(errbuf, errbufsz,
                        "script profile '%s' requires interpreter and interpreter_fp",
                        profile->name);
      return 0;
    }
  }
  else if (profile->package_root != NULL || profile->package_tree_fp != NULL) {
    bondage_set_error(errbuf, errbufsz,
                      "package_root/package_tree_fp are only valid for script profiles");
    return 0;
  }

  if ((profile->package_root == NULL) != (profile->package_tree_fp == NULL)) {
    bondage_set_error(errbuf, errbufsz,
                      "profile '%s' requires both package_root and package_tree_fp",
                      profile->name);
    return 0;
  }

  if (profile->package_root != NULL) {
    root_len = strlen(profile->package_root);
    if (strncmp(profile->target, profile->package_root, root_len) != 0 ||
        profile->target[root_len] != '/') {
      bondage_set_error(errbuf, errbufsz,
                        "profile '%s' target must live under package_root",
                        profile->name);
      return 0;
    }
  }

  if (!bondage_validate_string_list_paths(&profile->nono_allow_files,
                                          "nono_allow_file", profile->name,
                                          errbuf, errbufsz)) {
    return 0;
  }
  if (!bondage_validate_string_list_paths(&profile->nono_read_files,
                                          "nono_read_file", profile->name,
                                          errbuf, errbufsz)) {
    return 0;
  }
  if (!bondage_validate_string_list_paths(&profile->nono_allow_dirs,
                                          "nono_allow_dir", profile->name,
                                          errbuf, errbufsz)) {
    return 0;
  }
  if (!bondage_validate_string_list_paths(&profile->nono_read_dirs,
                                          "nono_read_dir", profile->name,
                                          errbuf, errbufsz)) {
    return 0;
  }
  if (!bondage_validate_string_list_paths(&profile->ensure_dir,
                                          "ensure_dir", profile->name,
                                          errbuf, errbufsz)) {
    return 0;
  }

  for (i = 0; i < profile->env_set.count; i++) {
    if (!bondage_string_has_key_value(profile->env_set.items[i])) {
      bondage_set_error(errbuf, errbufsz,
                        "profile '%s' env_set requires NAME=value",
                        profile->name);
      return 0;
    }
  }

  for (i = 0; i < profile->env_command.count; i++) {
    if (!bondage_string_has_key_value(profile->env_command.items[i])) {
      bondage_set_error(errbuf, errbufsz,
                        "profile '%s' env_command requires NAME=/absolute/cmd ...",
                        profile->name);
      return 0;
    }
  }

  return 1;
}

static int
bondage_validate_config(const struct bondage_config *config, char *errbuf,
                        size_t errbufsz)
{
  size_t i;
  int need_envchain = 0;
  int need_touchid = 0;

  if (config->global.nono == NULL || config->global.nono_fp == NULL) {
    bondage_set_error(errbuf, errbufsz,
                      "global section is missing required keys");
    return 0;
  }

  for (i = 0; i < config->profile_count; i++) {
    if (config->profiles[i].use_envchain) need_envchain = 1;
    if (strcmp(config->profiles[i].touch_policy, "prompt") == 0) need_touchid = 1;
    if (!bondage_validate_profile(&config->profiles[i], errbuf, errbufsz)) {
      return 0;
    }
  }

  if (need_envchain &&
      (config->global.envchain == NULL || config->global.envchain_fp == NULL)) {
    bondage_set_error(errbuf, errbufsz,
                      "global section is missing envchain/envchain_fp");
    return 0;
  }

  if (need_touchid &&
      (config->global.touchid == NULL || config->global.touchid_fp == NULL)) {
    bondage_set_error(errbuf, errbufsz,
                      "global section is missing touchid/touchid_fp");
    return 0;
  }

  return 1;
}

void
bondage_config_init(struct bondage_config *config)
{
  memset(config, 0, sizeof(*config));
}

void
bondage_config_free(struct bondage_config *config)
{
  size_t i;

  free(config->global.envchain);
  free(config->global.envchain_fp);
  free(config->global.nono);
  free(config->global.nono_fp);
  free(config->global.nono_profile_root);
  free(config->global.touchid);
  free(config->global.touchid_fp);
  free(config->global.tool_root);

  for (i = 0; i < config->default_count; i++) {
    bondage_free_profile(&config->defaults[i]);
  }

  for (i = 0; i < config->profile_count; i++) {
    bondage_free_profile(&config->profiles[i]);
  }

  free(config->defaults);
  free(config->profiles);
  memset(config, 0, sizeof(*config));
}

int
bondage_config_load(const char *path, struct bondage_config *config,
                    char *errbuf, size_t errbufsz)
{
  FILE *fp;
  char line[4096];
  unsigned long lineno = 0;
  enum bondage_section section = BONDAGE_SECTION_NONE;
  struct bondage_profile *current_defaults = NULL;
  struct bondage_profile *current_profile = NULL;

  fp = fopen(path, "r");
  if (fp == NULL) {
    bondage_set_error(errbuf, errbufsz, "cannot open %s", path);
    return 0;
  }

  while (fgets(line, sizeof(line), fp) != NULL) {
    char *cursor;
    char *eq;
    char *key;
    char *raw_value;
    char *value;

    lineno++;
    cursor = bondage_trim(line);

    if (*cursor == '\0' || *cursor == '#') continue;

    if (*cursor == '[') {
      size_t len = strlen(cursor);
      char *namebuf;
      char *defaults_name = NULL;
      char *profile_name = NULL;

      if (len < 3 || cursor[len - 1] != ']') {
        bondage_set_error(errbuf, errbufsz,
                          "%s:%lu: invalid section header", path, lineno);
        fclose(fp);
        return 0;
      }

      cursor[len - 1] = '\0';
      namebuf = bondage_trim(cursor + 1);

      if (strcmp(namebuf, "global") == 0) {
        section = BONDAGE_SECTION_GLOBAL;
        current_defaults = NULL;
        current_profile = NULL;
        continue;
      }

      if (bondage_parse_defaults_section(namebuf, &defaults_name)) {
        section = BONDAGE_SECTION_DEFAULTS;
        current_profile = NULL;
        if (!bondage_push_defaults(config, defaults_name, &current_defaults,
                                   errbuf, errbufsz)) {
          fclose(fp);
          return 0;
        }
        continue;
      }

      if (bondage_parse_profile_section(namebuf, &profile_name)) {
        section = BONDAGE_SECTION_PROFILE;
        current_defaults = NULL;
        if (!bondage_push_profile(config, profile_name, &current_profile,
                                  errbuf, errbufsz)) {
          fclose(fp);
          return 0;
        }
        continue;
      }

      bondage_set_error(errbuf, errbufsz,
                        "%s:%lu: unknown section '%s'", path, lineno, namebuf);
      fclose(fp);
      return 0;
    }

    eq = strchr(cursor, '=');
    if (eq == NULL) {
      bondage_set_error(errbuf, errbufsz,
                        "%s:%lu: expected key = value", path, lineno);
      fclose(fp);
      return 0;
    }

    *eq = '\0';
    key = bondage_trim(cursor);
    raw_value = eq + 1;
    if (!bondage_parse_value(raw_value, &value, errbuf, errbufsz)) {
      char parse_err[256];
      snprintf(parse_err, sizeof(parse_err), "%s", errbuf);
      bondage_set_error(errbuf, errbufsz,
                        "%s:%lu: %s", path, lineno, parse_err);
      fclose(fp);
      return 0;
    }

    if (section == BONDAGE_SECTION_GLOBAL) {
      if (!bondage_assign_global(&config->global, key, value, errbuf, errbufsz)) {
        char assign_err[256];
        snprintf(assign_err, sizeof(assign_err), "%s", errbuf);
        bondage_set_error(errbuf, errbufsz,
                          "%s:%lu: %s", path, lineno, assign_err);
        fclose(fp);
        return 0;
      }
      continue;
    }

    if (section == BONDAGE_SECTION_DEFAULTS && current_defaults != NULL) {
      if (!bondage_assign_profile(current_defaults, key, value, 0,
                                  errbuf, errbufsz)) {
        char assign_err[256];
        snprintf(assign_err, sizeof(assign_err), "%s", errbuf);
        bondage_set_error(errbuf, errbufsz,
                          "%s:%lu: %s", path, lineno, assign_err);
        fclose(fp);
        return 0;
      }
      continue;
    }

    if (section == BONDAGE_SECTION_PROFILE && current_profile != NULL) {
      if (!bondage_assign_profile(current_profile, key, value, 1,
                                  errbuf, errbufsz)) {
        char assign_err[256];
        snprintf(assign_err, sizeof(assign_err), "%s", errbuf);
        bondage_set_error(errbuf, errbufsz,
                          "%s:%lu: %s", path, lineno, assign_err);
        fclose(fp);
        return 0;
      }
      continue;
    }

    bondage_set_error(errbuf, errbufsz,
                      "%s:%lu: key outside a section", path, lineno);
    fclose(fp);
    return 0;
  }

  fclose(fp);

  if (!bondage_resolve_profiles(config, errbuf, errbufsz)) {
    return 0;
  }

  if (!bondage_validate_config(config, errbuf, errbufsz)) {
    return 0;
  }

  return 1;
}

const struct bondage_profile *
bondage_config_find_profile(const struct bondage_config *config,
                            const char *name)
{
  size_t i;

  for (i = 0; i < config->profile_count; i++) {
    if (config->profiles[i].name != NULL &&
        strcmp(config->profiles[i].name, name) == 0) {
      return &config->profiles[i];
    }
  }

  return NULL;
}

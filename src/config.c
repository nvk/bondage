#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

enum bondage_section {
  BONDAGE_SECTION_NONE = 0,
  BONDAGE_SECTION_GLOBAL,
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
bondage_free_profile(struct bondage_profile *profile)
{
  free(profile->name);
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
  bondage_free_string_list(&profile->nono_allow_files);
  bondage_free_string_list(&profile->nono_read_files);
  bondage_free_string_list(&profile->env_set);
  bondage_free_string_list(&profile->env_command);
  bondage_free_string_list(&profile->ensure_dir);
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
  profile->use_envchain = 1;
  profile->use_nono = 1;
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
                       const char *value, char *errbuf, size_t errbufsz)
{
  if (strcmp(key, "use_envchain") == 0) {
    return bondage_parse_bool(value, &profile->use_envchain, errbuf, errbufsz);
  }
  if (strcmp(key, "use_nono") == 0) {
    return bondage_parse_bool(value, &profile->use_nono, errbuf, errbufsz);
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
    return bondage_parse_bool(value, &profile->nono_allow_cwd, errbuf, errbufsz);
  }
  if (strcmp(key, "nono_allow_file") == 0) {
    return bondage_string_list_append(&profile->nono_allow_files, value,
                                      errbuf, errbufsz);
  }
  if (strcmp(key, "nono_read_file") == 0) {
    return bondage_string_list_append(&profile->nono_read_files, value,
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

  for (i = 0; i < config->profile_count; i++) {
    bondage_free_profile(&config->profiles[i]);
  }

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
        current_profile = NULL;
        continue;
      }

      if (bondage_parse_profile_section(namebuf, &profile_name)) {
        section = BONDAGE_SECTION_PROFILE;
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

    if (section == BONDAGE_SECTION_PROFILE && current_profile != NULL) {
      if (!bondage_assign_profile(current_profile, key, value, errbuf, errbufsz)) {
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

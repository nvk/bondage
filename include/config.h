#ifndef BONDAGE_CONFIG_H
#define BONDAGE_CONFIG_H

#include <stddef.h>

struct bondage_string_list {
  char **items;
  size_t count;
};

enum bondage_owner_kind {
  BONDAGE_OWNER_NONE = 0,
  BONDAGE_OWNER_DEFAULT,
  BONDAGE_OWNER_PROFILE
};

struct bondage_field_owner {
  enum bondage_owner_kind kind;
  char *name;
};

struct bondage_global {
  char *envchain;
  char *envchain_fp;
  char *nono;
  char *nono_fp;
  char *nono_profile_root;
  char *touchid;
  char *touchid_fp;
  char *tool_root;
};

struct bondage_profile {
  char *name;
  struct bondage_string_list inherits;
  char *namespace_name;
  char *nono_profile;
  char *touch_policy;
  char *target_kind;
  char *target;
  char *target_fp;
  char *interpreter;
  char *interpreter_fp;
  char *package_root;
  char *package_tree_fp;
  int use_envchain;
  int use_nono;
  int nono_allow_cwd;
  int use_envchain_set;
  int use_nono_set;
  int nono_allow_cwd_set;
  struct bondage_string_list nono_allow_files;
  struct bondage_string_list nono_read_files;
  struct bondage_string_list target_args;
  struct bondage_string_list env_set;
  struct bondage_string_list env_command;
  struct bondage_string_list ensure_dir;
  struct bondage_field_owner target_owner;
  struct bondage_field_owner target_fp_owner;
  struct bondage_field_owner interpreter_owner;
  struct bondage_field_owner interpreter_fp_owner;
  struct bondage_field_owner package_root_owner;
  struct bondage_field_owner package_tree_fp_owner;
};

struct bondage_config {
  struct bondage_global global;
  struct bondage_profile *defaults;
  size_t default_count;
  struct bondage_profile *profiles;
  size_t profile_count;
};

void
bondage_config_init(struct bondage_config *config);

void
bondage_config_free(struct bondage_config *config);

int
bondage_config_load(const char *path, struct bondage_config *config,
                    char *errbuf, size_t errbufsz);

const struct bondage_profile *
bondage_config_find_profile(const struct bondage_config *config,
                            const char *name);

#endif

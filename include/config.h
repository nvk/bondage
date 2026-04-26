#ifndef BONDAGE_CONFIG_H
#define BONDAGE_CONFIG_H

#include <stddef.h>

struct bondage_string_list {
  char **items;
  size_t count;
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
  struct bondage_string_list nono_allow_files;
  struct bondage_string_list nono_read_files;
  struct bondage_string_list env_set;
  struct bondage_string_list env_command;
  struct bondage_string_list ensure_dir;
};

struct bondage_config {
  struct bondage_global global;
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

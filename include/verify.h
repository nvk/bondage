#ifndef BONDAGE_VERIFY_H
#define BONDAGE_VERIFY_H

#include <stdio.h>

#include "config.h"

int
bondage_hash_file_path(const char *path,
                       int require_exec,
                       char *out,
                       size_t outsz,
                       char *errbuf,
                       size_t errbufsz);

int
bondage_hash_tree_path(const char *path,
                       char *out,
                       size_t outsz,
                       char *errbuf,
                       size_t errbufsz);

int
bondage_verify_env_command(const struct bondage_profile *profile,
                           const char *name,
                           const char *path,
                           FILE *stream,
                           char *errbuf,
                           size_t errbufsz);

/* Verify pinned helpers without running them or fetching their output. */
int
bondage_verify_env_commands(const struct bondage_profile *profile,
                            FILE *stream,
                            char *errbuf,
                            size_t errbufsz);

int
bondage_verify_profile(const struct bondage_config *config,
                       const struct bondage_profile *profile,
                       FILE *stream,
                       char *errbuf,
                       size_t errbufsz);

void
bondage_format_verify_failure(const struct bondage_config *config,
                              const struct bondage_profile *profile,
                              const char *config_path,
                              const char *base_error,
                              char *out,
                              size_t outsz);

#endif

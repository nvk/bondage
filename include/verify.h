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
bondage_verify_profile(const struct bondage_config *config,
                       const struct bondage_profile *profile,
                       FILE *stream,
                       char *errbuf,
                       size_t errbufsz);

#endif

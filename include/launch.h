#ifndef BONDAGE_LAUNCH_H
#define BONDAGE_LAUNCH_H

#include <stdio.h>
#include <sys/types.h>

#include "config.h"

struct bondage_argv {
  char **argv;
  size_t argc;
  char **envp;
  size_t envc;
};

void
bondage_argv_free(struct bondage_argv *args);

int
bondage_build_argv(const struct bondage_config *config,
                   const struct bondage_profile *profile,
                   int passthrough_argc,
                   char **passthrough_argv,
                   struct bondage_argv *out,
                   char *errbuf,
                   size_t errbufsz);

void
bondage_print_argv(const struct bondage_argv *args, FILE *stream);

int
bondage_exec_argv(const struct bondage_argv *args,
                  char *errbuf,
                  size_t errbufsz);

int
bondage_prepare_exec(const struct bondage_config *config,
                     const struct bondage_profile *profile,
                     struct bondage_argv *args,
                     char *errbuf,
                     size_t errbufsz);

#endif

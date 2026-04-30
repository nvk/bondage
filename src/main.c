#include <stdio.h>
#include <string.h>

#include "config.h"
#include "launch.h"
#include "main.h"
#include "repin.h"
#include "verify.h"

static const char *
bondage_default_config_path(void)
{
  return "./bondage.conf";
}

static void
bondage_usage(const char *progname)
{
  fprintf(stderr,
          "Usage:\n"
          "  %s status [config]\n"
          "  %s doctor [config]\n"
          "  %s verify <profile> [config]\n"
          "  %s repin <profile> [config]\n"
          "  %s repin-globals [config]\n"
          "  %s argv <profile> [config] [-- args...]\n"
          "  %s exec <profile> [config] [-- args...]\n"
          "  %s hash-file <absolute-path>\n"
          "  %s hash-tree <absolute-path>\n",
          progname, progname, progname, progname, progname, progname, progname,
          progname, progname);
}

static int
bondage_load_profile(const char *profile_name, const char *config_path,
                     struct bondage_config *config,
                     const struct bondage_profile **profile_out)
{
  char errbuf[256];

  bondage_config_init(config);
  if (!bondage_config_load(config_path, config, errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: %s\n", errbuf);
    bondage_config_free(config);
    return 0;
  }

  *profile_out = bondage_config_find_profile(config, profile_name);
  if (*profile_out == NULL) {
    fprintf(stderr, "bondage: unknown profile '%s'\n", profile_name);
    bondage_config_free(config);
    return 0;
  }

  return 1;
}

static void
bondage_print_profile_header(const struct bondage_profile *profile)
{
  printf("profile: %s\n", profile->name);
  printf("use_envchain: %s\n", profile->use_envchain ? "true" : "false");
  if (profile->namespace_name != NULL) {
    printf("namespace: %s\n", profile->namespace_name);
  }
  printf("use_nono: %s\n", profile->use_nono ? "true" : "false");
  if (profile->nono_profile != NULL) {
    printf("nono_profile: %s\n", profile->nono_profile);
  }
  printf("touch_policy: %s\n", profile->touch_policy);
  printf("target_kind: %s\n", profile->target_kind);
  printf("target: %s\n", profile->target);
  if (profile->interpreter != NULL) {
    printf("interpreter: %s\n", profile->interpreter);
  }
  if (profile->package_root != NULL) {
    printf("package_root: %s\n", profile->package_root);
  }
}

static int
bondage_status(const char *config_path)
{
  struct bondage_config config;
  char errbuf[256];
  size_t i;

  bondage_config_init(&config);
  if (!bondage_config_load(config_path, &config, errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: %s\n", errbuf);
    bondage_config_free(&config);
    return 1;
  }

  printf("config: %s\n", config_path);
  printf("envchain: %s\n", config.global.envchain);
  printf("nono: %s\n", config.global.nono);
  printf("profiles: %lu\n", (unsigned long)config.profile_count);
  for (i = 0; i < config.profile_count; i++) {
    const struct bondage_profile *profile = &config.profiles[i];
    printf("- %s\n", profile->name);
    printf("  use_envchain: %s\n", profile->use_envchain ? "true" : "false");
    if (profile->namespace_name != NULL) {
      printf("  namespace: %s\n", profile->namespace_name);
    }
    printf("  use_nono: %s\n", profile->use_nono ? "true" : "false");
    if (profile->nono_profile != NULL) {
      printf("  nono_profile: %s\n", profile->nono_profile);
    }
    printf("  touch_policy: %s\n", profile->touch_policy);
    printf("  target_kind: %s\n", profile->target_kind);
    printf("  target: %s\n", profile->target);
    if (profile->interpreter != NULL) {
      printf("  interpreter: %s\n", profile->interpreter);
    }
    if (profile->package_root != NULL) {
      printf("  package_root: %s\n", profile->package_root);
    }
  }

  bondage_config_free(&config);
  return 0;
}

static int
bondage_verify(const char *profile_name, const char *config_path)
{
  struct bondage_config config;
  const struct bondage_profile *profile;
  char errbuf[256];
  char final_errbuf[1024];

  if (!bondage_load_profile(profile_name, config_path, &config, &profile)) {
    return 1;
  }
  bondage_print_profile_header(profile);

  if (!bondage_verify_profile(&config, profile, stdout, errbuf, sizeof(errbuf))) {
    bondage_format_verify_failure(&config, profile, config_path,
                                  errbuf, final_errbuf, sizeof(final_errbuf));
    fprintf(stderr, "bondage: %s\n", final_errbuf);
    bondage_config_free(&config);
    return 1;
  }

  printf("status: verified\n");

  bondage_config_free(&config);
  return 0;
}

static int
bondage_argv_or_exec(const char *profile_name, const char *config_path,
                     int passthrough_argc, char **passthrough_argv,
                     int do_exec)
{
  struct bondage_config config;
  const struct bondage_profile *profile;
  struct bondage_argv args;
  char errbuf[256];
  char final_errbuf[1024];

  if (!bondage_load_profile(profile_name, config_path, &config, &profile)) {
    return 1;
  }
  if (!do_exec) {
    bondage_print_profile_header(profile);
  }

  if (!bondage_verify_profile(&config, profile, do_exec ? NULL : stdout,
                              errbuf, sizeof(errbuf))) {
    bondage_format_verify_failure(&config, profile, config_path,
                                  errbuf, final_errbuf, sizeof(final_errbuf));
    fprintf(stderr, "bondage: %s\n", final_errbuf);
    bondage_config_free(&config);
    return 1;
  }

  if (!bondage_build_argv(&config, profile, passthrough_argc, passthrough_argv,
                          &args, errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: %s\n", errbuf);
    bondage_config_free(&config);
    return 1;
  }

  if (!do_exec) {
    bondage_print_argv(&args, stdout);
    bondage_argv_free(&args);
    bondage_config_free(&config);
    return 0;
  }

  if (!bondage_prepare_exec(&config, profile, &args, errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: %s\n", errbuf);
    bondage_argv_free(&args);
    bondage_config_free(&config);
    return 1;
  }

  if (!bondage_exec_argv(&args, errbuf, sizeof(errbuf))) {
    fprintf(stderr, "bondage: %s\n", errbuf);
    bondage_argv_free(&args);
    bondage_config_free(&config);
    return 1;
  }

  bondage_argv_free(&args);
  bondage_config_free(&config);
  return 0;
}

static int
bondage_hash_path(const char *path, int is_tree)
{
  char fp[256];
  char errbuf[256];
  int ok;

  if (is_tree) {
    ok = bondage_hash_tree_path(path, fp, sizeof(fp), errbuf, sizeof(errbuf));
  }
  else {
    ok = bondage_hash_file_path(path, 0, fp, sizeof(fp), errbuf, sizeof(errbuf));
  }

  if (!ok) {
    fprintf(stderr, "bondage: %s\n", errbuf);
    return 1;
  }

  printf("%s\n", fp);
  return 0;
}

int
bondage_main(int argc, char **argv)
{
  const char *config_path = bondage_default_config_path();
  char **passthrough_argv = NULL;
  int passthrough_argc = 0;
  int do_exec = 0;

  if (argc < 2) {
    bondage_usage(argv[0]);
    return 2;
  }

  if ((strcmp(argv[1], "argv") == 0 || strcmp(argv[1], "exec") == 0) &&
      argc >= 3) {
    do_exec = strcmp(argv[1], "exec") == 0 ? 1 : 0;
    if (argc >= 4 && strcmp(argv[3], "--") != 0) {
      config_path = argv[3];
      if (argc >= 5 && strcmp(argv[4], "--") == 0) {
        passthrough_argv = &argv[5];
        passthrough_argc = argc - 5;
      }
    }
    else if (argc >= 4 && strcmp(argv[3], "--") == 0) {
      passthrough_argv = &argv[4];
      passthrough_argc = argc - 4;
    }
    return bondage_argv_or_exec(argv[2], config_path,
                                passthrough_argc, passthrough_argv,
                                do_exec);
  }

  if (strcmp(argv[1], "status") == 0) {
    if (argc >= 3) config_path = argv[2];
    return bondage_status(config_path);
  }

  if (strcmp(argv[1], "doctor") == 0) {
    if (argc >= 3) config_path = argv[2];
    return bondage_doctor(config_path);
  }

  if (strcmp(argv[1], "verify") == 0) {
    if (argc < 3) {
      bondage_usage(argv[0]);
      return 2;
    }
    if (argc >= 4) config_path = argv[3];
    return bondage_verify(argv[2], config_path);
  }

  if (strcmp(argv[1], "repin") == 0) {
    if (argc < 3) {
      bondage_usage(argv[0]);
      return 2;
    }
    if (argc >= 4) config_path = argv[3];
    return bondage_repin(argv[2], config_path);
  }

  if (strcmp(argv[1], "repin-globals") == 0) {
    if (argc >= 3) config_path = argv[2];
    return bondage_repin_globals(config_path);
  }

  if (strcmp(argv[1], "hash-file") == 0) {
    if (argc != 3) {
      bondage_usage(argv[0]);
      return 2;
    }
    return bondage_hash_path(argv[2], 0);
  }

  if (strcmp(argv[1], "hash-tree") == 0) {
    if (argc != 3) {
      bondage_usage(argv[0]);
      return 2;
    }
    return bondage_hash_path(argv[2], 1);
  }

  bondage_usage(argv[0]);
  return 2;
}

int
main(int argc, char **argv)
{
  return bondage_main(argc, argv);
}

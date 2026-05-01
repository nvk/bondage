#include <limits.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "launch.h"
#include "main.h"
#include "repin.h"
#include "verify.h"

static const char *
bondage_default_config_path(void)
{
  static char home_path[PATH_MAX];
  const char *override = getenv("BONDAGE_CONF");
  const char *home = getenv("HOME");
  int n;

  if (override != NULL && override[0] != '\0') return override;

  if (home != NULL && home[0] != '\0') {
    n = snprintf(home_path, sizeof(home_path),
                 "%s/.config/bondage/bondage.conf", home);
    if (n > 0 && (size_t)n < sizeof(home_path)) return home_path;
  }

  return "./bondage.conf";
}

enum bondage_command_kind {
  BONDAGE_CMD_STATUS,
  BONDAGE_CMD_DOCTOR,
  BONDAGE_CMD_VERIFY,
  BONDAGE_CMD_REPIN,
  BONDAGE_CMD_REPIN_GLOBALS,
  BONDAGE_CMD_ARGV,
  BONDAGE_CMD_EXEC,
  BONDAGE_CMD_HASH_FILE,
  BONDAGE_CMD_HASH_TREE
};

struct bondage_command {
  const char *name;
  enum bondage_command_kind kind;
};

struct bondage_cli_options {
  const char *config_path;
  int help;
  int command_index;
};

static const struct bondage_command bondage_commands[] = {
  {"status", BONDAGE_CMD_STATUS},
  {"doctor", BONDAGE_CMD_DOCTOR},
  {"verify", BONDAGE_CMD_VERIFY},
  {"repin", BONDAGE_CMD_REPIN},
  {"repin-globals", BONDAGE_CMD_REPIN_GLOBALS},
  {"argv", BONDAGE_CMD_ARGV},
  {"exec", BONDAGE_CMD_EXEC},
  {"hash-file", BONDAGE_CMD_HASH_FILE},
  {"hash-tree", BONDAGE_CMD_HASH_TREE},
  {NULL, BONDAGE_CMD_STATUS}
};

static void
bondage_usage(FILE *stream, const char *progname)
{
  fprintf(stream,
          "Usage:\n"
          "  %s [--config <path>] status [config]\n"
          "  %s [--config <path>] doctor [config]\n"
          "  %s [--config <path>] verify <profile> [config]\n"
          "  %s [--config <path>] repin <profile> [config]\n"
          "  %s [--config <path>] repin-globals [config]\n"
          "  %s [--config <path>] argv <profile> [config] [-- args...]\n"
          "  %s [--config <path>] exec <profile> [config] [-- args...]\n"
          "  %s hash-file <absolute-path>\n"
          "  %s hash-tree <absolute-path>\n"
          "\n"
          "Options:\n"
          "  -c, --config <path>  Read this bondage config file.\n"
          "  -h, --help           Show this help.\n",
          progname, progname, progname, progname, progname, progname, progname,
          progname, progname);
}

static const struct bondage_command *
bondage_find_command(const char *name)
{
  size_t i;

  for (i = 0; bondage_commands[i].name != NULL; i++) {
    if (strcmp(bondage_commands[i].name, name) == 0) return &bondage_commands[i];
  }

  return NULL;
}

static int
bondage_global_option_argc(int argc, char **argv)
{
  int i = 1;

  while (i < argc) {
    const char *arg = argv[i];

    if (strcmp(arg, "--") == 0) return i + 1;
    if (arg[0] != '-' || arg[1] == '\0') return i;

    if (strcmp(arg, "-c") == 0 || strcmp(arg, "--config") == 0) {
      if (i + 1 >= argc) return argc;
      i += 2;
      continue;
    }
    if (strncmp(arg, "--config=", 9) == 0) {
      i++;
      continue;
    }
    if (arg[0] == '-' && arg[1] == 'c' && arg[2] != '\0') {
      i++;
      continue;
    }
    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      i++;
      continue;
    }

    /* Let getopt_long report unknown leading options. */
    i++;
  }

  return i;
}

static int
bondage_parse_global_options(int argc, char **argv,
                             struct bondage_cli_options *options)
{
  static const struct option long_options[] = {
    {"config", required_argument, NULL, 'c'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0}
  };
  int parse_argc;
  int option;

  options->config_path = NULL;
  options->help = 0;
  options->command_index = 1;

  parse_argc = bondage_global_option_argc(argc, argv);
  optind = 1;
  opterr = 0;

  while ((option = getopt_long(parse_argc, argv, ":c:h", long_options, NULL)) != -1) {
    switch (option) {
    case 'c':
      options->config_path = optarg;
      break;
    case 'h':
      options->help = 1;
      break;
    case ':':
      fprintf(stderr, "bondage: option requires an argument: -%c\n", optopt);
      return 0;
    case '?':
    default:
      fprintf(stderr, "bondage: unknown option: %s\n", argv[optind - 1]);
      return 0;
    }
  }

  options->command_index = parse_argc;
  if (options->command_index < argc &&
      strcmp(argv[options->command_index], "--") == 0) {
    options->command_index++;
  }

  return 1;
}

static int
bondage_consume_optional_config(int argc, char **argv, int index,
                                const char *global_config,
                                const char **config_path)
{
  if (index < argc) {
    if (global_config != NULL) {
      fprintf(stderr,
              "bondage: config supplied both with --config and positional argument\n");
      return 0;
    }
    *config_path = argv[index];
    index++;
  }

  if (index != argc) {
    fprintf(stderr, "bondage: unexpected argument: %s\n", argv[index]);
    return 0;
  }

  return 1;
}

static int
bondage_parse_run_args(int argc, char **argv,
                       const char *global_config,
                       const char **profile_name,
                       const char **config_path,
                       int *passthrough_argc,
                       char ***passthrough_argv)
{
  int index = 0;

  if (argc < 1) return 0;

  *profile_name = argv[index++];
  *passthrough_argc = 0;
  *passthrough_argv = NULL;

  if (index < argc && strcmp(argv[index], "--") != 0) {
    if (global_config != NULL) {
      fprintf(stderr,
              "bondage: config supplied both with --config and positional argument\n");
      return 0;
    }
    *config_path = argv[index++];
  }

  if (index < argc) {
    if (strcmp(argv[index], "--") != 0) {
      fprintf(stderr, "bondage: unexpected argument: %s\n", argv[index]);
      return 0;
    }
    index++;
    *passthrough_argv = &argv[index];
    *passthrough_argc = argc - index;
  }

  return 1;
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
bondage_print_inherits(const struct bondage_profile *profile, const char *prefix)
{
  size_t i;

  if (profile->inherits.count == 0) return;

  printf("%sinherits:", prefix);
  for (i = 0; i < profile->inherits.count; i++) {
    printf("%s%s", i == 0 ? " " : ", ", profile->inherits.items[i]);
  }
  printf("\n");
}

static void
bondage_print_owner(const char *prefix,
                    const char *label,
                    const struct bondage_field_owner *owner)
{
  if (owner->kind == BONDAGE_OWNER_DEFAULT && owner->name != NULL) {
    printf("%s%s_owner: defaults \"%s\"\n", prefix, label, owner->name);
  }
  else if (owner->kind == BONDAGE_OWNER_PROFILE && owner->name != NULL) {
    printf("%s%s_owner: profile \"%s\"\n", prefix, label, owner->name);
  }
}

static void
bondage_print_string_list(const char *prefix,
                          const char *label,
                          const struct bondage_string_list *list)
{
  size_t i;

  for (i = 0; i < list->count; i++) {
    printf("%s%s: %s\n", prefix, label, list->items[i]);
  }
}

static void
bondage_print_profile_header(const struct bondage_profile *profile)
{
  printf("profile: %s\n", profile->name);
  bondage_print_inherits(profile, "");
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
  bondage_print_owner("", "target", &profile->target_owner);
  if (profile->interpreter != NULL) {
    printf("interpreter: %s\n", profile->interpreter);
    bondage_print_owner("", "interpreter", &profile->interpreter_owner);
  }
  if (profile->package_root != NULL) {
    printf("package_root: %s\n", profile->package_root);
    bondage_print_owner("", "package_root", &profile->package_root_owner);
  }
  bondage_print_string_list("", "target_arg", &profile->target_args);
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
  printf("defaults: %lu\n", (unsigned long)config.default_count);
  printf("profiles: %lu\n", (unsigned long)config.profile_count);
  for (i = 0; i < config.profile_count; i++) {
    const struct bondage_profile *profile = &config.profiles[i];
    printf("- %s\n", profile->name);
    bondage_print_inherits(profile, "  ");
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
    bondage_print_owner("  ", "target", &profile->target_owner);
    if (profile->interpreter != NULL) {
      printf("  interpreter: %s\n", profile->interpreter);
      bondage_print_owner("  ", "interpreter", &profile->interpreter_owner);
    }
    if (profile->package_root != NULL) {
      printf("  package_root: %s\n", profile->package_root);
      bondage_print_owner("  ", "package_root", &profile->package_root_owner);
    }
    bondage_print_string_list("  ", "target_arg", &profile->target_args);
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
  const struct bondage_command *command;
  struct bondage_cli_options options;
  const char *config_path;
  const char *profile_name;
  int command_argc;
  char **command_argv;
  char **passthrough_argv = NULL;
  int passthrough_argc = 0;

  if (!bondage_parse_global_options(argc, argv, &options)) {
    bondage_usage(stderr, argv[0]);
    return 2;
  }

  if (options.help) {
    bondage_usage(stdout, argv[0]);
    return 0;
  }

  if (options.command_index >= argc) {
    bondage_usage(stderr, argv[0]);
    return 2;
  }

  command = bondage_find_command(argv[options.command_index]);
  if (command == NULL) {
    fprintf(stderr, "bondage: unknown command: %s\n", argv[options.command_index]);
    bondage_usage(stderr, argv[0]);
    return 2;
  }

  config_path = options.config_path != NULL ?
                options.config_path : bondage_default_config_path();
  command_argc = argc - options.command_index - 1;
  command_argv = &argv[options.command_index + 1];

  switch (command->kind) {
  case BONDAGE_CMD_STATUS:
    if (!bondage_consume_optional_config(command_argc, command_argv, 0,
                                         options.config_path, &config_path)) {
      bondage_usage(stderr, argv[0]);
      return 2;
    }
    return bondage_status(config_path);

  case BONDAGE_CMD_DOCTOR:
    if (!bondage_consume_optional_config(command_argc, command_argv, 0,
                                         options.config_path, &config_path)) {
      bondage_usage(stderr, argv[0]);
      return 2;
    }
    return bondage_doctor(config_path);

  case BONDAGE_CMD_VERIFY:
    if (command_argc < 1) {
      bondage_usage(stderr, argv[0]);
      return 2;
    }
    profile_name = command_argv[0];
    if (!bondage_consume_optional_config(command_argc, command_argv, 1,
                                         options.config_path, &config_path)) {
      bondage_usage(stderr, argv[0]);
      return 2;
    }
    return bondage_verify(profile_name, config_path);

  case BONDAGE_CMD_REPIN:
    if (command_argc < 1) {
      bondage_usage(stderr, argv[0]);
      return 2;
    }
    profile_name = command_argv[0];
    if (!bondage_consume_optional_config(command_argc, command_argv, 1,
                                         options.config_path, &config_path)) {
      bondage_usage(stderr, argv[0]);
      return 2;
    }
    return bondage_repin(profile_name, config_path);

  case BONDAGE_CMD_REPIN_GLOBALS:
    if (!bondage_consume_optional_config(command_argc, command_argv, 0,
                                         options.config_path, &config_path)) {
      bondage_usage(stderr, argv[0]);
      return 2;
    }
    return bondage_repin_globals(config_path);

  case BONDAGE_CMD_ARGV:
  case BONDAGE_CMD_EXEC:
    if (!bondage_parse_run_args(command_argc, command_argv,
                                options.config_path, &profile_name,
                                &config_path, &passthrough_argc,
                                &passthrough_argv)) {
      bondage_usage(stderr, argv[0]);
      return 2;
    }
    return bondage_argv_or_exec(profile_name, config_path,
                                passthrough_argc, passthrough_argv,
                                command->kind == BONDAGE_CMD_EXEC);

  case BONDAGE_CMD_HASH_FILE:
    if (command_argc != 1) {
      bondage_usage(stderr, argv[0]);
      return 2;
    }
    return bondage_hash_path(command_argv[0], 0);

  case BONDAGE_CMD_HASH_TREE:
    if (command_argc != 1) {
      bondage_usage(stderr, argv[0]);
      return 2;
    }
    return bondage_hash_path(command_argv[0], 1);
  }

  bondage_usage(stderr, argv[0]);
  return 2;
}

int
main(int argc, char **argv)
{
  return bondage_main(argc, argv);
}

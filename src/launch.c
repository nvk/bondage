#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "launch.h"
#include "verify.h"

extern char **environ;

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

static char *
bondage_join_path2(const char *left, const char *right)
{
  size_t left_len = strlen(left);
  size_t right_len = strlen(right);
  char *joined = malloc(left_len + 1 + right_len + 1);

  if (joined == NULL) return NULL;
  memcpy(joined, left, left_len);
  joined[left_len] = '/';
  memcpy(joined + left_len + 1, right, right_len + 1);
  return joined;
}

static int
bondage_assignment_key_len(const char *assignment)
{
  const char *eq = strchr(assignment, '=');
  if (eq == NULL || eq == assignment) return 0;
  return (int)(eq - assignment);
}

static int
bondage_path_mkdirs(const char *path, char *errbuf, size_t errbufsz)
{
  char *tmp;
  char *cursor;

  if (path == NULL || path[0] != '/') {
    bondage_set_error(errbuf, errbufsz, "ensure_dir must be absolute");
    return 0;
  }

  tmp = bondage_xstrdup(path);
  if (tmp == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  for (cursor = tmp + 1; *cursor != '\0'; cursor++) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    if (mkdir(tmp, 0700) < 0 && errno != EEXIST) {
      bondage_set_error(errbuf, errbufsz, "mkdir failed for %s: %s",
                        tmp, strerror(errno));
      free(tmp);
      return 0;
    }
    *cursor = '/';
  }

  if (mkdir(tmp, 0700) < 0 && errno != EEXIST) {
    bondage_set_error(errbuf, errbufsz, "mkdir failed for %s: %s",
                      tmp, strerror(errno));
    free(tmp);
    return 0;
  }

  free(tmp);
  return 1;
}

static int
bondage_verify_touch_helper(const struct bondage_config *config,
                            char *errbuf,
                            size_t errbufsz)
{
  char actual_fp[256];

  if (config->global.touchid == NULL || config->global.touchid_fp == NULL) {
    bondage_set_error(errbuf, errbufsz, "touchid/touchid_fp are not configured");
    return 0;
  }

  if (!bondage_hash_file_path(config->global.touchid, 1,
                              actual_fp, sizeof(actual_fp),
                              errbuf, errbufsz)) {
    return 0;
  }

  if (strcmp(actual_fp, config->global.touchid_fp) != 0) {
    bondage_set_error(errbuf, errbufsz,
                      "touchid fingerprint mismatch: expected=%s actual=%s",
                      config->global.touchid_fp, actual_fp);
    return 0;
  }

  return 1;
}

static int
bondage_run_touch_helper(const struct bondage_config *config,
                         char *errbuf,
                         size_t errbufsz)
{
  pid_t pid;
  int status;
  char *argv[2];

  argv[0] = config->global.touchid;
  argv[1] = NULL;

  pid = fork();
  if (pid < 0) {
    bondage_set_error(errbuf, errbufsz, "fork failed: %s", strerror(errno));
    return 0;
  }

  if (pid == 0) {
    execve(argv[0], argv, environ);
    _exit(127);
  }

  if (waitpid(pid, &status, 0) < 0) {
    bondage_set_error(errbuf, errbufsz, "waitpid failed: %s", strerror(errno));
    return 0;
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    bondage_set_error(errbuf, errbufsz, "touchid helper denied or failed");
    return 0;
  }

  return 1;
}

static int
bondage_env_find(const char *const *envp, size_t envc, const char *key, size_t key_len)
{
  size_t i;

  for (i = 0; i < envc; i++) {
    if (strncmp(envp[i], key, key_len) == 0 && envp[i][key_len] == '=') {
      return (int)i;
    }
  }

  return -1;
}

static int
bondage_env_append_or_replace(char ***envp,
                              size_t *envc,
                              const char *assignment,
                              char *errbuf,
                              size_t errbufsz)
{
  int key_len;
  int idx;
  char *copy;
  char **grown;

  key_len = bondage_assignment_key_len(assignment);
  if (key_len <= 0) {
    bondage_set_error(errbuf, errbufsz, "env assignment requires NAME=value");
    return 0;
  }

  copy = bondage_xstrdup(assignment);
  if (copy == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  idx = bondage_env_find((const char *const *)*envp, *envc, assignment, (size_t)key_len);
  if (idx >= 0) {
    free((*envp)[idx]);
    (*envp)[idx] = copy;
    return 1;
  }

  grown = realloc(*envp, sizeof(char *) * (*envc + 2));
  if (grown == NULL) {
    free(copy);
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  *envp = grown;
  (*envp)[(*envc)++] = copy;
  (*envp)[*envc] = NULL;
  return 1;
}

static void
bondage_free_envp(char **envp, size_t envc)
{
  size_t i;

  if (envp == NULL) return;
  for (i = 0; i < envc; i++) {
    free(envp[i]);
  }
  free(envp);
}

static int
bondage_split_command(const char *value,
                      char ***argv_out,
                      size_t *argc_out,
                      char *errbuf,
                      size_t errbufsz)
{
  char **argv = NULL;
  size_t argc = 0;
  const char *cursor = value;
  int ok = 0;

  while (*cursor != '\0') {
    const char *start;
    const char *end;
    char *token;
    char **grown;

    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (*cursor == '\0') break;

    start = cursor;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') cursor++;
    end = cursor;

    token = bondage_xstrndup(start, (size_t)(end - start));
    if (token == NULL) {
      bondage_set_error(errbuf, errbufsz, "out of memory");
      goto cleanup;
    }

    grown = realloc(argv, sizeof(char *) * (argc + 2));
    if (grown == NULL) {
      free(token);
      bondage_set_error(errbuf, errbufsz, "out of memory");
      goto cleanup;
    }
    argv = grown;
    argv[argc++] = token;
    argv[argc] = NULL;
  }

  if (argc == 0) {
    bondage_set_error(errbuf, errbufsz, "env_command requires a command");
    goto cleanup;
  }
  if (argv[0][0] != '/') {
    bondage_set_error(errbuf, errbufsz, "env_command must use an absolute command path");
    goto cleanup;
  }

  *argv_out = argv;
  *argc_out = argc;
  ok = 1;

cleanup:
  if (!ok) {
    size_t i;
    for (i = 0; i < argc; i++) free(argv[i]);
    free(argv);
  }
  return ok;
}

static void
bondage_free_argv_list(char **argv, size_t argc)
{
  size_t i;

  for (i = 0; i < argc; i++) {
    free(argv[i]);
  }
  free(argv);
}

static int
bondage_resolve_nono_profile_arg(const struct bondage_config *config,
                                 const struct bondage_profile *profile,
                                 char **out,
                                 char *errbuf,
                                 size_t errbufsz)
{
  char *name_json = NULL;
  char *joined = NULL;
  size_t len;

  if (profile->nono_profile == NULL) {
    bondage_set_error(errbuf, errbufsz, "missing nono_profile");
    return 0;
  }

  if (profile->nono_profile[0] == '/') {
    *out = bondage_xstrdup(profile->nono_profile);
    if (*out == NULL) {
      bondage_set_error(errbuf, errbufsz, "out of memory");
      return 0;
    }
    return 1;
  }

  if (config->global.nono_profile_root == NULL) {
    *out = bondage_xstrdup(profile->nono_profile);
    if (*out == NULL) {
      bondage_set_error(errbuf, errbufsz, "out of memory");
      return 0;
    }
    return 1;
  }

  len = strlen(profile->nono_profile) + strlen(".json") + 1;
  name_json = malloc(len);
  if (name_json == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }
  snprintf(name_json, len, "%s.json", profile->nono_profile);

  joined = bondage_join_path2(config->global.nono_profile_root, name_json);
  free(name_json);
  if (joined == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  *out = joined;
  return 1;
}

static int
bondage_capture_command_output(const char *command_spec,
                               char **value_out,
                               char *errbuf,
                               size_t errbufsz)
{
  char **argv = NULL;
  size_t argc = 0;
  int pipefd[2];
  pid_t pid;
  char *buffer = NULL;
  size_t used = 0;
  size_t cap = 0;
  int status;
  int ok = 0;

  if (!bondage_split_command(command_spec, &argv, &argc, errbuf, errbufsz)) {
    return 0;
  }

  if (pipe(pipefd) < 0) {
    bondage_set_error(errbuf, errbufsz, "pipe failed: %s", strerror(errno));
    goto cleanup;
  }

  pid = fork();
  if (pid < 0) {
    bondage_set_error(errbuf, errbufsz, "fork failed: %s", strerror(errno));
    goto cleanup_pipe;
  }

  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    dup2(pipefd[1], STDOUT_FILENO);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    close(pipefd[0]);
    close(pipefd[1]);
    execve(argv[0], argv, environ);
    _exit(127);
  }

  close(pipefd[1]);
  pipefd[1] = -1;

  for (;;) {
    char chunk[1024];
    ssize_t nread = read(pipefd[0], chunk, sizeof(chunk));
    char *grown;

    if (nread == 0) break;
    if (nread < 0) {
      bondage_set_error(errbuf, errbufsz, "read failed: %s", strerror(errno));
      goto cleanup_pipe;
    }

    if (used + (size_t)nread + 1 > cap) {
      cap = (used + (size_t)nread + 1) * 2;
      grown = realloc(buffer, cap);
      if (grown == NULL) {
        bondage_set_error(errbuf, errbufsz, "out of memory");
        goto cleanup_pipe;
      }
      buffer = grown;
    }

    memcpy(buffer + used, chunk, (size_t)nread);
    used += (size_t)nread;
    buffer[used] = '\0';
  }

  if (waitpid(pid, &status, 0) < 0) {
    bondage_set_error(errbuf, errbufsz, "waitpid failed: %s", strerror(errno));
    goto cleanup_pipe;
  }

  if (buffer == NULL) {
    buffer = bondage_xstrdup("");
    if (buffer == NULL) {
      bondage_set_error(errbuf, errbufsz, "out of memory");
      goto cleanup_pipe;
    }
  }

  while (used > 0 &&
         (buffer[used - 1] == '\n' || buffer[used - 1] == '\r' ||
          buffer[used - 1] == ' ' || buffer[used - 1] == '\t')) {
    buffer[--used] = '\0';
  }

  *value_out = buffer;
  buffer = NULL;
  ok = 1;

cleanup_pipe:
  if (pipefd[0] >= 0) close(pipefd[0]);
  if (pipefd[1] >= 0) close(pipefd[1]);
cleanup:
  free(buffer);
  bondage_free_argv_list(argv, argc);
  return ok;
}

static int
bondage_build_envp(const struct bondage_profile *profile,
                   char ***envp_out,
                   size_t *envc_out,
                   char *errbuf,
                   size_t errbufsz)
{
  char **envp = NULL;
  size_t envc = 0;
  size_t i;
  int ok = 0;

  for (i = 0; environ[i] != NULL; i++) {
    char **grown = realloc(envp, sizeof(char *) * (envc + 2));
    if (grown == NULL) {
      bondage_set_error(errbuf, errbufsz, "out of memory");
      goto cleanup;
    }
    envp = grown;
    envp[envc] = bondage_xstrdup(environ[i]);
    if (envp[envc] == NULL) {
      bondage_set_error(errbuf, errbufsz, "out of memory");
      goto cleanup;
    }
    envc++;
    envp[envc] = NULL;
  }

  for (i = 0; i < profile->env_set.count; i++) {
    if (!bondage_env_append_or_replace(&envp, &envc, profile->env_set.items[i],
                                       errbuf, errbufsz)) {
      goto cleanup;
    }
  }

  for (i = 0; i < profile->env_command.count; i++) {
    const char *spec = profile->env_command.items[i];
    const char *eq = strchr(spec, '=');
    char *name = bondage_xstrndup(spec, (size_t)(eq - spec));
    char *value = NULL;
    char *assignment = NULL;
    size_t len;

    if (name == NULL) {
      bondage_set_error(errbuf, errbufsz, "out of memory");
      goto cleanup;
    }

    if (!bondage_capture_command_output(eq + 1, &value, errbuf, errbufsz)) {
      free(name);
      goto cleanup;
    }

    len = strlen(name) + 1 + strlen(value) + 1;
    assignment = malloc(len);
    if (assignment == NULL) {
      free(name);
      free(value);
      bondage_set_error(errbuf, errbufsz, "out of memory");
      goto cleanup;
    }

    snprintf(assignment, len, "%s=%s", name, value);
    free(name);
    free(value);

    if (!bondage_env_append_or_replace(&envp, &envc, assignment,
                                       errbuf, errbufsz)) {
      free(assignment);
      goto cleanup;
    }
    free(assignment);
  }

  *envp_out = envp;
  *envc_out = envc;
  ok = 1;

cleanup:
  if (!ok) bondage_free_envp(envp, envc);
  return ok;
}

void
bondage_argv_free(struct bondage_argv *args)
{
  size_t i;

  if (args == NULL) return;

  if (args->argv != NULL) {
    for (i = 0; i < args->argc; i++) {
      free(args->argv[i]);
    }
    free(args->argv);
  }

  bondage_free_envp(args->envp, args->envc);
  args->argv = NULL;
  args->argc = 0;
  args->envp = NULL;
  args->envc = 0;
}

int
bondage_build_argv(const struct bondage_config *config,
                   const struct bondage_profile *profile,
                   int passthrough_argc,
                   char **passthrough_argv,
                   struct bondage_argv *out,
                   char *errbuf,
                   size_t errbufsz)
{
  size_t fixed_argc = 0;
  size_t argc;
  size_t i;
  char **argv;
  size_t cursor = 0;
  const char *runner;
  const char *target0 = NULL;
  char *nono_profile_arg = NULL;

  memset(out, 0, sizeof(*out));

  if (strcmp(profile->target_kind, "native") == 0) {
    runner = profile->target;
  }
  else if (strcmp(profile->target_kind, "script") == 0) {
    runner = profile->interpreter;
    target0 = profile->target;
  }
  else {
    bondage_set_error(errbuf, errbufsz,
                      "cannot build argv for unknown target_kind '%s'",
                      profile->target_kind);
    return 0;
  }

  if (profile->use_envchain) fixed_argc += 2;
  if (profile->use_nono) {
    fixed_argc += 5;
    if (profile->nono_allow_cwd) fixed_argc += 1;
    fixed_argc += profile->nono_allow_files.count * 2;
    fixed_argc += profile->nono_read_files.count * 2;
  }
  fixed_argc += 1;
  if (target0 != NULL) fixed_argc += 1;
  fixed_argc += profile->target_args.count;

  argc = fixed_argc + (size_t)passthrough_argc;
  argv = calloc(argc + 1, sizeof(char *));
  if (argv == NULL) {
    bondage_set_error(errbuf, errbufsz, "out of memory");
    return 0;
  }

  if (profile->use_envchain) {
    argv[cursor++] = bondage_xstrdup(config->global.envchain);
    argv[cursor++] = bondage_xstrdup(profile->namespace_name);
  }

  if (profile->use_nono) {
    if (!bondage_resolve_nono_profile_arg(config, profile, &nono_profile_arg,
                                          errbuf, errbufsz)) {
      free(argv);
      return 0;
    }
    argv[cursor++] = bondage_xstrdup(config->global.nono);
    argv[cursor++] = bondage_xstrdup("wrap");
    argv[cursor++] = bondage_xstrdup("--profile");
    argv[cursor++] = nono_profile_arg;
    nono_profile_arg = NULL;
    if (profile->nono_allow_cwd) {
      argv[cursor++] = bondage_xstrdup("--allow-cwd");
    }
    for (i = 0; i < profile->nono_allow_files.count; i++) {
      argv[cursor++] = bondage_xstrdup("--allow-file");
      argv[cursor++] = bondage_xstrdup(profile->nono_allow_files.items[i]);
    }
    for (i = 0; i < profile->nono_read_files.count; i++) {
      argv[cursor++] = bondage_xstrdup("--read-file");
      argv[cursor++] = bondage_xstrdup(profile->nono_read_files.items[i]);
    }
    argv[cursor++] = bondage_xstrdup("--");
  }

  argv[cursor++] = bondage_xstrdup(runner);
  if (target0 != NULL) {
    argv[cursor++] = bondage_xstrdup(target0);
  }
  for (i = 0; i < profile->target_args.count; i++) {
    argv[cursor++] = bondage_xstrdup(profile->target_args.items[i]);
  }

  for (i = 0; i < (size_t)passthrough_argc; i++) {
    argv[cursor++] = bondage_xstrdup(passthrough_argv[i]);
  }

  for (i = 0; i < cursor; i++) {
    if (argv[i] == NULL) {
      bondage_set_error(errbuf, errbufsz, "out of memory");
      out->argv = argv;
      out->argc = cursor;
      bondage_argv_free(out);
      free(nono_profile_arg);
      return 0;
    }
  }

  out->argv = argv;
  out->argc = cursor;
  free(nono_profile_arg);
  return 1;
}

void
bondage_print_argv(const struct bondage_argv *args, FILE *stream)
{
  size_t i;

  for (i = 0; i < args->argc; i++) {
    fprintf(stream, "argv[%lu] = %s\n",
            (unsigned long)i, args->argv[i]);
  }
}

int
bondage_prepare_exec(const struct bondage_config *config,
                     const struct bondage_profile *profile,
                     struct bondage_argv *args,
                     char *errbuf,
                     size_t errbufsz)
{
  size_t i;

  if (strcmp(profile->touch_policy, "prompt") == 0) {
    if (!bondage_verify_touch_helper(config, errbuf, errbufsz)) {
      return 0;
    }
    if (!bondage_run_touch_helper(config, errbuf, errbufsz)) {
      return 0;
    }
  }

  for (i = 0; i < profile->ensure_dir.count; i++) {
    if (!bondage_path_mkdirs(profile->ensure_dir.items[i], errbuf, errbufsz)) {
      return 0;
    }
  }

  if (!bondage_build_envp(profile, &args->envp, &args->envc, errbuf, errbufsz)) {
    return 0;
  }

  return 1;
}

int
bondage_exec_argv(const struct bondage_argv *args,
                  char *errbuf,
                  size_t errbufsz)
{
  char **envp = args->envp != NULL ? args->envp : environ;

  fflush(NULL);
  execve(args->argv[0], args->argv, envp);
  bondage_set_error(errbuf, errbufsz, "execve failed for %s: %s",
                    args->argv[0], strerror(errno));
  return 0;
}

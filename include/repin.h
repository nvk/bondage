#ifndef BONDAGE_REPIN_H
#define BONDAGE_REPIN_H

int
bondage_repin(const char *profile_name, const char *config_path);

int
bondage_repin_globals(const char *config_path);

int
bondage_doctor(const char *config_path);

/*
 * Interactive batch repair: analyze the whole config, present every pending
 * repin, and apply them after an explicit typed confirmation on a TTY.
 * Refuses non-interactive use and refuses same-version fingerprint changes.
 */
int
bondage_repair(const char *config_path);

#endif

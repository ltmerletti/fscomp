#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "constants.h"
#include "db.h"
#include "report.h"
#include "scanner.h"

#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FSCOMP_VERSION "1.0.0"

/**
 * Prints program usage instructions, commands, and option flags to stdout.
 */
static void print_usage(const char *program_name) {
  printf("   __                                \n"
         "  / _|___  ___ ___  _ __ ___  _ __   \n"
         " | |_/ __|/ __/ _ \\\\| '_ ` _ \\\\| '_ \\\\ \n"
         " |  _\\\\__ \\\\ (_| (_) | | | | | | |_) |\n"
         " |_| |___/\\\\___\\\\___/|_| |_| |_| .__/ \n"
         "                             |_|     "
         "\n\n");
  printf("fscomp %s, transparent filesystem compression manager\n\n", FSCOMP_VERSION);
  printf("Usage:\n");
  printf("  %s <command> [options] [targets...]\n\n", program_name);
  printf("Commands:\n");
  printf("  add <directories...>    Add directory paths to the tracked list\n");
  printf("  remove <directories...> Remove directory paths from the tracked list\n");
  printf("  list                    Show all tracked directory paths\n");
  printf("  update                  Compress all tracked directory paths\n");
  printf("  scan [targets...]       Show files eligible for compression without writing\n");
  printf("  run [targets...]        Compress targets, or tracked list if none given\n");
  printf("  status                  Print summary of space saved across tracked files\n");
  printf("  report                  List savings by file extension and top saved files\n");
  printf("  config [show|init]      Print current settings or create a starter config\n\n");
  printf("Options:\n");
  printf("  -c, --config <path>     Config file path, default ~/.config/fscomp/config.json\n");
  printf("  -d, --db <path>         State database path, default ~/.config/fscomp/state.db\n");
  printf("  -T, --compressor <type> Compression method, LZFSE default, LZVN, or ZLIB\n");
  printf("  -r, --rehash-all        Verify sha256 for all files instead of checking mtime\n");
  printf("  -v, --verbose           Print each file path as it is processed\n");
  printf("  -q, --quiet             Print summary only without live progress or file notices\n");
  printf("  -s, --silent            Suppress all output\n");
  printf("  -j, --jobs <count>      Override configured worker count, 1-64\n");
  printf("  -h, --help              Show this help text\n");
  printf("  -V, --version           Print version\n");
}

/**
 * Returns true when an argument abbreviates the --jobs option.
 */
static bool is_abbreviated_jobs_option(const char *argument) {
  if (!argument || strncmp(argument, "--", 2) != 0) return false;

  size_t argument_name_length = strcspn(argument, "=");
  size_t jobs_option_length = strlen("--jobs");
  return argument_name_length > 2 && argument_name_length < jobs_option_length
      && strncmp(argument, "--jobs", argument_name_length) == 0;
}

/**
 * Modifies the tracked targets list by adding or removing directory paths.
 */
static int handle_target_mutation(config_t *config, const char *config_path, char **targets, size_t count,
                                  bool is_add) {
  if (count == 0) {
    fprintf(stderr, "Error: specify at least one directory path to %s.\n", is_add ? "add" : "remove");
    fprintf(stderr, "Example: fscomp %s ~/Documents\n", is_add ? "add" : "remove");
    return 1;
  }

  size_t changed_count = 0;
  for (size_t i = 0; i < count; i++) {
    if (is_add) {
      if (config_add_target(config, targets[i])) {
        printf("Added: %s\n", config->targets[config->target_count - 1]);
        changed_count++;
      } else {
        printf("Already tracked or duplicate: %s\n", targets[i]);
      }
    } else if (config_remove_target(config, targets[i])) {
      printf("Removed: %s\n", targets[i]);
      changed_count++;
    } else {
      printf("Not found in tracked list: %s\n", targets[i]);
    }
  }

  if (changed_count > 0) {
    char *save_path = config_path ? config_expand_path(config_path) : config_get_default_path();
    if (config_save(config, save_path)) {
      if (is_add) {
        printf("Saved %zu tracked director%s to config.\n", config->target_count,
               config->target_count == 1 ? "y" : "ies");
      } else {
        printf("Saved updated list (%zu director%s remaining).\n", config->target_count,
               config->target_count == 1 ? "y" : "ies");
      }
    } else {
      fprintf(stderr, "Warning: failed to save config file.\n");
    }
    free(save_path);
  }
  return 0;
}

/**
 * Entry point for the fscomp command-line utility.
 */
int main(int argument_count, char **arguments) {
  if (argument_count < 2) {
    print_usage(arguments[0]);
    return 1;
  }

  const char *config_path = NULL;
  const char *database_override_path = NULL;
  const char *compressor_override = NULL;
  bool rehash_all = false;
  bool verbose = false;
  bool quiet = false;
  bool silent = false;
  int worker_count = DEFAULT_WORKER_COUNT;
  bool worker_count_overridden = false;

  static struct option long_options[] = {
    {"config", required_argument, 0, 'c'},
    {"db", required_argument, 0, 'd'},
    {"compressor", required_argument, 0, 'T'},
    {"rehash-all", no_argument, 0, 'r'},
    {"verbose", no_argument, 0, 'v'},
    {"quiet", no_argument, 0, 'q'},
    {"silent", no_argument, 0, 's'},
    {"jobs", required_argument, 0, 'j'},
    {"help", no_argument, 0, 'h'},
    {"version", no_argument, 0, 'V'},
    {0, 0, 0, 0},
  };

  char *positional_arguments[argument_count];
  int positional_count = 0;

  for (int index = 1; index < argument_count && strcmp(arguments[index], "--") != 0; index++) {
    if (is_abbreviated_jobs_option(arguments[index])) {
      fprintf(stderr, "Error: unknown option '%s'. Use --jobs.\n", arguments[index]);
      return 1;
    }
  }

  optind = 1;
  int option_character;
  while ((option_character = getopt_long(argument_count, arguments, "c:d:T:rvqsj:hV", long_options, NULL)) != -1) {
    switch (option_character) {
    case 'c': config_path = optarg; break;
    case 'd': database_override_path = optarg; break;
    case 'T': compressor_override = optarg; break;
    case 'r': rehash_all = true; break;
    case 'v': verbose = true; break;
    case 'q': quiet = true; break;
    case 's': silent = true; break;
    case 'j': {
      char *parse_end = NULL;
      long requested_worker_count = strtol(optarg, &parse_end, 10);
      if (optarg[0] == '\0' || parse_end[0] != '\0' || requested_worker_count < DEFAULT_WORKER_COUNT
          || requested_worker_count > MAXIMUM_WORKER_COUNT) {
        fprintf(stderr, "Error: --jobs must be between %d and %d.\n", DEFAULT_WORKER_COUNT, MAXIMUM_WORKER_COUNT);
        return 1;
      }
      worker_count = (int)requested_worker_count;
      worker_count_overridden = true;
      break;
    }
    case 'h': print_usage(arguments[0]); return 0;
    case 'V': printf("fscomp version %s\n", FSCOMP_VERSION); return 0;
    default: print_usage(arguments[0]); return 1;
    }
  }

  while (optind < argument_count)
    positional_arguments[positional_count++] = arguments[optind++];

  if (positional_count == 0) {
    print_usage(arguments[0]);
    return 1;
  }

  const char *command = positional_arguments[0];
  const char *subcommand = (positional_count > 1) ? positional_arguments[1] : "";

  int argument_offset = 1;
  if (strcmp(command, "config") == 0 || strcmp(command, "target") == 0) {
    if (positional_count > 1) argument_offset = 2;
  }

  size_t cli_target_count = 0;
  char **cli_targets = NULL;
  if (positional_count > argument_offset) {
    cli_target_count = (size_t)(positional_count - argument_offset);
    cli_targets = malloc(cli_target_count * sizeof(char *));
    for (size_t i = 0; i < cli_target_count; i++)
      cli_targets[i] = config_expand_path(positional_arguments[argument_offset + i]);
  }

  config_t *config = config_load(config_path);
  if (!config) {
    fprintf(stderr, "Error: could not read configuration.\n");
    return 1;
  }

  if (!worker_count_overridden) worker_count = config->worker_count;

  if (compressor_override) {
    free(config->compressor);
    config->compressor = strdup(compressor_override);
  }
  if (database_override_path) {
    free(config->database_path);
    config->database_path = config_expand_path(database_override_path);
  }

  int exit_code = 0;

  /* Handle target management commands */
  if (strcmp(command, "add") == 0 || (strcmp(command, "target") == 0 && strcmp(subcommand, "add") == 0)) {
    exit_code = handle_target_mutation(config, config_path, cli_targets, cli_target_count, true);
  } else if (strcmp(command, "remove") == 0 || (strcmp(command, "target") == 0 && strcmp(subcommand, "remove") == 0)) {
    exit_code = handle_target_mutation(config, config_path, cli_targets, cli_target_count, false);
  }

  else if (strcmp(command, "list") == 0
           || (strcmp(command, "target") == 0 && (strcmp(subcommand, "list") == 0 || subcommand[0] == '\0'))) {
    if (config->target_count == 0) {
      printf("No tracked directories yet.\n");
      printf("Add directories with: fscomp add <path>\n");
    } else {
      printf("Tracked directories (%zu):\n", config->target_count);
      for (size_t i = 0; i < config->target_count; i++)
        printf("  %s\n", config->targets[i]);
    }
  }

  else if (strcmp(command, "update") == 0 || strcmp(command, "scan") == 0 || strcmp(command, "run") == 0) {
    bool is_dry_run = (strcmp(command, "scan") == 0);

    if (cli_target_count > 0) {
      if (config->targets) {
        for (size_t i = 0; i < config->target_count; i++)
          free(config->targets[i]);
        free(config->targets);
      }
      config->targets = cli_targets;
      config->target_count = cli_target_count;
    }

    if (config->target_count == 0) {
      fprintf(stderr, "No directories to process.\n");
      fprintf(stderr, "Add tracked directories with: fscomp add <path>\n");
      fprintf(stderr, "Or pass a path directly:      fscomp %s <path>\n", command);
      exit_code = 1;
    } else {
      db_t *database = NULL;
      if (!is_dry_run) {
        database = db_open(config->database_path);
        if (!database) {
          fprintf(stderr, "Error: could not open database at %s\n", config->database_path);
          exit_code = 1;
        }
      } else {
        if (config->database_path && access(config->database_path, R_OK) == 0)
          database = db_open(config->database_path);
      }

      if (exit_code == 0) {
        scan_options_t options = {
          .dry_run = is_dry_run,
          .rehash_all = rehash_all,
          .verbose = verbose,
          .quiet = quiet,
          .silent = silent,
          .worker_count = worker_count,
        };

        scan_statistics_t statistics;
        if (!silent && !quiet) {
          printf("Running fscomp %s using %s with %d worker%s on %zu target%s\n", is_dry_run ? "scan" : "run",
                 config->compressor ? config->compressor : "LZFSE", worker_count, worker_count == 1 ? "" : "s",
                 config->target_count, config->target_count == 1 ? "" : "s");
        }

        if (!scanner_execute(config, database, &options, &statistics)) exit_code = 1;
        if (!silent) report_print_scan_summary(&statistics, is_dry_run);
      }
      if (database) db_close(database);
    }
  } else if (strcmp(command, "status") == 0 || strcmp(command, "report") == 0) {
    db_t *database = db_open(config->database_path);
    if (!database) {
      fprintf(stderr, "Error: database not found at %s\n", config->database_path);
      exit_code = 1;
    } else {
      if (strcmp(command, "status") == 0) report_print_status(database);
      else report_print_report(database);
      db_close(database);
    }
  } else if (strcmp(command, "config") == 0) {
    if (strcmp(subcommand, "init") == 0) {
      char *default_path = config_get_default_path();
      if (config_save(config, default_path)) {
        printf("Saved default config to %s\n", default_path);
      } else {
        fprintf(stderr, "Could not write config to %s\n", default_path);
        exit_code = 1;
      }
      free(default_path);
    } else {
      printf("Active configuration:\n");
      printf("  Compressor:            %s\n", config->compressor);
      printf("  ZLIB level:            %" PRId32 "\n", config->zlib_level);
      printf("  Minimum size:          %" PRId64 " bytes\n", config->min_size_bytes);
      printf("  Minimum saving:        %" PRId32 "%%\n", config->min_saving_percentage);
      printf("  Worker count:          %" PRId32 "\n", config->worker_count);
      printf("  Database path:         %s\n", config->database_path);
      printf("  afsctool path:         %s\n", config->afsctool_path);
      printf("  Targets (%zu):\n", config->target_count);
      for (size_t i = 0; i < config->target_count; i++)
        printf("    %s\n", config->targets[i]);
      printf("  Excludes (%zu):\n", config->exclude_count);
      for (size_t i = 0; i < config->exclude_count; i++)
        printf("    %s\n", config->excludes[i]);
      printf("  Incompressible extensions (%zu):\n    ", config->incompressible_count);
      for (size_t i = 0; i < config->incompressible_count; i++)
        printf(".%s%s", config->incompressible_extensions[i], (i + 1 == config->incompressible_count) ? "" : ", ");
      printf("\n");
    }
  } else {
    fprintf(stderr, "Unknown command: %s\n\n", command);
    print_usage(arguments[0]);
    exit_code = 1;
  }

  if (cli_targets && config->targets != cli_targets) {
    for (size_t i = 0; i < cli_target_count; i++)
      free(cli_targets[i]);
    free(cli_targets);
  }
  config_free(config);
  return exit_code;
}

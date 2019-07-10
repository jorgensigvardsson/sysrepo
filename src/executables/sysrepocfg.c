/**
 * @file sysrepocfg.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief sysrepocfg tool
 *
 * @copyright
 * Copyright 2018 Deutsche Telekom AG.
 * Copyright 2018 - 2019 CESNET, z.s.p.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "sysrepo.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <getopt.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>

#include <libyang/libyang.h>

static void
help_print(void)
{
    printf(
        "sysrepocfg - sysrepo configuration tool\n"
        "\n"
        "Usage:\n"
        "  sysrepocfg <operation-option> [other-options]\n"
        "\n"
        "Available operation-options:\n"
        "  -h, --help                   Prints usage help.\n"
        "  -I, --import[=<file-path>]   Import the configuration from a file or STDIN.\n"
        "  -X, --export[=<file-path>]   Export configuration to a file or STDOUT.\n"
        "  -E, --edit[=<file-path>/<editor>]\n"
        "                               Edit configuration data by merging (applying) a configuration (edit) file or\n"
        "                               by editing the current datastore content using a text editor.\n"
        "  -R, --rpc[=<file-path>/<editor>]\n"
        "                               Send a RPC/action in a file or using a text editor. Output is printed to STDOUT.\n"
        "  -N, --notification[=<file-path>/<editor>]\n"
        "                               Send a notification in a file or using a text editor.\n"
        "  -C, --copy-from <file-path>/<source-datastore>\n"
        "                               Perform a copy-config from a file or a datastore.\n"
        "\n"
        "       When both a <file-path> and <editor>/<target-datastore> can be specified, it is always first checked\n"
        "       that the file exists. If not, then it is interpreted as the other parameter.\n"
        "       If no <file-path> and no <editor> is set, use text editor in $VISUAL or $EDITOR environment variables.\n"
        "\n"
        "Available other-options:\n"
        "  -d, --datastore <datastore>  Datastore to be operated on, \"running\" by default (\"running\", \"startup\",\n"
        "                               \"candidate\", \"operational\", or \"state\") (import, export, edit, copy-from op).\n"
        "  -m, --module <module-name>   Module to be operated on, otherwise it is operated on full datastore\n"
        "                               (import, export, edit, copy-from op).\n"
        "  -x, --xpath <xpath>          XPath to select (export op).\n"
        "  -f, --format <format>        Data format to be used, by default based on file extension or \"xml\" if not applicable\n"
        "                               (\"xml\", \"json\", or \"lyb\") (import, export, edit, rpc, notification, copy-from op).\n"
        "  -l, --lock                   Lock the specified datastore for the whole operation (edit op).\n"
        "  -n, --not-strict             Silently ignore any unknown data (import, edit, rpc, notification, copy-from op).\n"
        "  -v, --verbosity <level>      Change verbosity to a level (none, error, warning, info, debug) or number (0, 1, 2, 3, 4).\n"
        "\n"
    );
}

static void
error_print(int sr_error, const char *format, ...)
{
    va_list ap;
    char msg[2048];

    if (!sr_error) {
        sprintf(msg, "sysrepocfg error: %s\n", format);
    } else {
        sprintf(msg, "sysrepocfg error: %s (%s)\n", format, sr_strerror(sr_error));
    }

    va_start(ap, format);
    vfprintf(stderr, msg, ap);
    va_end(ap);
}

static void
error_ly_print(struct ly_ctx *ctx)
{
    struct ly_err_item *e;

    for (e = ly_err_first(ctx); e; e = e->next) {
        error_print(0, "libyang: %s", e->msg);
    }

    ly_err_clean(ctx, NULL);
}

static int
step_edit_input(const char *editor, const char *path)
{
    int ret;
    pid_t pid, wait_pid;

    /* learn what editor to use */
    if (!editor) {
        editor = getenv("VISUAL");
    }
    if (!editor) {
        editor = getenv("EDITOR");
    }
    if (!editor) {
        error_print(0, "Editor not specified nor read from the environment");
        return EXIT_FAILURE;
    }

    if ((pid = vfork()) == -1) {
        error_print(0, "Fork failed (%s)", strerror(errno));
        return EXIT_FAILURE;
    } else if (pid == 0) {
        /* child */
        execlp(editor, editor, path, (char *)NULL);

        error_print(0, "Exec failed (%s)", strerror(errno));
        exit(EXIT_FAILURE);
    } else {
        /* parent */
        wait_pid = wait(&ret);
        if (wait_pid != pid) {
            error_print(0, "Child process other than the editor exited, weird");
            return EXIT_FAILURE;
        }
        if (!WIFEXITED(ret)) {
            error_print(0, "Editor exited in a non-standard way");
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}

static int
step_read_file(FILE *file, char **mem)
{
    size_t mem_size, mem_used;

    mem_size = 512;
    mem_used = 0;
    *mem = malloc(mem_size);

    do {
        if (mem_used == mem_size) {
            mem_size >>= 1;
            *mem = realloc(*mem, mem_size);
        }

        mem_used += fread(*mem + mem_used, 1, mem_size - mem_used, file);
    } while (mem_used == mem_size);

    if (ferror(file)) {
        free(*mem);
        error_print(0, "Error reading from file (%s)", strerror(errno));
        return EXIT_FAILURE;
    } else if (!feof(file)) {
        free(*mem);
        error_print(0, "Unknown file problem");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int
step_load_data(sr_session_ctx_t *sess, const char *file_path, LYD_FORMAT format, int flags, struct lyd_node **data)
{
    struct ly_ctx *ly_ctx;
    char *ptr;

    ly_ctx = (struct ly_ctx *)sr_get_context(sr_session_get_connection(sess));

    /* learn format */
    if (format == LYD_UNKNOWN) {
        if (!file_path) {
            error_print(0, "When reading data from STDIN, format must be specified");
            return EXIT_FAILURE;
        }

        ptr = strrchr(file_path, '.');
        if (ptr && !strcmp(ptr, ".xml")) {
            format = LYD_XML;
        } else if (ptr && !strcmp(ptr, ".json")) {
            format = LYD_JSON;
        } else if (ptr && !strcmp(ptr, ".lyb")) {
            format = LYD_LYB;
        } else {
            error_print(0, "Failed to detect format of \"%s\"", file_path);
            return EXIT_FAILURE;
        }
    }

    /* do not validate candidate data */
    if (sr_session_get_ds(sess) == SR_DS_CANDIDATE) {
        flags |= LYD_OPT_TRUSTED;
    }

    /* parse import data */
    if (file_path) {
        *data = lyd_parse_path(ly_ctx, file_path, format, flags, NULL);
    } else {
        /* we need to load the data into memory first */
        if (step_read_file(stdin, &ptr)) {
            return EXIT_FAILURE;
        }
        *data = lyd_parse_mem(ly_ctx, ptr, format, flags);
        free(ptr);
    }
    if (ly_errno) {
        error_ly_print(ly_ctx);
        error_print(0, "Data parsing failed");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int
step_create_input_file(LYD_FORMAT format, char *tmp_file)
{
    int suffix, fd;

    if (format == LYD_LYB) {
        error_print(0, "LYB binary format cannot be opened in a text editor");
        return EXIT_FAILURE;
    } else if (format == LYD_UNKNOWN) {
        format = LYD_XML;
    }

    /* create temporary file */
    if (format == LYD_JSON) {
        sprintf(tmp_file, "/tmp/srtmpXXXXXX.json");
        suffix = 5;
    } else {
        sprintf(tmp_file, "/tmp/srtmpXXXXXX.xml");
        suffix = 4;
    }
    fd = mkstemps(tmp_file, suffix);
    if (fd == -1) {
        error_print(0, "Failed to open temporary file (%s)", strerror(errno));
        return EXIT_FAILURE;
    }
    close(fd);

    return EXIT_SUCCESS;
}

static int
op_import(sr_session_ctx_t *sess, const char *file_path, const char *module_name, LYD_FORMAT format, int not_strict)
{
    struct lyd_node *data;
    int r, flags;

    flags = LYD_OPT_CONFIG | (not_strict ? 0 : LYD_OPT_STRICT);
    if (step_load_data(sess, file_path, format, flags, &data)) {
        return EXIT_FAILURE;
    }

    /* replace config (always spends data) */
    r = sr_replace_config(sess, module_name, data, sr_session_get_ds(sess));
    if (r) {
        error_print(r, "Replace config failed");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int
op_export(sr_session_ctx_t *sess, const char *file_path, const char *module_name, const char *xpath, LYD_FORMAT format)
{
    struct lyd_node *data;
    FILE *file = NULL;
    char *str;
    int r;

    if (format == LYD_UNKNOWN) {
        format = LYD_XML;
    }

    if (file_path) {
        file = fopen(file_path, "w");
        if (!file) {
            error_print(0, "Failed to open \"%s\" for writing (%s)", file_path, strerror(errno));
            return EXIT_FAILURE;
        }
    }

    /* get subtrees */
    if (module_name) {
        asprintf(&str, "/%s:*", module_name);
        r = sr_get_data(sess, str, &data);
        free(str);
    } else if (xpath) {
        r = sr_get_data(sess, xpath, &data);
    } else {
        r = sr_get_data(sess, "/*", &data);
    }
    if (r != SR_ERR_OK) {
        error_print(r, "Getting data failed");
        if (file) {
            fclose(file);
        }
        return EXIT_FAILURE;
    }

    /* print exported data */
    lyd_print_file(file ? file : stdout, data, format, LYP_FORMAT | LYP_WITHSIBLINGS);
    lyd_free_withsiblings(data);

    /* cleanup */
    if (file) {
        fclose(file);
    }
    return EXIT_SUCCESS;
}

static int
op_edit(sr_session_ctx_t *sess, const char *file_path, const char *editor, const char *module_name, LYD_FORMAT format,
        int lock, int not_strict)
{
    char tmp_file[22];
    int r, flags, rc = EXIT_FAILURE;
    struct lyd_node *data;

    if (file_path) {
        /* just apply an edit form a file */
        flags = LYD_OPT_EDIT | (not_strict ? 0 : LYD_OPT_STRICT);
        if (step_load_data(sess, file_path, format, flags, &data)) {
            return EXIT_FAILURE;
        }

        r = sr_edit_batch(sess, data, "merge");
        lyd_free_withsiblings(data);
        if (r != SR_ERR_OK) {
            error_print(r, "Failed to prepare edit");
            return EXIT_FAILURE;
        }

        r = sr_apply_changes(sess);
        if (r != SR_ERR_OK) {
            error_print(r, "Failed to merge edit data");
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }

    /* create temporary file */
    if (step_create_input_file(format, tmp_file)) {
        return EXIT_FAILURE;
    }

    /* lock if requested */
    if (lock && ((r = sr_lock(sess, module_name)) != SR_ERR_OK)) {
        error_print(r, "Lock failed");
        return EXIT_FAILURE;
    }

    /* use export operation to get data to edit */
    if (op_export(sess, tmp_file, module_name, NULL, format)) {
        goto cleanup_unlock;
    }

    /* edit */
    if (step_edit_input(editor, tmp_file)) {
        goto cleanup_unlock;
    }

    /* use import operation to store edited data */
    if (op_import(sess, tmp_file, module_name, format, not_strict)) {
        goto cleanup_unlock;
    }

    /* success */
    rc = EXIT_SUCCESS;

cleanup_unlock:
    if (lock && ((r = sr_unlock(sess, module_name)) != SR_ERR_OK)) {
        error_print(r, "Unlock failed");
    }
    return rc;
}

static int
op_rpc(sr_session_ctx_t *sess, const char *file_path, const char *editor, LYD_FORMAT format, int not_strict)
{
    char tmp_file[22];
    int r, flags;
    struct lyd_node *input, *output, *node;

    if (!file_path) {
        /* create temp file */
        if (step_create_input_file(format, tmp_file)) {
            return EXIT_FAILURE;
        }

        /* load rpc/action into the file */
        if (step_edit_input(editor, tmp_file)) {
            return EXIT_FAILURE;
        }

        file_path = tmp_file;
    }

    /* load the file */
    flags = LYD_OPT_RPC | (not_strict ? 0 : LYD_OPT_STRICT);
    if (step_load_data(sess, file_path, format, flags, &input)) {
        return EXIT_FAILURE;
    }

    /* send rpc/action */
    r = sr_rpc_send_tree(sess, input, &output);
    lyd_free_withsiblings(input);
    if (r) {
        error_print(r, "Sending RPC/action failed");
        return EXIT_FAILURE;
    }

    /* print output if any */
    LY_TREE_FOR(output->child, node) {
        if (!node->dflt) {
            break;
        }
    }
    if (node) {
        lyd_print_file(stdout, output, format, LYP_FORMAT);
    }
    lyd_free_withsiblings(output);

    return EXIT_SUCCESS;
}

static int
op_notif(sr_session_ctx_t *sess, const char *file_path, const char *editor, LYD_FORMAT format, int not_strict)
{
    char tmp_file[22];
    int r, flags;
    struct lyd_node *notif;

    if (!file_path) {
        /* create temp file */
        if (step_create_input_file(format, tmp_file)) {
            return EXIT_FAILURE;
        }

        /* load notif into the file */
        if (step_edit_input(editor, tmp_file)) {
            return EXIT_FAILURE;
        }

        file_path = tmp_file;
    }

    /* load the file */
    flags = LYD_OPT_NOTIF | (not_strict ? 0 : LYD_OPT_STRICT);
    if (step_load_data(sess, file_path, format, flags, &notif)) {
        return EXIT_FAILURE;
    }

    /* send notification */
    r = sr_event_notif_send_tree(sess, notif);
    lyd_free_withsiblings(notif);
    if (r) {
        error_print(r, "Sending notification failed");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int
op_copy(sr_session_ctx_t *sess, const char *file_path, sr_datastore_t source_ds, sr_datastore_t target_ds,
        const char *module_name, LYD_FORMAT format, int not_strict)
{
    int r, flags;
    struct lyd_node *data;

    if (file_path) {
        /* load the file */
        flags = LYD_OPT_CONFIG | (not_strict ? 0 : LYD_OPT_STRICT);
        if (step_load_data(sess, file_path, format, flags, &data)) {
            return EXIT_FAILURE;
        }

        /* replace config */
        r = sr_replace_config(sess, module_name, data, target_ds);
        if (r) {
            error_print(r, "Replace config failed");
            return EXIT_FAILURE;
        }
    } else {
        /* copy config */
        r = sr_copy_config(sess, module_name, source_ds, target_ds);
        if (r) {
            error_print(r, "Copy config failed");
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}

static int
arg_is_file(const char *optarg)
{
    return !access(optarg, F_OK);
}

static int
arg_get_ds(const char *optarg, sr_datastore_t *ds)
{
    if (!strcmp(optarg, "running")) {
        *ds = SR_DS_RUNNING;
    } else if (!strcmp(optarg, "startup")) {
        *ds = SR_DS_STARTUP;
    } else if (!strcmp(optarg, "candidate")) {
        *ds = SR_DS_CANDIDATE;
    } else if (!strcmp(optarg, "operational")) {
        *ds = SR_DS_OPERATIONAL;
    } else if (!strcmp(optarg, "state")) {
        *ds = SR_DS_STATE;
    } else {
        error_print(0, "Unknown datastore \"%s\"", optarg);
        return -1;
    }

    return 0;
}

int
main(int argc, char** argv)
{
    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *sess = NULL;
    sr_datastore_t ds = SR_DS_RUNNING, source_ds;
    LYD_FORMAT format = LYD_UNKNOWN;
    const char *module_name = NULL, *editor = NULL, *file_path = NULL, *xpath = NULL;
    sr_log_level_t log_level = 0;
    int r, rc = EXIT_FAILURE, opt, operation = 0, lock = 0, not_strict = 0;
    struct option options[] = {
        {"help",            no_argument,       NULL, 'h'},
        {"import",          optional_argument, NULL, 'I'},
        {"export",          optional_argument, NULL, 'X'},
        {"edit",            optional_argument, NULL, 'E'},
        {"rpc",             optional_argument, NULL, 'R'},
        {"notification",    optional_argument, NULL, 'N'},
        {"copy-from",       required_argument, NULL, 'C'},
        {"datastore",       required_argument, NULL, 'd'},
        {"module",          required_argument, NULL, 'm'},
        {"xpath",           required_argument, NULL, 'x'},
        {"format",          required_argument, NULL, 'f'},
        {"lock",            no_argument,       NULL, 'l'},
        {"not-strict",      no_argument,       NULL, 'n'},
        {"verbosity",       required_argument, NULL, 'v'},
    };

    if (argc == 1) {
        help_print();
        goto cleanup;
    }

    /* process options */
    opterr = 0;
    while ((opt = getopt_long(argc, argv, "hI::X::E::R::N::C:d:m:x:f:lnv:", options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            help_print();
            rc = EXIT_SUCCESS;
            goto cleanup;
        case 'I':
            if (operation) {
                error_print(0, "Operation already specified");
                goto cleanup;
            }
            if (optarg) {
                file_path = optarg;
            }
            operation = opt;
            break;
        case 'X':
            if (operation) {
                error_print(0, "Operation already specified");
                goto cleanup;
            }
            if (optarg) {
                file_path = optarg;
            }
            operation = opt;
            break;
        case 'E':
            if (operation) {
                error_print(0, "Operation already specified");
                goto cleanup;
            }
            if (optarg) {
                if (arg_is_file(optarg)) {
                    file_path = optarg;
                } else {
                    editor = optarg;
                }
            }
            operation = opt;
            break;
        case 'R':
            if (operation) {
                error_print(0, "Operation already specified");
                goto cleanup;
            }
            if (optarg) {
                if (arg_is_file(optarg)) {
                    file_path = optarg;
                } else {
                    editor = optarg;
                }
            }
            operation = opt;
            break;
        case 'N':
            if (operation) {
                error_print(0, "Operation already specified");
                goto cleanup;
            }
            if (optarg) {
                if (arg_is_file(optarg)) {
                    file_path = optarg;
                } else {
                    editor = optarg;
                }
            }
            operation = opt;
            break;
        case 'C':
            if (operation) {
                error_print(0, "Operation already specified");
                goto cleanup;
            }
            if (arg_is_file(optarg)) {
                file_path = optarg;
            } else {
                if (arg_get_ds(optarg, &source_ds)) {
                    goto cleanup;
                }
            }
            operation = opt;
            break;
        case 'd':
            if (arg_get_ds(optarg, &ds)) {
                goto cleanup;
            }
            break;
        case 'm':
            if (module_name) {
                error_print(0, "Module already specified");
                goto cleanup;
            } else if (xpath) {
                error_print(0, "Only one of options --module and --xpath can be set");
                goto cleanup;
            }
            module_name = optarg;
            break;
        case 'x':
            if (xpath) {
                error_print(0, "XPath already specified");
                goto cleanup;
            } else if (module_name) {
                error_print(0, "Only one of options --module and --xpath can be set");
                goto cleanup;
            }
            xpath = optarg;
            break;
        case 'f':
            if (!strcmp(optarg, "xml")) {
                format = LYD_XML;
            } else if (!strcmp(optarg, "json")) {
                format = LYD_JSON;
            } else if (!strcmp(optarg, "lyb")) {
                format = LYD_LYB;
            } else {
                error_print(0, "Unknown format \"%s\"", optarg);
                goto cleanup;
            }
            break;
        case 'l':
            lock = 1;
            break;
        case 'n':
            not_strict = 1;
            break;
        case 'v':
            if (!strcmp(optarg, "none")) {
                log_level = SR_LL_NONE;
            } else if (!strcmp(optarg, "error")) {
                log_level = SR_LL_ERR;
            } else if (!strcmp(optarg, "warning")) {
                log_level = SR_LL_WRN;
            } else if (!strcmp(optarg, "info")) {
                log_level = SR_LL_INF;
            } else if (!strcmp(optarg, "debug")) {
                log_level = SR_LL_DBG;
            } else if ((strlen(optarg) == 1) && (optarg[0] >= '0') && (optarg[0] <= '4')) {
                log_level = atoi(optarg);
            } else {
                error_print(0, "Invalid verbosity \"%s\"", optarg);
                goto cleanup;
            }
            sr_log_stderr(log_level);
            break;
        default:
            error_print(0, "Invalid option or missing argument: -%c", optopt);
            goto cleanup;
        }
    }

    /* check for additional argument */
    if (optind < argc) {
        error_print(0, "Redundant parameters (%s)", argv[optind]);
        goto cleanup;
    }

    /* create connection */
    if ((r = sr_connect(0, &conn)) != SR_ERR_OK) {
        error_print(r, "Failed to connect");
        goto cleanup;
    }

    /* create session */
    if ((r = sr_session_start(conn, ds, &sess)) != SR_ERR_OK) {
        error_print(r, "Failed to start a session");
        goto cleanup;
    }

    /* perform the operation */
    switch (operation) {
    case 'I':
        rc = op_import(sess, file_path, module_name, format, not_strict);
        break;
    case 'X':
        rc = op_export(sess, file_path, module_name, xpath, format);
        break;
    case 'E':
        rc = op_edit(sess, file_path, editor, module_name, format, lock, not_strict);
        break;
    case 'R':
        rc = op_rpc(sess, file_path, editor, format, not_strict);
        break;
    case 'N':
        rc = op_notif(sess, file_path, editor, format, not_strict);
        break;
    case 'C':
        rc = op_copy(sess, file_path, source_ds, ds, module_name, format, not_strict);
        break;
    case 0:
        error_print(0, "No operation specified");
        break;
    default:
        error_print(0, "Internal");
        break;
    }

cleanup:
    sr_session_stop(sess);
    sr_disconnect(conn);
    return rc;
}

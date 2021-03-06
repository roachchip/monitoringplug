/***
 * Monitoring Plugin - check_nfs.c
 **
 *
 * check_nfs - Check if the Host is exporting at least one or the named path.
 *
 * Copyright (C) 2012 Marius Rieder <marius.rieder@durchmesser.ch>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * $Id$
 */

const char *progname  = "check_nfs";
const char *progdesc  = "Check if the Host is exporting at least one or the named path.";
const char *progvers  = "0.1";
const char *progcopy  = "2011";
const char *progauth  = "Marius Rieder <marius.rieder@durchmesser.ch>";
const char *progusage = "-H hostname [--help] [--timeout TIMEOUT]";

/* MP Includes */
#include "mp_common.h"
#include "rpc_utils.h"
/* Default Includes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <ctype.h>
/* Library Includes */
#include <rpc/rpc.h>
#include <rpcsvc/mount.h>

/* Global Vars */
const char *hostname = NULL;
const char *export = NULL;
char *noconnection = NULL;
char *callfailed = NULL;
char *noexport = NULL;
char *nfs_warn = NULL;
char *nfs_crit = NULL;
char *exportok = NULL;
struct timeval to;
char **rpcversion = NULL;
int rpcversions = 0;
char **rpctransport = NULL;
int rpctransports = 0;
thresholds *time_threshold = NULL;
CLIENT *client = NULL;

/* Function prototype */
int check_export(struct rpcent *program, unsigned long version, char *proto);

int main (int argc, char **argv) {
    /* Local Vars */
    int i, j, ret;
    int tstate;
    char *buf;
    struct rpcent *program;
    struct rpcent *nfs;
    struct timeval start_time;
    double time_delta;

    /* Set signal handling and alarm */
    if (signal (SIGALRM, rpc_timeout_alarm_handler) == SIG_ERR)
        critical("Setup SIGALRM trap failed!");

    /* Process check arguments */
    if (process_arguments(argc, argv) != OK)
        unknown("Parsing arguments failed!");

    /* Start plugin timeout */
    alarm(mp_timeout);
    to.tv_sec = (time_t)mp_timeout;
    to.tv_usec = 0;

    // PLUGIN CODE
    program = rpc_getrpcent("showmount");
    if (program == NULL)
        program = rpc_getrpcent("mount");
    if (program == NULL)
        program = rpc_getrpcent("mountd");
    nfs = rpc_getrpcent("nfs");

    for(i=0; i < rpcversions; i++) {
        for(j=0; j < rpctransports; j++) {
            check_export(program, atoi(rpcversion[i]), rpctransport[j]);

            gettimeofday(&start_time, NULL);

            ret = rpc_ping((char *)hostname, nfs, atoi(rpcversion[i]), rpctransport[j], to);

            time_delta = mp_time_delta(start_time);

            tstate = get_status(time_delta, time_threshold);

            buf = mp_malloc(128);
            mp_snprintf(buf, 128, "%s:v%s", rpctransport[j], rpcversion[i]);

            mp_perfdata_float(buf, (float)time_delta, "s", time_threshold);

            if (ret != RPC_SUCCESS || tstate == STATE_CRITICAL) {
                mp_strcat_comma(&nfs_crit, buf);
            } else if(tstate == STATE_WARNING)  {
                mp_strcat_comma(&nfs_warn, buf);
            }

            free(buf);
        }
    }

    /* Free */
    free(program->r_name);
    free(program);
    free(nfs->r_name);
    free(nfs);
    free(rpcversion);
    free(rpctransport);
    free_threshold(time_threshold);

    if (noconnection || callfailed || noexport || nfs_crit) {
        char *out = NULL;
        if (noconnection) {
            out = strdup("Can't connect to:");
            mp_strcat_space(&out, noconnection);
        }
        if (callfailed) {
            mp_strcat_space(&out, "Call failed by:");
            mp_strcat_space(&out, callfailed);
        }
        if (noexport) {
            mp_strcat_space(&out, "No export found by:");
            mp_strcat_space(&out, noexport);
        }
        if (nfs_crit) {
            mp_strcat_space(&out, "NFS Critical:");
            mp_strcat_space(&out, nfs_crit);
        }
        if (nfs_warn) {
            mp_strcat_space(&out, "NFS Warning:");
            mp_strcat_space(&out, nfs_warn);
        }
        critical(out);
    } else if (nfs_warn) {
        warning("NFS Warning: %s", nfs_warn);
    } else {
        if(export)
            ok("%s exported by %s", export, exportok);
        ok("mountd export by %s", exportok);
    }

    critical("You should never reach this point.");
}

int check_export(struct rpcent *program, unsigned long version, char *proto) {
    char *buf;
    int ret;
    exports exportlist, exportlistPtr;

    buf = mp_malloc(128);
    mp_snprintf(buf, 128, "%s:%sv%ld", proto, program->r_name, version);

    if (mp_verbose >= 1)
        printf("Connect to %s:%sv%ld", proto, program->r_name, version);

    client = clnt_create((char *)hostname, program->r_number, 3, proto);
    if (client == NULL) {
        if (mp_verbose >= 1)
            printf("   failed!\n");
        mp_strcat_comma(&noconnection, buf);
        free(buf);
        return 1;
    }

    if (mp_verbose >= 1)
        printf("   ok\n");

    memset(&exportlist, '\0', sizeof(exportlist));

    ret = clnt_call(client, MOUNTPROC_EXPORT, (xdrproc_t) xdr_void, NULL,
            (xdrproc_t) mp_xdr_exports, (caddr_t) &exportlist, to);
    if (ret != RPC_SUCCESS) {
        if (mp_verbose >= 1)
            printf("Get export tailed. %d: %s\n", ret, clnt_sperrno(ret));
        mp_strcat_comma(&callfailed, buf);
        free(buf);
        clnt_destroy(client);
        return 1;
    }

    if (mp_verbose >= 1) {
        groups grouplist;
        exportlistPtr = exportlist;
        while (exportlistPtr) {

            printf("%-*s ", 40, exportlistPtr->ex_dir);
            grouplist = exportlistPtr->ex_groups;
            if (grouplist)
                while (grouplist) {
                    printf("%s%s", grouplist->gr_name,
                            grouplist->gr_next ? "," : "");
                    grouplist = grouplist->gr_next;
                }
            else
                printf("(everyone)");

            printf("\n");
            exportlistPtr = exportlistPtr->ex_next;
        }
    }

    if (export != NULL) {
        exportlistPtr=exportlist;
        while (exportlistPtr) {
            if (strcmp(export, exportlistPtr->ex_dir) == 0)
                break;
            exportlistPtr = exportlistPtr->ex_next;
        }

        if (exportlistPtr==NULL) {
            mp_strcat_comma(&noexport, buf);
            free(buf);
            clnt_freeres(client, (xdrproc_t) mp_xdr_exports, (caddr_t) &exportlist);
            clnt_destroy(client);
            return 1;
        }
    } else {
        if (exportlist==NULL) {
            mp_strcat_comma(&noexport, buf);
            free(buf);
            clnt_freeres(client, (xdrproc_t) mp_xdr_exports, (caddr_t) &exportlist);
            clnt_destroy(client);
            return 1;
        }
    }

    clnt_freeres(client, (xdrproc_t) mp_xdr_exports, (caddr_t) &exportlist);
    clnt_destroy(client);

    mp_strcat_comma(&exportok, buf);
    free(buf);

    return 0;
}

int process_arguments (int argc, char **argv) {
    int c;
    int option = 0;

    static struct option longopts[] = {
        MP_LONGOPTS_DEFAULT,
        MP_LONGOPTS_HOST,
        // PLUGIN OPTS
        {"export", required_argument, 0, 'e'},
        {"rpcversion", required_argument, 0, 'r'},
        {"transport", required_argument, 0, 'T'},
        MP_LONGOPTS_END
    };

    /* Set default */
    setWarnTime(&time_threshold, "0.5s");
    setCritTime(&time_threshold, "1s");

    while (1) {
        c = mp_getopt(&argc, &argv, MP_OPTSTR_DEFAULT"H:w:c:e:r:T:", longopts, &option);

        if (c == -1 || c == EOF)
            break;

        getopt_wc_time(c, optarg, &time_threshold);

        switch (c) {
            /* Host opt */
            case 'H':
                getopt_host(optarg, &hostname);
                break;
            /* Plugin opts */
            case 'e':
                export = optarg;
                break;
            case 'r':
                mp_array_push(&rpcversion, optarg, &rpcversions);
                break;
            case 'T': {
                mp_array_push(&rpctransport, optarg, &rpctransports);
                break;
            }
        }
    }

    /* Check requirements */
    if (!hostname)
        usage("Hostname is mandatory.");

    /* Apply defaults */
    if (rpcversion == NULL)
        mp_array_push(&rpcversion, "3", &rpcversions);
    if (rpctransport == NULL) {
        mp_array_push(&rpctransport, "udp", &rpctransports);
        mp_array_push(&rpctransport, "tcp", &rpctransports);
    }

    return(OK);
}

void print_help (void) {
    print_revision();
    print_copyright();

    printf("\n");

    printf("Check description: %s", progdesc);

    printf("\n\n");

    print_usage();

    print_help_default();
    print_help_host();
    printf(" -T, --transport=transport[,transport]\n");
    printf("      Transports to check.\n");
    printf(" -r, --rpcversion=version[,version]\n");
    printf("      Versions to check.\n");
    printf(" -e, --export=path\n");
    printf("      Check whether the server exports path.\n");
    print_help_warn_time("0.5s");
    print_help_crit_time("1s");
}

/* vim: set ts=4 sw=4 et syn=c : */

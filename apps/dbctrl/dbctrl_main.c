/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2017 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* Command line options to be passed to getopt(3) */
#define DBCTRL_OPTS "hDSd:pbn:r:m:Zi"

/*
 * remove_entry
 */
static int
remove_entry(char *dbname, char *key)
{
#ifdef NOTYET /* This assumes direct access to database */
    return db_del(dbname, key);
#else
    return 0;
#endif
}

/*! usage
 */
static void
usage(char *argv0)
{
    fprintf(stderr, "usage:%s\n"
	    "where options are\n"
            "\t-h\t\tHelp\n"
            "\t-D\t\tDebug\n"
            "\t-S\t\tLog on syslog\n"
            "\t-d <db>\t\tDatabase name (default: running)\n"
    	    "\t-p\t\tDump database on stdout\n"
    	    "\t-b\t\tBrief output, just print keys. Combine with -p or -m\n"
	    "\t-n \"<key> <val>\" Add database entry\n"
            "\t-r <key>\tRemove database entry\n"
	    "\t-m <regexp key>\tMatch regexp key in database\n"
    	    "\t-Z\t\tDelete database\n"
    	    "\t-i\t\tInit database\n",
	    argv0
	    );
    exit(0);
}

int
main(int argc, char **argv)
{
    char             c;
    int              zapdb;
    int              initdb;
    int              dumpdb;
    int              addent;
    int              rment;
    char            *matchkey = NULL;
    char            *addstr;
    char             rmkey[MAXPATHLEN];
    int              brief;
    char             db[MAXPATHLEN] = {0,};
    int              use_syslog;
    clicon_handle    h;

    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__PROGRAM__, LOG_INFO, CLICON_LOG_STDERR); 

    /* Defaults */
    zapdb      = 0;
    initdb     = 0;
    dumpdb     = 0;
    addent     = 0;
    rment      = 0;
    brief      = 0;
    use_syslog = 0;
    addstr     = NULL;
    memcpy(db, "running", strlen("running")+1);
    memset(rmkey, '\0', sizeof(rmkey));

    if ((h = clicon_handle_init()) == NULL)
	goto done;
    /* getopt in two steps, first find config-file before over-riding options. */
    while ((c = getopt(argc, argv, DBCTRL_OPTS)) != -1)
	switch (c) {
	case '?' :
	case 'h' : /* help */
	    usage(argv[0]);
	    break;
	case 'D' : /* debug */
	    debug = 1;	
	    break;
	 case 'S': /* Log on syslog */
	     use_syslog = 1;
	     break;
	}
    /* 
     * Logs, error and debug to stderr or syslog, set debug level
     */
    clicon_log_init(__PROGRAM__, debug?LOG_DEBUG:LOG_INFO, 
		    use_syslog?CLICON_LOG_SYSLOG:CLICON_LOG_STDERR); 
    clicon_debug_init(debug, NULL); 


    /* Now rest of options */   
    optind = 1;
    while ((c = getopt(argc, argv, DBCTRL_OPTS)) != -1)
	switch (c) {
	case 'Z': /* Zap database */
	    zapdb++;
	    break;
	case 'i': /* Init database */
	    initdb++;
	    break;
	case 'p': /* Dump/print database */
	    dumpdb++;
	    break;
	case 'b': /* Dump/print/match database  brief (combone w -p or -m) */
	    brief++;
	    break;
	case 'd': /* db either db filename or symbolic: running|candidate */
	    if (!optarg || sscanf(optarg, "%s", db) != 1)
	        usage(argv[0]);
	    break;
	case 'n': /* add database entry */
	  if (!optarg || !strlen(optarg) || (addstr = strdup(optarg)) == NULL)
	        usage(argv[0]);
	  /* XXX addign both key and value, for now only key */
	    addent++;
	    break;
	case 'r':
	     if (!optarg || sscanf(optarg, "%s", rmkey) != 1)
		 usage(argv[0]);
	     rment++;
	     break;
	case 'm':
	  if (!optarg || !strlen(optarg) || (matchkey = strdup(optarg)) == NULL)
	        usage(argv[0]);
	    dumpdb++;
	    break;
	case 'D':  /* Processed earlier, ignore now. */
	case 'S':
	    break;
	default:
	    usage(argv[0]);
	    break;
	}
    argc -= optind;
    argv += optind;

    if (*db == '\0'){
	clicon_err(OE_FATAL, 0, "database not specified (with -d <db>): %s");
	goto done;
    }
    if (dumpdb){
	/* Here db must be local file-path */
	if (xmldb_dump_local(stdout, db, matchkey)) {
	    fprintf(stderr, "Match error\n");
	    goto done;
	}
    }
    if (addent) /* add entry */
	if (xmldb_put_xkey(h, db, addstr, NULL, OP_REPLACE) < 0)
	    goto done;
    if (rment)
        if (remove_entry(db, rmkey) < 0)
	    goto done;
    if (zapdb) /* remove databases */ 
	/* XXX This assumes direct access to database */
	if (xmldb_delete(h, db) < 0){
	    clicon_err(OE_FATAL, errno, "xmldb_delete %s", db);
	    goto done;
	}
    if (initdb)
	if (xmldb_init(h, db) < 0)
	    goto done;
  done:
    return 0;
}

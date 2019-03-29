/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

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
#include <pwd.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_backend_transaction.h"
#include "backend_plugin.h"
#include "backend_handle.h"
#include "backend_commit.h"
#include "backend_client.h"

/*! Key values are checked for validity independent of user-defined callbacks
 *
 * Key values are checked as follows:
 * 1. If no value and default value defined, add it.
 * 2. If no value and mandatory flag set in spec, report error.
 * 3. Validate value versus spec, and report error if no match. Currently only int ranges and
 *    string regexp checked.
 * See also db_lv_set() where defaults are also filled in. The case here for defaults
 * are if code comes via XML/NETCONF.
 * @param[in]   yspec   Yang spec
 * @param[in]   td      Transaction data
 * @param[out]  cbret   Cligen buffer containing netconf error (if retval == 0)
 * @retval     -1       Error
 * @retval      0       Validation failed (with cbret set)
 * @retval      1       Validation OK       
 */
static int
generic_validate(yang_spec          *yspec,
		 transaction_data_t *td,
		 cbuf               *cbret)
{
    int             retval = -1;
    cxobj          *x1;
    cxobj          *x2;
    yang_stmt      *ys;
    int             i;
    int             ret;

    /* All entries */
    if ((ret = xml_yang_validate_all_top(td->td_target, cbret)) < 0) 
	goto done;
    if (ret == 0)
	goto fail;
    /* changed entries */
    for (i=0; i<td->td_clen; i++){
	x1 = td->td_scvec[i]; /* source changed */
	x2 = td->td_tcvec[i]; /* target changed */
	/* Should this be recursive? */
	if ((ret = xml_yang_validate_add(x2, cbret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
    /* deleted entries */
    for (i=0; i<td->td_dlen; i++){
	x1 = td->td_dvec[i];
	ys = xml_spec(x1);
	if (ys && yang_mandatory(ys) && yang_config(ys)==0){
	    if (netconf_missing_element(cbret, "protocol", xml_name(x1), "Missing mandatory variable") < 0)
		goto done;
	    goto fail;
	}
    }
    /* added entries */
    for (i=0; i<td->td_alen; i++){
	x2 = td->td_avec[i];
	if ((ret = xml_yang_validate_add(x2, cbret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
    // ok:
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Common startup validation
 * Get db, upgrade it w potential transformed XML, populate it w yang spec,
 * sort it, validate it by triggering a transaction
 * and call application callback validations.
 * @param[in]  h       Clicon handle
 * @param[in]  db      The startup database. The wanted backend state
 * @param[out] xtr     Transformed XML
 * @param[out] cbret   CLIgen buffer w error stmt if retval = 0
 * @retval    -1       Error - or validation failed (but cbret not set)
 * @retval     0       Validation failed (with cbret set)
 * @retval     1       Validation OK       
 * @note Need to differentiate between error and validation fail 
 *
 * 1. Parse startup XML (or JSON)
 * 2. If syntax failure, call startup-cb(ERROR), copy failsafe db to 
 * candidate and commit. Done
 * 3. Check yang module versions between backend and init config XML. (msdiff)
 * 4. Validate startup db. (valid)
 * 5. If valid fails, call startup-cb(Invalid, msdiff), keep startup in candidate and commit failsafe db. Done.
 * 6. Call startup-cb(OK, msdiff) and commit.
 */
static int
startup_common(clicon_handle       h, 
	       char               *db,
	       transaction_data_t *td,
	       cbuf               *cbret)
{
    int                 retval = -1;
    yang_spec          *yspec;
    int                 ret;
    modstate_diff_t    *msd = NULL;
    cxobj              *xt = NULL;
    cxobj              *x;
    
    /* If CLICON_XMLDB_MODSTATE is enabled, then get the db XML with 
     * potentially non-matching module-state in msd
     */
    if (clicon_option_bool(h, "CLICON_XMLDB_MODSTATE"))
	if ((msd = modstate_diff_new()) == NULL)
	    goto done;
    if (xmldb_get(h, db, "/", 1, &xt, msd) < 0)
	goto done;
    if (msd){
	if ((ret = clixon_module_upgrade(h, xt, msd, cbret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_YANG, 0, "Yang spec not set");
	goto done;
    }
    /* After upgrading, XML tree needs to be sorted and yang spec populated */
    if (xml_apply0(xt, CX_ELMNT, xml_spec_populate, yspec) < 0)
	goto done;
    if (xml_apply0(xt, CX_ELMNT, xml_sort, NULL) < 0)
	goto done;
    /* Handcraft transition with with only add tree */
    td->td_target = xt;
    x = NULL;
    while ((x = xml_child_each(td->td_target, x, CX_ELMNT)) != NULL){
	if (cxvec_append(x, &td->td_avec, &td->td_alen) < 0) 
	    goto done;
    }

    /* 4. Call plugin transaction start callbacks */
    if (plugin_transaction_begin(h, td) < 0)
	goto done;

    /* 5. Make generic validation on all new or changed data.
       Note this is only call that uses 3-values */
    if ((ret = generic_validate(yspec, td, cbret)) < 0)
	goto done;
    if (ret == 0)
	goto fail; /* STARTUP_INVALID */

    /* 6. Call plugin transaction validate callbacks */
    if (plugin_transaction_validate(h, td) < 0)
	goto done;

    /* 7. Call plugin transaction complete callbacks */
    if (plugin_transaction_complete(h, td) < 0)
	goto done;
    retval = 1;
 done:
    if (msd)
	modstate_diff_free(msd);
    return retval;
 fail: 
    retval = 0;
    goto done;
}

/*! Read startup db, check upgrades and validate it, return upgraded XML
 *
 * @param[in]  h       Clicon handle
 * @param[in]  db      The startup database. The wanted backend state
 * @param[out] xtr     (Potentially) transformed XML
 * @param[out] cbret   CLIgen buffer w error stmt if retval = 0
 * @retval    -1       Error - or validation failed (but cbret not set)
 * @retval     0       Validation failed (with cbret set)
 * @retval     1       Validation OK       
 */
int
startup_validate(clicon_handle  h, 
		 char          *db,
		 cxobj        **xtr,
		 cbuf          *cbret)
{
    int                 retval = -1;
    int                 ret;
    transaction_data_t *td = NULL;

    /* Handcraft a transition with only target and add trees */
    if ((td = transaction_new()) == NULL)
	goto done;
    if ((ret = startup_common(h, db, td, cbret)) < 0)
	goto done;
    if (ret == 0)
	goto fail;
    if (xtr){
	*xtr = td->td_target;
	td->td_target = NULL;
    }
    retval = 1;
 done:
    if (td)
	transaction_free(td);
    return retval;
 fail: /* cbret should be set */
    retval = 0;
    goto done;
}

/*! Read startup db, check upgrades and commit it
 *
 * @param[in]  h       Clicon handle
 * @param[in]  db      The startup database. The wanted backend state
 * @param[out] cbret   CLIgen buffer w error stmt if retval = 0
 * @retval    -1       Error - or validation failed (but cbret not set)
 * @retval     0       Validation failed (with cbret set)
 * @retval     1       Validation OK       
 */
int
startup_commit(clicon_handle  h, 
	       char          *db,
	       cbuf          *cbret)
{
    int                 retval = -1;
    int                 ret;
    transaction_data_t *td = NULL;

    /* Handcraft a transition with only target and add trees */
    if ((td = transaction_new()) == NULL)
	goto done;
    if ((ret = startup_common(h, db, td, cbret)) < 0)
	goto done;
    if (ret == 0)
	goto fail;
     /* 8. Call plugin transaction commit callbacks */
     if (plugin_transaction_commit(h, td) < 0)
	 goto done;
     /* 9, write (potentially modified) tree to running
      * XXX note here startup is copied to candidate, which may confuse everything
      */
     if ((ret = xmldb_put(h, "running", OP_REPLACE, td->td_target,
			  clicon_username_get(h), cbret)) < 0)
	 goto done;
     if (ret == 0)
	 goto fail;
    /* 10. Call plugin transaction end callbacks */
    plugin_transaction_end(h, td);

    /* 11. Copy running back to candidate in case end functions updated running 
     * XXX: room for improvement: candidate and running may be equal.
     * Copy only diffs?
     */
    if (xmldb_copy(h, "running", "candidate") < 0){
	/* ignore errors or signal major setback ? */
	clicon_log(LOG_NOTICE, "Error in rollback, trying to continue");
	goto done;
    } 
    retval = 1;
 done:
    if (td)
	transaction_free(td);
    return retval;
 fail: /* cbret should be set */
    retval = 0;
    goto done;
}

/*! Validate a candidate db and comnpare to running
 * Get both source and dest datastore, validate target, compute diffs
 * and call application callback validations.
 * @param[in] h         Clicon handle
 * @param[in] candidate The candidate database. The wanted backend state
 * @retval   -1       Error - or validation failed (but cbret not set)
 * @retval    0       Validation failed (with cbret set)
 * @retval    1       Validation OK       
 * @note Need to differentiate between error and validation fail 
 *       (only done for generic_validate)
 */
static int
from_validate_common(clicon_handle       h, 
		     char               *candidate,
		     transaction_data_t *td,
		     cbuf               *cbret)
{
    int         retval = -1;
    yang_spec  *yspec;
    int         i;
    cxobj      *xn;
    int         ret;
    
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }	

    /* This is the state we are going to */
    if (xmldb_get(h, candidate, "/", 1, &td->td_target, NULL) < 0)
	goto done;

    /* Validate the target state. It is not completely clear this should be done 
     * here. It is being made in generic_validate below. 
     * But xml_diff requires some basic validation, at least check that yang-specs
     * have been assigned
     */
    if ((ret = xml_yang_validate_all_top(td->td_target, cbret)) < 0)
	goto done;
    if (ret == 0)
	goto fail;

    /* 2. Parse xml trees 
     * This is the state we are going from */
    if (xmldb_get(h, "running", "/", 1, &td->td_src, NULL) < 0)
	goto done;
    
    /* 3. Compute differences */
    if (xml_diff(yspec, 
		 td->td_src,
		 td->td_target,
		 &td->td_dvec,      /* removed: only in running */
		 &td->td_dlen,
		 &td->td_avec,      /* added: only in candidate */
		 &td->td_alen,
		 &td->td_scvec,     /* changed: original values */
		 &td->td_tcvec,     /* changed: wanted values */
		 &td->td_clen) < 0)
	goto done;
    if (debug>1)
	transaction_print(stderr, td);
    /* Mark as changed in tree */
    for (i=0; i<td->td_dlen; i++){ /* Also down */
	xn = td->td_dvec[i];
	xml_flag_set(xn, XML_FLAG_DEL);
	xml_apply(xn, CX_ELMNT, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_DEL);
	xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    for (i=0; i<td->td_alen; i++){ /* Also down */
	xn = td->td_avec[i];
	xml_flag_set(xn, XML_FLAG_ADD);
	xml_apply(xn, CX_ELMNT, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_ADD);
	xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    for (i=0; i<td->td_clen; i++){ /* Also up */
	xn = td->td_scvec[i];
	xml_flag_set(xn, XML_FLAG_CHANGE);
	xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
	xn = td->td_tcvec[i];
	xml_flag_set(xn, XML_FLAG_CHANGE);
	xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    /* 4. Call plugin transaction start callbacks */
    if (plugin_transaction_begin(h, td) < 0)
	goto done;

    /* 5. Make generic validation on all new or changed data.
       Note this is only call that uses 3-values */
    if ((ret = generic_validate(yspec, td, cbret)) < 0)
	goto done;
    if (ret == 0)
	goto fail;

    /* 6. Call plugin transaction validate callbacks */
    if (plugin_transaction_validate(h, td) < 0)
	goto done;

    /* 7. Call plugin transaction complete callbacks */
    if (plugin_transaction_complete(h, td) < 0)
	goto done;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Do a diff between candidate and running, then start a commit transaction
 *
 * The code reverts changes if the commit fails. But if the revert
 * fails, we just ignore the errors and proceed. Maybe we should
 * do something more drastic?
 * @param[in]  h         Clicon handle
 * @param[in]  candidate A candidate database, not necessarily "candidate"
 * @retval   -1       Error - or validation failed 
 * @retval    0       Validation failed (with cbret set)
 * @retval    1       Validation OK       
 * @note Need to differentiate between error and validation fail
 *       (only done for validate_common)
 */
int
candidate_commit(clicon_handle h, 
		 char         *candidate,
		 cbuf         *cbret)
{
    int                 retval = -1;
    transaction_data_t *td = NULL;
    int                 ret;

     /* 1. Start transaction */
    if ((td = transaction_new()) == NULL)
	goto done;

    /* Common steps (with validate). Load candidate and running and compute diffs
     * Note this is only call that uses 3-values
     */
    if ((ret = from_validate_common(h, candidate, td, cbret)) < 0)
	goto done;
    if (ret == 0)
	goto fail;

     /* 7. Call plugin transaction commit callbacks */
     if (plugin_transaction_commit(h, td) < 0)
	 goto done;

     /* Optionally write (potentially modified) tree back to candidate */
     if (clicon_option_bool(h, "CLICON_TRANSACTION_MOD")){
	 if ((ret = xmldb_put(h, candidate, OP_REPLACE, td->td_target,
			      clicon_username_get(h), cbret)) < 0)
	     goto done;
	 if (ret == 0)
	     goto fail;
     }
     /* 8. Success: Copy candidate to running 
      */
     if (xmldb_copy(h, candidate, "running") < 0)
	 goto done;

    /* 9. Call plugin transaction end callbacks */
    plugin_transaction_end(h, td);

    /* 8. Copy running back to candidate in case end functions updated running 
     * XXX: room for improvement: candidate and running may be equal.
     * Copy only diffs?
     */
    if (xmldb_copy(h, "running", candidate) < 0){
	/* ignore errors or signal major setback ? */
	clicon_log(LOG_NOTICE, "Error in rollback, trying to continue");
	goto done;
    } 
    retval = 1;
 done:
     /* In case of failure (or error), call plugin transaction termination callbacks */
     if (retval < 1 && td)
	 plugin_transaction_abort(h, td);
     if (td)
	 transaction_free(td);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Commit the candidate configuration as the device's new current configuration
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 * @note NACM:    The server MUST determine the exact nodes in the running
 *  configuration datastore that are actually different and only check
 *  "create", "update", and "delete" access permissions for this set of
 *  nodes, which could be empty.
 */
int
from_client_commit(clicon_handle h,
		   cxobj        *xe,
		   cbuf         *cbret,
		   void         *arg,
		   void         *regarg)
{
    int                  retval = -1;
    struct client_entry *ce = (struct client_entry *)arg;
    int                  mypid = ce->ce_pid;
    int                  piddb;
    cbuf                *cbx = NULL; /* Assist cbuf */
    int                  ret;

    /* Check if target locked by other client */
    piddb = xmldb_islocked(h, "running");
    if (piddb && mypid != piddb){
	if ((cbx = cbuf_new()) == NULL){
	    clicon_err(OE_XML, errno, "cbuf_new");
	    goto done;
	}	
	cprintf(cbx, "<session-id>%d</session-id>", piddb);
	if (netconf_lock_denied(cbret, cbuf_get(cbx), "Operation failed, lock is already held") < 0)
	    goto done;
	goto ok;
    }
    if ((ret = candidate_commit(h, "candidate", cbret)) < 0){ /* Assume validation fail, nofatal */
	clicon_debug(1, "Commit candidate failed");
	if (ret < 0)
	    if (netconf_operation_failed(cbret, "application", clicon_err_reason)< 0)
		goto done;
        goto ok;
    }
    if (ret == 1)
	cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
 ok:
    retval = 0;
 done:
    if (cbx)
	cbuf_free(cbx);
    return retval; /* may be zero if we ignoring errors from commit */
} /* from_client_commit */

/*! Revert the candidate configuration to the current running configuration.
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval  0  OK. This may indicate both ok and err msg back to client
 * @retval     0       OK
 * @retval    -1       Error
 * NACM: No datastore permissions are needed.
 */
int
from_client_discard_changes(clicon_handle h,
			    cxobj        *xe,
			    cbuf         *cbret,
			    void         *arg,
			    void         *regarg)
{
    int                  retval = -1;
    struct client_entry *ce = (struct client_entry *)arg;
    int                  mypid = ce->ce_pid;
    int                  piddb;
    cbuf                *cbx = NULL; /* Assist cbuf */
    
    /* Check if target locked by other client */
    piddb = xmldb_islocked(h, "candidate");
    if (piddb && mypid != piddb){
	if ((cbx = cbuf_new()) == NULL){
	    clicon_err(OE_XML, errno, "cbuf_new");
	    goto done;
	}	
	cprintf(cbx, "<session-id>%d</session-id>", piddb);
	if (netconf_lock_denied(cbret, cbuf_get(cbx), "Operation failed, lock is already held") < 0)
	    goto done;
	goto ok;
    }
    if (xmldb_copy(h, "running", "candidate") < 0){
	if (netconf_operation_failed(cbret, "application", clicon_err_reason)< 0)
	    goto done;
	goto ok;
    }
    cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
 ok:
    retval = 0;
 done:
    if (cbx)
	cbuf_free(cbx);
    return retval; /* may be zero if we ignoring errors from commit */
}

/*! Cancel an ongoing confirmed commit.
 * If the confirmed commit is persistent, the parameter 'persist-id' must be
 * given, and it must match the value of the 'persist' parameter.
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval  0  OK. This may indicate both ok and err msg back to client
 * @retval     0       OK
 * @retval    -1       Error
 * @see RFC 6241 Sec 8.4
 */
int
from_client_cancel_commit(clicon_handle h,
			  cxobj        *xe,
			  cbuf         *cbret,
			  void         *arg,
			  void         *regarg)
{
    int retval = -1;
    retval = 0;
    // done:
    return retval;
}

/*! Validates the contents of the specified configuration.
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK. This may indicate both ok and err msg back to client 
 *                     (eg invalid)
 * @retval    -1       Error
 */
int
from_client_validate(clicon_handle h,
		     cxobj        *xe,
		     cbuf         *cbret,
		     void         *arg,
		     void         *regarg)
{
    int                 retval = -1;
    transaction_data_t *td = NULL;
    int                 ret;
    char               *db;

    if ((db = netconf_db_find(xe, "source")) == NULL){
	if (netconf_missing_element(cbret, "protocol", "source", NULL) < 0)
	    goto done;
	goto ok;
    }
    clicon_debug(1, "Validate %s",  db);

     /* 1. Start transaction */
    if ((td = transaction_new()) == NULL)
	goto done;
    /* Common steps (with commit) */
    if ((ret = from_validate_common(h, db, td, cbret)) < 1){
	clicon_debug(1, "Validate %s failed",  db);
	if (ret < 0){
	    if (netconf_operation_failed(cbret, "application", clicon_err_reason)< 0)
		goto done;
	}
	goto ok;
    }
    /* Optionally write (potentially modified) tree back to candidate */
    if (clicon_option_bool(h, "CLICON_TRANSACTION_MOD")){
	if ((ret = xmldb_put(h, "candidate", OP_REPLACE, td->td_target,
			     clicon_username_get(h), cbret)) < 0)
	    goto done;
	goto ok;
    }
    cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
 ok:
    retval = 0;
 done:
     if (retval < 0 && td)
	 plugin_transaction_abort(h, td);
     if (td)
	 transaction_free(td);
    return retval;
} /* from_client_validate */


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

#include "clixon_backend_handle.h"
#include "backend_plugin.h"
#include "backend_commit.h"
#include "backend_client.h"
#include "backend_handle.h"

static struct client_entry *
ce_find_bypid(struct client_entry *ce_list,
	      int pid)
{
    struct client_entry *ce;

    for (ce = ce_list; ce; ce = ce->ce_next)
	if (ce->ce_pid == pid)
	    return ce;
    return NULL;
}

/*! Stream callback for netconf stream notification (RFC 5277)
 * @param[in]  h     Clicon handle
 * @param[in]  op    0:event, 1:rm
 * @param[in]  event Event as XML
 * @param[in]  arg   Extra argument provided in stream_ss_add
 * @see stream_ss_add
 */
int
ce_event_cb(clicon_handle h,
	    int           op,
	    cxobj        *event,
	    void         *arg)
{
    struct client_entry *ce = (struct client_entry *)arg;
    
    clicon_debug(1, "%s op:%d", __FUNCTION__, op);
    switch (op){
    case 1:
	/* Risk of recursion here */
	if (ce->ce_s)
	    backend_client_rm(h, ce);
	break;
    default:
	if (send_msg_notify_xml(ce->ce_s, event) < 0){
	    if (errno == ECONNRESET || errno == EPIPE){
		clicon_log(LOG_WARNING, "client %d reset", ce->ce_nr);
	    }
	    break;
	}
    }
    return 0;
}

/*! Remove client entry state
 * Close down everything wrt clients (eg sockets, subscriptions)
 * Finally actually remove client struct in handle
 * @param[in]  h   Clicon handle
 * @param[in]  ce  Client handle
 * @see backend_client_delete for actual deallocation of client entry struct
 */
int
backend_client_rm(clicon_handle        h, 
		  struct client_entry *ce)
{
    struct client_entry   *c;
    struct client_entry   *c0;
    struct client_entry  **ce_prev;

    clicon_debug(1, "%s", __FUNCTION__);
    /* for all streams: XXX better to do it top-level? */
    stream_ss_delete_all(h, ce_event_cb, (void*)ce);
    c0 = backend_client_list(h);
    ce_prev = &c0; /* this points to stack and is not real backpointer */
    for (c = *ce_prev; c; c = c->ce_next){
	if (c == ce){
	    if (ce->ce_s){
		event_unreg_fd(ce->ce_s, from_client);
		close(ce->ce_s);
		ce->ce_s = 0;
	    }
	    break;
	}
	ce_prev = &c->ce_next;
    }

    return backend_client_delete(h, ce); /* actually purge it */
}


/*! Get streams state according to RFC 8040 or RFC5277 common function
 * @param[in]     h       Clicon handle
 * @param[in]     yspec   Yang spec
 * @param[in]     xpath   Xpath selection, not used but may be to filter early
 * @param[in]     module  Name of yang module
 * @param[in]     top     Top symbol, ie netconf or restconf-state
 * @param[in,out] xret    Existing XML tree, merge x into this
 * @retval       -1       Error (fatal)
 * @retval        0       OK
 * @retval        1       Statedata callback failed
 */
static int
client_get_streams(clicon_handle   h,
		   yang_stmt      *yspec,
		   char           *xpath,
		   char           *module,
		   char           *top,
		   cxobj         **xret)
{
    int            retval = -1;
    yang_stmt     *ystream = NULL; /* yang stream module */
    yang_stmt     *yns = NULL;  /* yang namespace */
    cxobj         *x = NULL;
    cbuf          *cb = NULL;

    if ((ystream = yang_find(yspec, Y_MODULE, module)) == NULL){
	clicon_err(OE_YANG, 0, "%s yang module not found", module);
	goto done;
    }
    if ((yns = yang_find(ystream, Y_NAMESPACE, NULL)) == NULL){
	clicon_err(OE_YANG, 0, "%s yang namespace not found", module);
	goto done;
    }
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, 0, "clicon buffer");
	goto done;
    }
    cprintf(cb,"<%s xmlns=\"%s\">", top, yang_argument_get(yns));
    if (stream_get_xml(h, strcmp(top,"restconf-state")==0, cb) < 0)
	goto done;
    cprintf(cb,"</%s>", top);

    if (xml_parse_string(cbuf_get(cb), yspec, &x) < 0){
	if (netconf_operation_failed_xml(xret, "protocol", clicon_err_reason)< 0)
	    goto done;
	retval = 1;
	goto done;
    }
    retval = netconf_trymerge(x, yspec, xret);
 done:
    if (cb)
	cbuf_free(cb);
    if (x)
	xml_free(x);
    return retval;
}

/*! Get system state-data, including streams and plugins
 * @param[in]     h       Clicon handle
 * @param[in]     xpath   Xpath selection, not used but may be to filter early
 * @param[in,out] xret    Existing XML tree, merge x into this
 * @retval       -1       Error (fatal)
 * @retval        0       OK
 * @retval        1       Statedata callback failed (clicon_err called)
 */
static int
client_statedata(clicon_handle h,
		 char         *xpath,
		 cxobj       **xret)
{
    int        retval = -1;
    cxobj    **xvec = NULL;
    size_t     xlen;
    int        i;
    yang_stmt *yspec;

    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_YANG, ENOENT, "No yang spec");
	goto done;
    }
    if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC5277"))
	if ((retval = client_get_streams(h, yspec, xpath, "clixon-rfc5277", "netconf", xret)) != 0)
	    goto done;
    if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC8040"))
	if ((retval = client_get_streams(h, yspec, xpath, "ietf-restconf-monitoring", "restconf-state", xret)) != 0)
	    goto done;
    if (clicon_option_bool(h, "CLICON_MODULE_LIBRARY_RFC7895"))
	if ((retval = yang_modules_state_get(h, yspec, xpath, 0, xret)) != 0)
	    goto done;
    if ((retval = clixon_plugin_statedata(h, yspec, xpath, xret)) != 0)
	goto done;
    /* Code complex to filter out anything that is outside of xpath */
    if (xpath_vec(*xret, "%s", &xvec, &xlen, xpath?xpath:"/") < 0)
	goto done;
    /* If vectors are specified then mark the nodes found and
     * then filter out everything else,
     * otherwise return complete tree.
     */
    if (xvec != NULL){
	for (i=0; i<xlen; i++)
	    xml_flag_set(xvec[i], XML_FLAG_MARK);
    }
    /* Remove everything that is not marked */
    if (!xml_flag(*xret, XML_FLAG_MARK))
	if (xml_tree_prune_flagged_sub(*xret, XML_FLAG_MARK, 1, NULL) < 0)
	    goto done;
    /* reset flag */
    if (xml_apply(*xret, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)XML_FLAG_MARK) < 0)
	goto done;
    retval = 0; /* OK */
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    if (xvec)
	free(xvec);
    return retval;
}

/*! Retrieve all or part of a specified configuration.
 * 
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
from_client_get_config(clicon_handle h,
		       cxobj        *xe,
		       cbuf         *cbret,
		       void         *arg,
		       void         *regarg)
{
    int     retval = -1;
    char   *db;
    cxobj  *xfilter;
    char   *xpath = "/";
    cxobj  *xret = NULL;
    cbuf   *cbx = NULL; /* Assist cbuf */
    cxobj  *xnacm = NULL;
    cxobj **xvec = NULL;
    size_t  xlen;    
    int     ret;
    char   *username;

    username = clicon_username_get(h);
    if ((db = netconf_db_find(xe, "source")) == NULL){
	clicon_err(OE_XML, 0, "db not found");
	goto done;
    }
    if (xmldb_validate_db(db) < 0){
	if ((cbx = cbuf_new()) == NULL){
	    clicon_err(OE_XML, errno, "cbuf_new");
	    goto done;
	}	
	cprintf(cbx, "No such database: %s", db);
	if (netconf_invalid_value(cbret, "protocol", cbuf_get(cbx))< 0)
	    goto done;
	goto ok;
    }
    if ((xfilter = xml_find(xe, "filter")) != NULL)
	if ((xpath = xml_find_value(xfilter, "select"))==NULL)
	    xpath="/";
    if (xmldb_get(h, db, xpath, &xret, NULL) < 0){
	if (netconf_operation_failed(cbret, "application", "read registry")< 0)
	    goto done;
	goto ok;
    }
    /* Pre-NACM access step */
    if ((ret = nacm_access_pre(h, username, NACM_DATA, &xnacm)) < 0)
	goto done;
    if (ret == 0){ /* Do NACM validation */
	if (xpath_vec(xret, "%s", &xvec, &xlen, xpath?xpath:"/") < 0)
	    goto done;
	/* NACM datanode/module read validation */
	if (nacm_datanode_read(xret, xvec, xlen, username, xnacm) < 0) 
	    goto done;
    }
    cprintf(cbret, "<rpc-reply>");
    if (xret==NULL)
	cprintf(cbret, "<data/>");
    else{
	if (xml_name_set(xret, "data") < 0)
	    goto done;
	if (clicon_xml2cbuf(cbret, xret, 0, 0) < 0)
	    goto done;
    }
    cprintf(cbret, "</rpc-reply>");
 ok:
    retval = 0;
 done:
    if (xnacm)
	xml_free(xnacm);
    if (xvec)
	free(xvec);
    if (cbx)
	cbuf_free(cbx);
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Loads all or part of a specified configuration to target configuration
 * 
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
from_client_edit_config(clicon_handle h,
			cxobj        *xn,
			cbuf         *cbret,
			void         *arg,
			void         *regarg)
{
    int                 retval = -1;
    struct client_entry *ce = (struct client_entry *)arg;
    int                 mypid = ce->ce_pid;
    char               *target;
    cxobj              *xc;
    cxobj              *x;
    enum operation_type operation = OP_MERGE;
    int                 piddb;
    int                 non_config = 0;
    yang_stmt          *yspec;
    cbuf               *cbx = NULL; /* Assist cbuf */
    int                 ret;
    char               *username;

    username = clicon_username_get(h);
    if ((yspec =  clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_YANG, ENOENT, "No yang spec9");
	goto done;
    }
    if ((target = netconf_db_find(xn, "target")) == NULL){
	if (netconf_missing_element(cbret, "protocol", "target", NULL) < 0)
	    goto done;
	goto ok;
    }
    if ((cbx = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }	
    if (xmldb_validate_db(target) < 0){
	cprintf(cbx, "No such database: %s", target);
	if (netconf_invalid_value(cbret, "protocol", cbuf_get(cbx))< 0)
	    goto done;
	goto ok;
    }
    /* Check if target locked by other client */
    piddb = xmldb_islocked(h, target);
    if (piddb && mypid != piddb){
	cprintf(cbx, "<session-id>%d</session-id>", piddb);
	if (netconf_lock_denied(cbret, cbuf_get(cbx), "Operation failed, lock is already held") < 0)
	    goto done;
	goto ok;
    }
    if ((x = xpath_first(xn, "default-operation")) != NULL){
	if (xml_operation(xml_body(x), &operation) < 0){
	    if (netconf_invalid_value(cbret, "protocol", "Wrong operation")< 0)
		goto done;
	    goto ok;
	}
    }
    if ((xc = xpath_first(xn, "config")) == NULL){
	if (netconf_missing_element(cbret, "protocol", "config", NULL) < 0)
	    goto done;
	goto ok;
    }
    else{
	/* <config> yang spec may be set to anyxml by ingress yang check,...*/
	if (xml_spec(xc) != NULL)
	    xml_spec_set(xc, NULL);
	/* Populate XML with Yang spec (why not do this in parser?) 
	 * Maybe validate xml here as in text_modify_top?
	 */
	if (xml_apply(xc, CX_ELMNT, xml_spec_populate, yspec) < 0)
	    goto done;

	if (xml_apply(xc, CX_ELMNT, xml_non_config_data, &non_config) < 0)
	    goto done;
	if (non_config){
	    if (netconf_invalid_value(cbret, "protocol", "State data not allowed")< 0)
		goto done;
	    goto ok;
	}
	/* xmldb_put (difflist handling) requires list keys */
	if ((ret = xml_yang_validate_list_key_only(xc, cbret)) < 0)
	    goto done;
	if (ret == 0)
	    goto ok;
	/* Cant do this earlier since we dont have a yang spec to
	 * the upper part of the tree, until we get the "config" tree.
	 */
	if (xml_apply0(xc, CX_ELMNT, xml_sort, NULL) < 0)
	    goto done;
	if ((ret = xmldb_put(h, target, operation, xc, username, cbret)) < 0){
	    clicon_debug(1, "%s ERROR PUT", __FUNCTION__);	
	    if (netconf_operation_failed(cbret, "protocol", clicon_err_reason)< 0)
		goto done;
	    goto ok;
	}
	if (ret == 0)
	    goto ok;
    }
    assert(cbuf_len(cbret) == 0);
    cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
 ok:
    retval = 0;
 done:
    if (cbx)
	cbuf_free(cbx);
    clicon_debug(1, "%s done cbret:%s", __FUNCTION__, cbuf_get(cbret));	
    return retval;
    
} /* from_client_edit_config */

/*! Create or replace an entire config with another complete config db
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 * NACM: If source running and target startup --> only exec permission
 * else: 
 * - omit data nodes to which the client does not have read access
 * - access denied if user lacks create/delete/update
 */
static int
from_client_copy_config(clicon_handle h,
			cxobj        *xe,
			cbuf         *cbret,
			void         *arg,
			void         *regarg)
{
    int    retval = -1;
    struct client_entry *ce = (struct client_entry *)arg;
    char  *source;
    char  *target;
    int    piddb;
    int    mypid = ce->ce_pid;
    cbuf  *cbx = NULL; /* Assist cbuf */
    
    if ((source = netconf_db_find(xe, "source")) == NULL){
	if (netconf_missing_element(cbret, "protocol", "source", NULL) < 0)
	    goto done;
	goto ok;
    }
    if ((cbx = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }	
    if (xmldb_validate_db(source) < 0){
	cprintf(cbx, "No such database: %s", source);
	if (netconf_invalid_value(cbret, "protocol", cbuf_get(cbx))< 0)
	    goto done;
	goto ok;
    }
    if ((target = netconf_db_find(xe, "target")) == NULL){
	if (netconf_missing_element(cbret, "protocol", "target", NULL) < 0)
	    goto done;
	goto ok;
    }
    if (xmldb_validate_db(target) < 0){
	cprintf(cbx, "No such database: %s", target);
	if (netconf_invalid_value(cbret, "protocol", cbuf_get(cbx))< 0)
	    goto done;
	goto ok;
    }
    /* Check if target locked by other client */
    piddb = xmldb_islocked(h, target);
    if (piddb && mypid != piddb){
	cprintf(cbx, "<session-id>%d</session-id>", piddb);
	if (netconf_lock_denied(cbret, cbuf_get(cbx), "Copy failed, lock is already held") < 0)
	    goto done;
	goto ok;
    }
    if (xmldb_copy(h, source, target) < 0){
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
    return retval;
}

/*! Delete a configuration datastore.
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
from_client_delete_config(clicon_handle h,
			  cxobj        *xe,
			  cbuf         *cbret,
			  void         *arg, 
			  void         *regarg)
{
    int                  retval = -1;
    struct client_entry *ce = (struct client_entry *)arg;
    char                *target;
    int                  piddb;
    cbuf                *cbx = NULL; /* Assist cbuf */
    int                  mypid = ce->ce_pid;

    if ((target = netconf_db_find(xe, "target")) == NULL ||
	strcmp(target, "running")==0){
	if (netconf_missing_element(cbret, "protocol", "target", NULL) < 0)
	    goto done;
	goto ok;
    }
    if ((cbx = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }	
    if (xmldb_validate_db(target) < 0){
	cprintf(cbx, "No such database: %s", target);
	if (netconf_invalid_value(cbret, "protocol", cbuf_get(cbx))< 0)
	    goto done;
	goto ok;
    }
    /* Check if target locked by other client */
    piddb = xmldb_islocked(h, target);
    if (piddb && mypid != piddb){
	cprintf(cbx, "<session-id>%d</session-id>", piddb);
	if (netconf_lock_denied(cbret, cbuf_get(cbx), "Operation failed, lock is already held") < 0)
	    goto done;
	goto ok;
    }
    if (xmldb_delete(h, target) < 0){
	if (netconf_operation_failed(cbret, "protocol", clicon_err_reason)< 0)
	    goto done;
	goto ok;
    }
    if (xmldb_create(h, target) < 0){
	if (netconf_operation_failed(cbret, "protocol", clicon_err_reason)< 0)
	    goto done;
	goto ok;
    }
    cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
 ok:
    retval = 0;
  done:
    if (cbx)
	cbuf_free(cbx);
    return retval;
}

/*! Lock the configuration system of a device
 * 
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
from_client_lock(clicon_handle h,
		 cxobj        *xe,
		 cbuf         *cbret,
		 void         *arg, 
		 void         *regarg)
{
    int                  retval = -1;
    struct client_entry *ce = (struct client_entry *)arg;
    int                  pid = ce->ce_pid;
    char                *db;
    int                  piddb;
    cbuf                *cbx = NULL; /* Assist cbuf */
    
    if ((db = netconf_db_find(xe, "target")) == NULL){
	if (netconf_missing_element(cbret, "protocol", "target", NULL) < 0)
	    goto done;
	goto ok;
    }
    if ((cbx = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }	
    if (xmldb_validate_db(db) < 0){
	cprintf(cbx, "No such database: %s", db);
	if (netconf_invalid_value(cbret, "protocol", cbuf_get(cbx))< 0)
	    goto done;
	goto ok;
    }
    /*
     * A lock MUST not be granted if either of the following conditions is true:
     * 1) A lock is already held by any NETCONF session or another entity.
     * 2) The target configuration is <candidate>, it has already been modified, and 
     *    these changes have not been committed or rolled back.
     */
    piddb = xmldb_islocked(h, db);
    if (piddb){
	cprintf(cbx, "<session-id>%d</session-id>", piddb);
	if (netconf_lock_denied(cbret, cbuf_get(cbx), "Operation failed, lock is already held") < 0)
	    goto done;
	goto ok;
    }
    else{
	if (xmldb_lock(h, db, pid) < 0)
	    goto done;
	cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
    }
 ok:
    retval = 0;
 done:
    if (cbx)
	cbuf_free(cbx);
    return retval;
}

/*! Release a configuration lock previously obtained with the 'lock' operation
 * 
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
from_client_unlock(clicon_handle h,
		   cxobj        *xe,
		   cbuf         *cbret,
		   void         *arg,
		   void         *regarg)
{
    int                  retval = -1;
    struct client_entry *ce = (struct client_entry *)arg;
    int                  pid = ce->ce_pid;
    char                *db;
    int                  piddb;
    cbuf                *cbx = NULL; /* Assist cbuf */

    if ((db = netconf_db_find(xe, "target")) == NULL){
	if (netconf_missing_element(cbret, "protocol", "target", NULL) < 0)
	    goto done;
	goto ok;
    }
    if ((cbx = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }	
    if (xmldb_validate_db(db) < 0){
	cprintf(cbx, "No such database: %s", db);
	if (netconf_invalid_value(cbret, "protocol", cbuf_get(cbx))< 0)
	    goto done;
	goto ok;
    }
    piddb = xmldb_islocked(h, db);
    /* 
     * An unlock operation will not succeed if any of the following
     * conditions are true:
     * 1) the specified lock is not currently active
     * 2) the session issuing the <unlock> operation is not the same
     *    session that obtained the lock
     */
    if (piddb==0 || piddb != pid){
	cprintf(cbx, "<session-id>pid=%d piddb=%d</session-id>", pid, piddb);
	if (netconf_lock_denied(cbret, cbuf_get(cbx), "Unlock failed, lock is already held") < 0)
	    goto done;
	goto ok;
    }
    else{
	xmldb_unlock(h, db);
	if (cprintf(cbret, "<rpc-reply><ok/></rpc-reply>") < 0)
	    goto done;
    }
 ok:
    retval = 0;
 done:
    if (cbx)
	cbuf_free(cbx);
    return retval;
}

/*! Retrieve running configuration and device state information.
 * 
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 *
 * @see from_client_get_config
 */
static int
from_client_get(clicon_handle h,
		cxobj        *xe,
		cbuf         *cbret,
		void         *arg, 
		void         *regarg)
{
    int     retval = -1;
    cxobj  *xfilter;
    char   *xpath = "/";
    cxobj  *xret = NULL;
    int     ret;
    cxobj **xvec = NULL;
    size_t  xlen;    
    cxobj  *xnacm = NULL;
    char               *username;

    username = clicon_username_get(h);
    if ((xfilter = xml_find(xe, "filter")) != NULL)
	if ((xpath = xml_find_value(xfilter, "select"))==NULL)
	    xpath="/";
    /* Get config */
    if (xmldb_get(h, "running", xpath, &xret, NULL) < 0){
	if (netconf_operation_failed(cbret, "application", "read registry")< 0)
	    goto done;
	goto ok;
    }
    /* Get state data from plugins as defined by plugin_statedata(), if any */
    assert(xret);
    clicon_err_reset();
    if ((ret = client_statedata(h, xpath, &xret)) < 0)
	goto done;
    if (ret == 1){ /* Error from callback (error in xret) */
	if (clicon_xml2cbuf(cbret, xret, 0, 0) < 0)
	    goto done;
	goto ok;
    }
    /* Pre-NACM access step */
    if ((ret = nacm_access_pre(h, username, NACM_DATA, &xnacm)) < 0)
	goto done;
    if (ret == 0){ /* Do NACM validation */
	if (xpath_vec(xret, "%s", &xvec, &xlen, xpath?xpath:"/") < 0)
	    goto done;
	/* NACM datanode/module read validation */
	if (nacm_datanode_read(xret, xvec, xlen, username, xnacm) < 0) 
	    goto done;
    }
    cprintf(cbret, "<rpc-reply>");     /* OK */
    if (xret==NULL)
	cprintf(cbret, "<data/>");
    else{
	if (xml_name_set(xret, "data") < 0)
	    goto done;
	if (clicon_xml2cbuf(cbret, xret, 0, 0) < 0)
	    goto done;
    }
    cprintf(cbret, "</rpc-reply>");
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    if (xnacm)
	xml_free(xnacm);
    if (xvec)
	free(xvec);
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Request graceful termination of a NETCONF session.
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
from_client_close_session(clicon_handle h,
			 cxobj        *xe,
			 cbuf         *cbret,
			 void         *arg, 
			 void         *regarg)
{
    struct client_entry *ce = (struct client_entry *)arg;
    int                  pid = ce->ce_pid;

    xmldb_unlock_all(h, pid);
    stream_ss_delete_all(h, ce_event_cb, (void*)ce);
    cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
    return 0;
}

/*! Internal message:  Force the termination of a NETCONF session.
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
from_client_kill_session(clicon_handle h,
			 cxobj        *xe,
			 cbuf         *cbret,
			 void         *arg,
			 void         *regarg)
{
    int                  retval = -1;
    uint32_t             pid; /* other pid */
    char                *str;
    struct client_entry *ce;
    char                *db = "running"; /* XXX */
    cxobj               *x;
    
    if ((x = xml_find(xe, "session-id")) == NULL ||
	(str = xml_find_value(x, "body")) == NULL){
	if (netconf_missing_element(cbret, "protocol", "session-id", NULL) < 0)
	    goto done;
	goto ok;
    }
    pid = atoi(str);
    /* may or may not be in active client list, probably not */
    if ((ce = ce_find_bypid(backend_client_list(h), pid)) != NULL){
	xmldb_unlock_all(h, pid);	    
	backend_client_rm(h, ce);
    }
    
    if (kill (pid, 0) != 0 && errno == ESRCH) /* Nothing there */
	;
    else{
	killpg(pid, SIGTERM);
	kill(pid, SIGTERM);
#if 0 /* Hate sleeps we assume it died, see also 0 in next if.. */
	sleep(1);
#endif
    }
    if (1 || (kill (pid, 0) != 0 && errno == ESRCH)){ /* Nothing there */
	/* clear from locks */
	if (xmldb_islocked(h, db) == pid)
	    xmldb_unlock(h, db);
    }
    else{ /* failed to kill client */
	    if (netconf_operation_failed(cbret, "application", "Failed to kill session")< 0)
		goto done;
	goto ok;
    }
    cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Create a notification subscription
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 * @see RFC5277 2.1
 * @example:
 *    <create-subscription> 
 *       <stream>RESULT</stream> # If not present, events in the default NETCONF stream will be sent.
 *       <filter type="xpath" select="XPATH-EXPR"/>
 *       <startTime></startTime>
 *       <stopTime></stopTime>
 *    </create-subscription> 
 */
static int
from_client_create_subscription(clicon_handle h,
				cxobj        *xe,
				cbuf         *cbret,
				void         *arg, 
				void         *regarg)
{
    int                  retval = -1;
    struct client_entry *ce = (struct client_entry *)arg;
    char                *stream = "NETCONF";
    cxobj               *x; /* Generic xml tree */
    cxobj               *xfilter; /* Filter xml tree */
    char                *ftype;
    char                *starttime = NULL;
    char                *stoptime = NULL;
    char                *selector = NULL;
    struct timeval       start;
    struct timeval       stop;
    
    if ((x = xpath_first(xe, "//stream")) != NULL)
	stream = xml_find_value(x, "body");
    if ((x = xpath_first(xe, "//stopTime")) != NULL){
	if ((stoptime = xml_find_value(x, "body")) != NULL &&
	    str2time(stoptime, &stop) < 0){
	    if (netconf_bad_element(cbret, "application", "stopTime", "Expected timestamp") < 0)
		goto done;
	    goto ok;	
	}
    }
    if ((x = xpath_first(xe, "//startTime")) != NULL){
	if ((starttime = xml_find_value(x, "body")) != NULL &&
	    str2time(starttime, &start) < 0){
	    if (netconf_bad_element(cbret, "application", "startTime", "Expected timestamp") < 0)
		goto done;
	    goto ok;	
	}	
    }
    if ((xfilter = xpath_first(xe, "//filter")) != NULL){
	if ((ftype = xml_find_value(xfilter, "type")) != NULL){
	    /* Only accept xpath as filter type */
	    if (strcmp(ftype, "xpath") != 0){
		if (netconf_operation_failed(cbret, "application", "Only xpath filter type supported")< 0)
		    goto done;
		goto ok;
	    }
	    if ((selector = xml_find_value(xfilter, "select")) == NULL)
		goto done;
	}
    }
    if ((stream_find(h, stream)) == NULL){
	if (netconf_invalid_value(cbret, "application", "No such stream") < 0)
	    goto done;
	goto ok;
    }
    /* Add subscriber to stream - to make notifications for this client */
    if (stream_ss_add(h, stream, selector,
		      starttime?&start:NULL, stoptime?&stop:NULL,
		      ce_event_cb, (void*)ce) < 0)
	goto done;
    /* Replay of this stream to specific subscription according to start and 
     * stop (if present). 
     * RFC 5277: If <startTime> is not present, this is not a replay
     * subscription.
     * Schedule the replay to occur right after this RPC completes, eg "now"
     */
    if (starttime){ 
	if (stream_replay_trigger(h, stream, ce_event_cb, (void*)ce) < 0)
	    goto done;
    }
    cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
 ok:
    retval = 0;
  done:
    return retval;
}

/*! Set debug level. 
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
from_client_debug(clicon_handle h,
		  cxobj        *xe,
		  cbuf         *cbret,
		  void         *arg,
		  void         *regarg)
{
    int      retval = -1;
    uint32_t level;
    char    *valstr;
    
    if ((valstr = xml_find_body(xe, "level")) == NULL){
	if (netconf_missing_element(cbret, "application", "level", NULL) < 0)
	    goto done;
	goto ok;
    }
    level = atoi(valstr);

    clicon_debug_init(level, NULL); /* 0: dont debug, 1:debug */
    setlogmask(LOG_UPTO(level?LOG_DEBUG:LOG_INFO)); /* for syslog */
    clicon_log(LOG_NOTICE, "%s debug:%d", __FUNCTION__, debug);
    cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
 ok:
    retval = 0;
 done:
    return retval;
}

/*! An internal clicon message has arrived from a client. Receive and dispatch.
 * @param[in]   h    Clicon handle
 * @param[in]   s    Socket where message arrived. read from this.
 * @param[in]   arg  Client entry (from).
 * @retval      0    OK
 * @retval      -1   Error Terminates backend and is never called). Instead errors are
 *                   propagated back to client.
 */
static int
from_client_msg(clicon_handle        h,
		struct client_entry *ce, 
		struct clicon_msg   *msg)
{
    int                  retval = -1;
    cxobj               *xt = NULL;
    cxobj               *x;
    cxobj               *xe;
    char                *rpc = NULL;
    char                *module = NULL;
    cbuf                *cbret = NULL; /* return message */
    int                  ret;
    char                *username;
    yang_stmt           *yspec;
    yang_stmt           *ye;
    yang_stmt           *ymod;
    cxobj               *xnacm = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    yspec = clicon_dbspec_yang(h); 
    /* Return netconf message. Should be filled in by the dispatch(sub) functions 
     * as wither rpc-error or by positive response.
     */
    if ((cbret = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if (clicon_msg_decode(msg, yspec, &xt) < 0){
	if (netconf_malformed_message(cbret, "XML parse error")< 0)
	    goto done;
	goto reply;
    }

    if ((x = xpath_first(xt, "/rpc")) == NULL){
	if (netconf_malformed_message(cbret, "rpc keyword expected")< 0)
	    goto done;
	goto reply;
    }
    /* Populate incoming XML tree with yang - 
     * should really have been dealt with by decode above
     * maybe not necessary since it should be */
    if (xml_spec_populate_rpc(h, x, yspec) < 0)
    	goto done;
    if ((ret = xml_yang_validate_rpc(x, cbret)) < 0)
	goto done;
    if (ret == 0)
	goto reply;
    xe = NULL;
    username = xml_find_value(x, "username");
    /* May be used by callbacks, etc */
    clicon_username_set(h, username);
    while ((xe = xml_child_each(x, xe, CX_ELMNT)) != NULL) {
	rpc = xml_name(xe);
	if ((ye = xml_spec(xe)) == NULL){
	    if (netconf_operation_not_supported(cbret, "protocol", rpc) < 0)
		goto done;
	    goto reply;
	}
	if ((ymod = ys_module(ye)) == NULL){
	    clicon_err(OE_XML, ENOENT, "rpc yang does not have module");
	    goto done;
	}
	module = yang_argument_get(ymod);
	clicon_debug(1, "%s module:%s rpc:%s", __FUNCTION__, module, rpc);
	/* Pre-NACM access step */
	xnacm = NULL;
	if ((ret = nacm_access_pre(h, username, NACM_RPC, &xnacm)) < 0)
	    goto done;
	if (ret == 0){ /* Do NACM validation */
	    /* NACM rpc operation exec validation */
	    if ((ret = nacm_rpc(rpc, module, username, xnacm, cbret)) < 0)
		goto done;
	    if (xnacm)
		xml_free(xnacm);
	    if (ret == 0) /* Not permitted and cbret set */
		goto reply;
	}
	clicon_err_reset();
	if ((ret = rpc_callback_call(h, xe, cbret, ce)) < 0){
	    if (netconf_operation_failed(cbret, "application", clicon_err_reason)< 0)
		goto done;
	    clicon_log(LOG_NOTICE, "%s Error in rpc_callback_call:%s", __FUNCTION__, xml_name(xe));
	    goto reply; /* Dont quit here on user callbacks */
	}
	if (ret == 0){ /* not handled by callback */
	    if (netconf_operation_failed(cbret, "application", "Callback not recognized")< 0)
		goto done;
	    goto reply;
	}
    }
 reply:
    if (cbuf_len(cbret) == 0)
	if (netconf_operation_failed(cbret, "application", clicon_errno?clicon_err_reason:"unknown")< 0)
	    goto done;
    clicon_debug(1, "%s cbret:%s", __FUNCTION__, cbuf_get(cbret));
    /* XXX problem here is that cbret has not been parsed so may contain 
       parse errors */
    if (send_msg_reply(ce->ce_s, cbuf_get(cbret), cbuf_len(cbret)+1) < 0){
	switch (errno){
	case EPIPE:
	    /* man (2) write: 
	     * EPIPE  fd is connected to a pipe or socket whose reading end is 
	     * closed.  When this happens the writing process will also receive 
	     * a SIGPIPE signal. 
	     * In Clixon this means a client, eg restconf, netconf or cli closes
	     * the (UNIX domain) socket.
	     */
	case ECONNRESET:
	    clicon_log(LOG_WARNING, "client rpc reset");
	    break;
	default:
	    goto done;
	}
    }
    // ok:
    retval = 0;
  done:  
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    if (xt)
	xml_free(xt);
    if (cbret)
	cbuf_free(cbret);
    /* Sanity: log if clicon_err() is not called ! */
    if (retval < 0 && clicon_errno < 0) 
	clicon_log(LOG_NOTICE, "%s: Internal error: No clicon_err call on error (message: %s)",
		   __FUNCTION__, rpc?rpc:"");
    //    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    return retval;// -1 here terminates backend
}

/*! An internal clicon message has arrived from a client. Receive and dispatch.
 * @param[in]   s    Socket where message arrived. read from this.
 * @param[in]   arg  Client entry (from).
 * @retval      0    OK
 * @retval      -1   Error Terminates backend and is never called). Instead errors are
 *                   propagated back to client.
 */
int
from_client(int   s, 
	    void* arg)
{
    int                  retval = -1;
    struct clicon_msg   *msg = NULL;
    struct client_entry *ce = (struct client_entry *)arg;
    clicon_handle        h = ce->ce_handle;
    int                  eof;

    clicon_debug(1, "%s", __FUNCTION__);
    // assert(s == ce->ce_s);
    if (clicon_msg_rcv(ce->ce_s, &msg, &eof) < 0)
	goto done;
    if (eof)
	backend_client_rm(h, ce); 
    else
	if (from_client_msg(h, ce, msg) < 0)
	    goto done;
    retval = 0;
  done:
    clicon_debug(1, "%s retval=%d", __FUNCTION__, retval);
    if (msg)
	free(msg);
    return retval; /* -1 here terminates backend */
}

/*! Init backend rpc: Set up standard netconf rpc callbacks
 * @param[in]  h     Clicon handle
 * @retval       -1       Error (fatal)
 * @retval        0       OK
 * @see ietf-netconf@2011-06-01.yang 
 */
int
backend_rpc_init(clicon_handle h)
{
    int retval = -1;

    /* In backend_client.? RFC 6241 */
    if (rpc_callback_register(h, from_client_get_config, NULL,
		      "urn:ietf:params:xml:ns:netconf:base:1.0", "get-config") < 0)
	goto done;
    if (rpc_callback_register(h, from_client_edit_config, NULL,
		      "urn:ietf:params:xml:ns:netconf:base:1.0", "edit-config") < 0)
	goto done;
    if (rpc_callback_register(h, from_client_copy_config, NULL,
		      "urn:ietf:params:xml:ns:netconf:base:1.0", "copy-config") < 0)
	goto done;
    if (rpc_callback_register(h, from_client_delete_config, NULL,
		      "urn:ietf:params:xml:ns:netconf:base:1.0", "delete-config") < 0)
	goto done;
    if (rpc_callback_register(h, from_client_lock, NULL,
		      "urn:ietf:params:xml:ns:netconf:base:1.0", "lock") < 0)
	goto done;
    if (rpc_callback_register(h, from_client_unlock, NULL,
		      "urn:ietf:params:xml:ns:netconf:base:1.0", "unlock") < 0)
	goto done;
    if (rpc_callback_register(h, from_client_get, NULL,
		      "urn:ietf:params:xml:ns:netconf:base:1.0", "get") < 0)
	goto done;
    if (rpc_callback_register(h, from_client_close_session, NULL,
		      "urn:ietf:params:xml:ns:netconf:base:1.0", "close-session") < 0)
	goto done;
    if (rpc_callback_register(h, from_client_kill_session, NULL,
		      "urn:ietf:params:xml:ns:netconf:base:1.0", "kill-session") < 0)
	goto done;
    /* In backend_commit.? */
    if (rpc_callback_register(h, from_client_commit, NULL,
		      "urn:ietf:params:xml:ns:netconf:base:1.0", "commit") < 0)
	goto done;
    if (rpc_callback_register(h, from_client_discard_changes, NULL,
		      "urn:ietf:params:xml:ns:netconf:base:1.0", "discard-changes") < 0)
	goto done;
    /* if-feature confirmed-commit */
    if (rpc_callback_register(h, from_client_cancel_commit, NULL,
		      "urn:ietf:params:xml:ns:netconf:base:1.0", "cancel-commit") < 0)
	goto done;
    /* if-feature validate */
    if (rpc_callback_register(h, from_client_validate, NULL,
		      "urn:ietf:params:xml:ns:netconf:base:1.0", "validate") < 0)
	goto done;

    /* In backend_client.? RPC from RFC 5277 */
    if (rpc_callback_register(h, from_client_create_subscription, NULL,
		      "urn:ietf:params:xml:ns:netmod:notification", "create-subscription") < 0)
	goto done;
    /* In backend_client.? Clixon RPC */
    if (rpc_callback_register(h, from_client_debug, NULL,
			      "http://clicon.org/lib", "debug") < 0)
	goto done;
    retval =0;
 done:
    return retval;
}

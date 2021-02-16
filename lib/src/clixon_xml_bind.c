/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2021 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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

 *
 * Translation / mapping code between formats
 */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <syslog.h>
#include <fcntl.h>
#include <assert.h>
#include <arpa/inet.h>
#include <sys/param.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */

#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_string.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_yang_module.h"
#include "clixon_plugin.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_log.h"
#include "clixon_err.h"
#include "clixon_netconf_lib.h"
#include "clixon_xml_sort.h"
#include "clixon_yang_type.h"
#include "clixon_xml_bind.h"

/*
 * Local variables
 */
static int _yang_unknown_anydata = 0;

/*! Kludge to equate unknown XML with anydata
 * The problem with this is that its global and should be bound to a handle
 */
int
xml_bind_yang_unknown_anydata(int val)
{
    _yang_unknown_anydata = val;
    return 0;
}

/*! After yang binding, bodies of containers and lists are stripped from XML bodies
 * May apply to other nodes?
 */
static int
strip_whitespace(cxobj *xt)
{
    yang_stmt    *yt;
    enum rfc_6020 keyword;
    cxobj        *xc;
    
    if ((yt = xml_spec(xt)) != NULL){
	keyword = yang_keyword_get(yt);
	if (keyword == Y_LIST || keyword == Y_CONTAINER){
	    xc = NULL;
	    while ((xc = xml_find_type(xt, NULL, "body", CX_BODY)) != NULL)
		xml_purge(xc);
	}
    }
    return 0;
}

/*! Associate XML node x with x:s parents yang:s matching child
 *
 * @param[in]   xt     XML tree node
 * @param[out]  xerr   Reason for failure, or NULL
 * @retval      1      OK Yang assignment made
 * @retval      2      OK Yang assignment not made because yang parent is anyxml or anydata
 * @retval      0      Yang assigment not made and xerr set
 * @retval     -1      Error
 * @note retval = 2 is special
 * @see populate_self_top
 */
static int
populate_self_parent(cxobj  *xt,
		     cxobj  *xsibling,
		     cxobj **xerr)
{
    int        retval = -1;
    yang_stmt *y = NULL;     /* yang node */
    yang_stmt *yparent;      /* yang parent */
    cxobj     *xp = NULL;    /* xml parent */
    char      *name;
    char      *ns = NULL;    /* XML namespace of xt */
    char      *nsy = NULL;   /* Yang namespace of xt */
    cbuf      *cb = NULL;

    name = xml_name(xt);
    /* optimization for massive lists - use the first element as role model */
    if (xsibling &&
	xml_child_nr_type(xt, CX_ATTR) == 0){
	y = xml_spec(xsibling);
	goto set;
    }
    xp = xml_parent(xt);
    if (xp == NULL){
	if (xerr &&
	    netconf_bad_element_xml(xerr, "application", name, "Missing parent") < 0)
	    goto done;
	goto fail;
    }
    if ((yparent = xml_spec(xp)) == NULL){
	if (xerr &&
	    netconf_bad_element_xml(xerr, "application", name, "Missing parent yang node") < 0)
	    goto done;
	goto fail;
    }
    if (yang_keyword_get(yparent) == Y_ANYXML || yang_keyword_get(yparent) == Y_ANYDATA){
	retval = 2;
	goto done;
    }
    if (xml2ns(xt, xml_prefix(xt), &ns) < 0)
	goto done;
    if ((y = yang_find_datanode(yparent, name)) == NULL){
	if (_yang_unknown_anydata){
	    /* Add dummy Y_ANYDATA yang stmt, see ysp_add */
	    if ((y = yang_anydata_add(yparent, name)) < 0)
		goto done;
	    xml_spec_set(xt, y);
	    retval = 2; /* treat as anydata */
	    clicon_log(LOG_WARNING,
		       "%s: %d: No YANG spec for %s, anydata used",
		       __FUNCTION__, __LINE__, name);
	    goto done;
	}
	if ((cb = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	cprintf(cb, "Failed to find YANG spec of XML node: %s", name);
	cprintf(cb, " with parent: %s", xml_name(xp));
	if (ns)
	    cprintf(cb, " in namespace: %s", ns);
	if (xerr &&
	    netconf_unknown_element_xml(xerr, "application", name, cbuf_get(cb)) < 0)
	    goto done;
	goto fail;
    }
    nsy = yang_find_mynamespace(y);
    if (ns == NULL || nsy == NULL){
	if (xerr &&
	    netconf_bad_element_xml(xerr, "application", name, "Missing namespace") < 0)
	    goto done;
	goto fail;
    }
    /* Assign spec only if namespaces match */
    if (strcmp(ns, nsy) != 0){
	if (xerr &&
	    netconf_bad_element_xml(xerr, "application", name, "Namespace mismatch") < 0)
	    goto done;
	goto fail;
    }
 set:
    xml_spec_set(xt, y);
#ifdef XML_EXPLICIT_INDEX
    if (xml_search_index_p(xt))
	xml_search_child_insert(xp, xt);
#endif
    retval = 1;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Associate XML node x with yang spec y by going through all top-level modules and finding match
 *
 * @param[in]   xt     XML tree node
 * @param[in]   yspec  Yang spec
 * @param[out]  xerr   Reason for failure, or NULL
 * @retval      1      OK yang assignment made
 * @retval      0      yang assigment not made and xerr set
 * @retval     -1      Error
 * @see populate_self_parent
 */
static int
populate_self_top(cxobj     *xt, 
		  yang_stmt *yspec,
		  cxobj    **xerr)
{
    int        retval = -1;
    yang_stmt *y = NULL;     /* yang node */
    yang_stmt *ymod;         /* yang module */
    char      *name;
    char      *ns = NULL;    /* XML namespace of xt */
    char      *nsy = NULL;   /* Yang namespace of xt */
    cbuf      *cb = NULL;
    cxobj     *xp;

    name = xml_name(xt);
    if (yspec == NULL){
	if (xerr &&
	    netconf_bad_element_xml(xerr, "application", name, "Missing yang spec") < 0)
	    goto done;
	goto fail;
    }
    if (ys_module_by_xml(yspec, xt, &ymod) < 0)
	goto done;
    if (xml2ns(xt, xml_prefix(xt), &ns) < 0)
	goto done;
    /* ymod is "real" module, name may belong to included submodule */
    if (ymod == NULL){
	if (xerr){
	    if ((cb = cbuf_new()) == NULL){
		clicon_err(OE_UNIX, errno, "cbuf_new");
		goto done;
	    }
	    cprintf(cb, "Failed to find YANG spec of XML node: %s", name);
	    if ((xp = xml_parent(xt)) != NULL)
		cprintf(cb, " with parent: %s", xml_name(xp));
	    if (ns)
		cprintf(cb, " in namespace: %s", ns);
	    if (netconf_unknown_element_xml(xerr, "application", name, cbuf_get(cb)) < 0)
		goto done;
	}
	goto fail;
    }

    if ((y = yang_find_schemanode(ymod, name)) == NULL){ /* also rpc */
	if (_yang_unknown_anydata){
	    /* Add dummy Y_ANYDATA yang stmt, see ysp_add */
	    if ((y = yang_anydata_add(ymod, name)) < 0)
		goto done;
	    xml_spec_set(xt, y);
	    retval = 2; /* treat as anydata */
	    clicon_log(LOG_WARNING,
		       "%s: %d: No YANG spec for %s, anydata used",
		       __FUNCTION__, __LINE__, name);
	    goto done;
	}
	if ((cb = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	cprintf(cb, "Failed to find YANG spec of XML node: %s", name);
	if ((xp = xml_parent(xt)) != NULL)
	    cprintf(cb, " with parent: %s", xml_name(xp));
	if (ns)
	    cprintf(cb, " in namespace: %s", ns);
	if (xerr &&
	    netconf_unknown_element_xml(xerr, "application", name, cbuf_get(cb)) < 0)
	    goto done;
	goto fail;
    }
    nsy = yang_find_mynamespace(y);
    if (ns == NULL || nsy == NULL){
	if (xerr &&
	    netconf_bad_element_xml(xerr, "application", name, "Missing namespace") < 0)
	    goto done;
	goto fail;
    }
    /* Assign spec only if namespaces match */
    if (strcmp(ns, nsy) != 0){
	if (xerr &&
	    netconf_bad_element_xml(xerr, "application", name, "Namespace mismatch") < 0)
	    goto done;
	goto fail;
    }
    xml_spec_set(xt, y);
    retval = 1;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Find yang spec association of tree of XML nodes
 *
 * Populate xt:s children as top-level symbols
 * This may be unnecessary if yspec is set on manual creation: x=xml_new(); xml_spec_set(x,y)
 * @param[in]   xt     XML tree node
 * @param[in]   yb     How to bind yang to XML top-level when parsing
 * @param[in]   yspec  Yang spec
 * @param[out]  xerr   Reason for failure, or NULL
 * @retval      1      OK yang assignment made
 * @retval      0      Partial or no yang assigment made (at least one failed) and xerr set
 * @retval     -1      Error
 * @code
 *   cxobj *xerr = NULL;
 *   if (xml_bind_yang(x, YB_MODULE, yspec, &xerr) < 0)
 *     err;
 * @endcode
 * @note For subs to anyxml nodes will not have spec set
 * There are several functions in the API family
 * @see xml_bind_yang_rpc     for incoming rpc 
 * @see xml_bind_yang0        If the calling xml object should also be populated
 */
int
xml_bind_yang(cxobj     *xt, 
	      yang_bind  yb,
	      yang_stmt *yspec,
	      cxobj    **xerr)
{
    int    retval = -1;
    cxobj *xc;         /* xml child */
    int    ret;
    int    failed = 0; /* we continue loop after failure, should we stop at fail?`*/

    strip_whitespace(xt);
    xc = NULL;     /* Apply on children */
    while ((xc = xml_child_each(xt, xc, CX_ELMNT)) != NULL) {
	if ((ret = xml_bind_yang0(xc, yb, yspec, xerr)) < 0)
	    goto done;
	if (ret == 0)
	    failed++;
    }
    if (failed)
	goto fail;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

static int
xml_bind_yang0_opt(cxobj     *xt, 
		   yang_bind  yb,
		   cxobj     *xsibling,
		   cxobj    **xerr)
{
    int        retval = -1;
    cxobj     *xc;           /* xml child */
    int        ret;
    int        failed = 0; /* we continue loop after failure, should we stop at fail?`*/
    yang_stmt *yc0 = NULL;
    cxobj     *xc0 = NULL;
    cxobj     *xs;
    char      *name0 = NULL;
    char      *prefix0 = NULL;
    char      *name;
    char      *prefix;

    switch (yb){
    case YB_PARENT:
	if ((ret = populate_self_parent(xt, xsibling, xerr)) < 0)
	    goto done;
	break;
    default:
	clicon_err(OE_XML, EINVAL, "Invalid yang binding: %d", yb);
	goto done;
	break;
    }
    if (ret == 0)
	goto fail;
    else if (ret == 2)     /* ret=2 for anyxml from parent^ */
    	goto ok;
    strip_whitespace(xt);
    xc = NULL;     /* Apply on children */
    while ((xc = xml_child_each(xt, xc, CX_ELMNT)) != NULL) {
	/* It is xml2ns in populate_self_parent that needs improvement */
	/* cache previous + prefix */
	name = xml_name(xc);
	prefix = xml_prefix(xc);
	if (yc0 != NULL &&
	    clicon_strcmp(name0, name) == 0 &&
	    clicon_strcmp(prefix0, prefix) == 0){
	    if ((ret = xml_bind_yang0_opt(xc, YB_PARENT, xc0, xerr)) < 0)
		goto done;
	}
	else if (xsibling &&
		 (xs = xml_find_type(xsibling, prefix, name, CX_ELMNT)) != NULL){
	    if ((ret = xml_bind_yang0_opt(xc, YB_PARENT, xs, xerr)) < 0)
		goto done;
	}
	else if ((ret = xml_bind_yang0_opt(xc, YB_PARENT, NULL, xerr)) < 0)
	    goto done;
	if (ret == 0)
	    failed++;
	xc0 = xc;
	yc0 = xml_spec(xc); /* cache */
	name0 = xml_name(xc);
	prefix0 = xml_prefix(xc);
    }
    if (failed)
	goto fail;
 ok:
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Find yang spec association of tree of XML nodes
 *
 * @param[in]   xt     XML tree node
 * @param[in]   yb     How to bind yang to XML top-level when parsing
 * @param[in]   yspec  Yang spec
 * @param[out]  xerr   Reason for failure, or NULL
 * @retval      1      OK yang assignment made
 * @retval      0      Partial or no yang assigment made (at least one failed) and xerr set
 * @retval     -1      Error
 * Populate xt as top-level node
 * @see xml_bind_yang  If only children of xt should be populated, not xt itself
 */
int
xml_bind_yang0(cxobj     *xt, 
	       yang_bind  yb,
	       yang_stmt *yspec,
	       cxobj    **xerr)
{
    int        retval = -1;
    cxobj     *xc;           /* xml child */
    int        ret;
    int        failed = 0; /* we continue loop after failure, should we stop at fail?`*/

    switch (yb){
    case YB_MODULE:
	if ((ret = populate_self_top(xt, yspec, xerr)) < 0) 
	    goto done;
	break;
    case YB_PARENT:
	if ((ret = populate_self_parent(xt, NULL, xerr)) < 0)
	    goto done;
	break;
    case YB_NONE:
	ret = 1;
	break;
    default:
	clicon_err(OE_XML, EINVAL, "Invalid yang binding: %d", yb);
	goto done;
	break;
    }
    if (ret == 0)
	goto fail;
    else if (ret == 2)     /* ret=2 for anyxml from parent^ */
    	goto ok;
    strip_whitespace(xt);
    xc = NULL;     /* Apply on children */
    while ((xc = xml_child_each(xt, xc, CX_ELMNT)) != NULL) {
	if ((ret = xml_bind_yang0_opt(xc, YB_PARENT, NULL, xerr)) < 0)
	    goto done;
	if (ret == 0)
	    failed++;
    }
    if (failed)
	goto fail;
 ok:
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Find yang spec association of XML node for incoming RPC starting with <rpc>
 * 
 * Incoming RPC has an "input" structure that is not taken care of by xml_bind_yang
 * @param[in]   xrpc   XML rpc node
 * @param[in]   yspec  Yang spec
 * @param[out]  xerr   Reason for failure, or NULL
 * @retval      1      OK yang assignment made
 * @retval      0      Partial or no yang assigment made (at least one failed) and xerr set
 * @retval     -1      Error
 * The 
 * @code
 *   if (xml_bind_yang_rpc(h, x, NULL) < 0)
 *      err;
 * @endcode
 * @see xml_bind_yang  For other generic cases
 * @see xml_bind_yang_rpc_reply
 */
int
xml_bind_yang_rpc(cxobj     *xrpc,
		  yang_stmt *yspec,
		  cxobj    **xerr)
{
    int        retval = -1;
    yang_stmt *yrpc = NULL;    /* yang node */
    yang_stmt *ymod=NULL; /* yang module */
    yang_stmt *yi = NULL; /* input */
    cxobj     *x;
    int        ret;
    char      *opname;  /* top-level netconf operation */
    char      *rpcname; /* RPC name */
    char      *name;
    cbuf      *cb = NULL;
    cxobj     *xc;
    
    opname = xml_name(xrpc);
    if ((strcmp(opname, "hello")) == 0) /* Hello: dont bind, dont appear in any yang spec  */
	goto ok;
    else if ((strcmp(opname, "notification")) == 0)
	goto ok;
    else if ((strcmp(opname, "rpc")) == 0)
	; /* continue */
    else {   /* Notify, rpc-reply? */
	if (xerr &&
	    netconf_unknown_element_xml(xerr, "protocol", opname, "Unrecognized netconf operation") < 0)
	    goto done;
	goto fail;
    }
    x = NULL;
    while ((x = xml_child_each(xrpc, x, CX_ELMNT)) != NULL) {
	rpcname = xml_name(x);
	if (ys_module_by_xml(yspec, x, &ymod) < 0)
	    goto done;
	if (ymod == NULL){
	    if (xerr &&
		netconf_unknown_element_xml(xerr, "application", rpcname, "Unrecognized RPC (wrong namespace?)") < 0)
		goto done;
	    goto fail;
	}
	if ((yrpc = yang_find(ymod, Y_RPC, rpcname)) == NULL){
	    if (xerr &&
		netconf_unknown_element_xml(xerr, "application", rpcname, "Unrecognized RPC") < 0)
		goto done;
	    goto fail;
	}
	xml_spec_set(x, yrpc); /* required for validate */
	if ((yi = yang_find(yrpc, Y_INPUT, NULL)) == NULL){
	    /* If no yang input spec but RPC has elements, return unknown element */
	    if (xml_child_nr_type(x, CX_ELMNT) != 0){
		xc = xml_child_i_type(x, 0, CX_ELMNT); /* Pick first */
		name = xml_name(xc);
		if ((cb = cbuf_new()) == NULL){
		    clicon_err(OE_UNIX, errno, "cbuf_new");
		    goto done;
		}
		cprintf(cb, "Unrecognized parameter: %s in rpc: %s", name, rpcname);
		if (xerr &&
		    netconf_unknown_element_xml(xerr, "application", name, cbuf_get(cb)) < 0)
		    goto done;
		goto fail;
	    }
	}
	else{
	    /* xml_bind_yang need to have parent with yang spec for
	     * recursive population to work. Therefore, assign input yang
	     * to rpc level although not 100% intuitive */
	    xml_spec_set(x, yi); 
	    if ((ret = xml_bind_yang(x, YB_PARENT, NULL, xerr)) < 0)
		goto done;
	    if (ret == 0)
		goto fail;
	}
    }
 ok:
    retval = 1;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Find yang spec association of XML node for outgoing RPC starting with <rpc-reply>
 * 
 * Outgoing RPC has an "output" structure that is not taken care of by xml_bind_yang
 * @param[in]   xrpc   XML rpc node
 * @param[in]   name   Name of RPC (not seen in output/reply)
 * @param[in]   yspec  Yang spec
 * @param[out]  xerr   Reason for failure, or NULL
 * @retval      1      OK yang assignment made
 * @retval      0      Partial or no yang assigment made (at least one failed) and xerr set
 * @retval     -1      Error
 *
 * @code
 *   if (xml_bind_yang_rpc_reply(x, "get-config", yspec, name) < 0)
 *      err;
 * @endcode
 * @see xml_bind_yang  For other generic cases
 */
int
xml_bind_yang_rpc_reply(cxobj     *xrpc,
			char      *name,
			yang_stmt *yspec,
			cxobj    **xerr)
{
    int        retval = -1;
    yang_stmt *yrpc = NULL;    /* yang node */
    yang_stmt *ymod=NULL;      /* yang module */
    yang_stmt *yo = NULL;      /* output */
    cxobj     *x;
    int        ret;
    cxobj     *xerr1 = NULL;
    char      *opname;
    cbuf      *cberr = NULL;
    
    opname = xml_name(xrpc);
    if (strcmp(opname, "rpc-reply")){
	if ((cberr = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	cprintf(cberr, "Internal error, unrecognized netconf operation in backend reply, expected rpc-reply but received: %s", opname);
	if (xerr && netconf_operation_failed_xml(xerr, "application", cbuf_get(cberr)) < 0)
	    goto done;
	goto fail;
    }
    x = NULL;
    while ((x = xml_child_each(xrpc, x, CX_ELMNT)) != NULL) {
	if (ys_module_by_xml(yspec, x, &ymod) < 0)
	    goto done;
	if (ymod == NULL)
	    continue;
	if ((yrpc = yang_find(ymod, Y_RPC, name)) == NULL)
	    continue;
	//	xml_spec_set(xrpc, yrpc);
	if ((yo = yang_find(yrpc, Y_OUTPUT, NULL)) == NULL)
	    continue;
	/* xml_bind_yang need to have parent with yang spec for
	 * recursive population to work. Therefore, assign input yang
	 * to rpc level although not 100% intuitive */
	break;
    }
    if (yo != NULL){
	xml_spec_set(xrpc, yo); 
	/* Use a temporary xml error tree since it is stringified in the original error on error */
	if ((ret = xml_bind_yang(xrpc, YB_PARENT, NULL, &xerr1)) < 0)
	    goto done;
	if (ret == 0){
	    if ((cberr = cbuf_new()) == NULL){
		clicon_err(OE_UNIX, errno, "cbuf_new");
		goto done;
	    }
	    cprintf(cberr, "Internal error in backend reply: ");
	    if (netconf_err2cb(xerr1, cberr) < 0)
		goto done;
	    if (xerr && netconf_operation_failed_xml(xerr, "application", cbuf_get(cberr)) < 0)
		goto done;
	    goto fail;
	}
    }
    retval = 1;
 done:
    if (cberr)
	cbuf_free(cberr);
    if (xerr1)
	xml_free(xerr1);
    return retval;
 fail:
    retval = 0;
    goto done;
}


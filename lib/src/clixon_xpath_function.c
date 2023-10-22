/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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
  use your version of this file under the terms of Apache License version 2, indicate
  your decision by deleting the provisions above and replace them with the 
  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 * Clixon XML XPath 1.0 according to https://www.w3.org/TR/xpath-10
 * and rfc 7950
 *
 */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <syslog.h>
#include <fcntl.h>
#include <math.h> /* NaN */

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_options.h"
#include "clixon_yang.h"
#include "clixon_yang_type.h"
#include "clixon_xml.h"
#include "clixon_xml_map.h"
#include "clixon_yang_module.h"
#include "clixon_validate.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_xpath_eval.h"
#include "clixon_xpath_function.h"

/*! xpath function translation table
 *
 * @see enum clixon_xpath_function
 */
static const map_str2int xpath_fnname_map[] = { /* alphabetic order */
    {"bit-is-set",           XPATHFN_BIT_IS_SET},
    {"boolean",              XPATHFN_BOOLEAN},
    {"eiling",               XPATHFN_CEILING},
    {"comment",              XPATHFN_COMMENT},
    {"concat",               XPATHFN_CONCAT},
    {"contains",             XPATHFN_CONTAINS},
    {"count",                XPATHFN_COUNT},
    {"current",              XPATHFN_CURRENT},
    {"deref",                XPATHFN_DEREF},
    {"derived-from",         XPATHFN_DERIVED_FROM},
    {"derived-from-or-self", XPATHFN_DERIVED_FROM_OR_SELF},
    {"enum-value",           XPATHFN_ENUM_VALUE},
    {"false",                XPATHFN_FALSE},
    {"floor",                XPATHFN_FLOOR},
    {"id",                   XPATHFN_ID},
    {"lang",                 XPATHFN_LANG},
    {"last",                 XPATHFN_LAST},
    {"local-name",           XPATHFN_LOCAL_NAME},
    {"name",                 XPATHFN_NAME},
    {"namespace-uri",        XPATHFN_NAMESPACE_URI},
    {"normalize-space",      XPATHFN_NORMALIZE_SPACE},
    {"node",                 XPATHFN_NODE},
    {"not",                  XPATHFN_NOT},
    {"number",               XPATHFN_NUMBER},
    {"position",             XPATHFN_POSITION},
    {"processing-instructions", XPATHFN_PROCESSING_INSTRUCTIONS},
    {"re-match",             XPATHFN_RE_MATCH},
    {"round",                XPATHFN_ROUND},
    {"starts-with",          XPATHFN_STARTS_WITH},
    {"string",               XPATHFN_STRING},
    {"substring",            XPATHFN_SUBSTRING},
    {"substring-after",      XPATHFN_SUBSTRING_AFTER},
    {"substring-before",     XPATHFN_SUBSTRING_BEFORE},
    {"sum",                  XPATHFN_SUM},
    {"text",                 XPATHFN_TEXT},
    {"translate",            XPATHFN_TRANSLATE},
    {"true",                 XPATHFN_TRUE},
    {NULL,                  -1}
};

/*! Translate xpath function name to int code
 */
int
xp_fnname_str2int(char *fnname)
{
    return clicon_str2int(xpath_fnname_map, fnname);
}

/*! Translate xpath function code to string name
 */
const char *
xp_fnname_int2str(enum clixon_xpath_function code)
{
    return clicon_int2str(xpath_fnname_map, code);
}

int
xp_function_current(xp_ctx            *xc0,
                    struct xpath_tree *xs,
                    cvec              *nsc,
                    int                localonly,
                    xp_ctx           **xrp)
{
    int         retval = -1;
    cxobj     **vec = NULL;
    int         veclen = 0;
    xp_ctx     *xc = NULL;

    if ((xc = ctx_dup(xc0)) == NULL)
        goto done;
    if (cxvec_append(xc->xc_initial, &vec, &veclen) < 0)
        goto done;
    ctx_nodeset_replace(xc, vec, veclen);
    *xrp = xc;
    xc = NULL;
    retval = 0;
 done:
    if (xc)
        ctx_free(xc);
    return retval;
}

int
xp_function_deref(xp_ctx            *xc0,
                  struct xpath_tree *xs,
                  cvec              *nsc,
                  int                localonly,
                  xp_ctx           **xrp)
{
    int         retval = -1;
    xp_ctx     *xc = NULL;
    int         i;
    cxobj     **vec = NULL;
    int         veclen = 0;
    cxobj      *xv;
    cxobj      *xref;
    yang_stmt  *ys;
    yang_stmt  *yt;
    yang_stmt  *ypath;
    char       *path;

    /* Create new xc */
    if ((xc = ctx_dup(xc0)) == NULL)
        goto done;
    for (i=0; i<xc->xc_size; i++){
        xv = xc->xc_nodeset[i];
        if ((ys = xml_spec(xv)) == NULL)
            continue;
        /* Get base type yc */
        if (yang_type_get(ys, NULL, &yt, NULL, NULL, NULL, NULL, NULL) < 0)
            goto done;
        if (strcmp(yang_argument_get(yt), "leafref") == 0){
            if ((ypath = yang_find(yt, Y_PATH, NULL)) != NULL){
                path = yang_argument_get(ypath);
                if ((xref = xpath_first(xv, nsc, "%s", path)) != NULL)
                    if (cxvec_append(xref, &vec, &veclen) < 0)
                        goto done;
            }
            ctx_nodeset_replace(xc, vec, veclen);
        }
        else if (strcmp(yang_argument_get(yt), "identityref") == 0){
        }
    }
    *xrp = xc;
    xc = NULL;
    retval = 0;
 done:
    if (xc)
        ctx_free(xc);
    return retval;
}

/*! Helper function for derived-from(-and-self) - eval one node
 *
 * @param[in]  nsc  XML Namespace context
 * @param[in]  self If set, implements derived_from_or_self
 * @retval     1    OK and match
 * @retval     0    OK but not match
 * @retval    -1    Error
 */
static int
derived_from_one(char  *baseidentity,
                 cvec  *nsc,
                 cxobj *xleaf,
                 int    self)
{
    int        retval = -1;
    yang_stmt *yleaf;
    yang_stmt *ytype;
    yang_stmt *ybaseid;
    yang_stmt *ymod;
    cvec      *idrefvec; /* Derived identityref list: (module:id)**/
    char      *node = NULL;
    char      *prefix = NULL;
    char      *id = NULL;
    cbuf      *cb = NULL;
    char      *baseid = NULL;

    /* Split baseidentity to get its id (w/o prefix) */
    if (nodeid_split(baseidentity, NULL, &baseid) < 0)
        goto done;
    if ((yleaf = xml_spec(xleaf)) == NULL)
        goto nomatch;
    if (yang_keyword_get(yleaf) != Y_LEAF && yang_keyword_get(yleaf) != Y_LEAF_LIST)
        goto nomatch;
    /* Node is of type identityref */
    if (yang_type_get(yleaf, NULL, &ytype, NULL, NULL, NULL, NULL, NULL) < 0)
        goto done;
    if (ytype == NULL || strcmp(yang_argument_get(ytype), "identityref"))
        goto nomatch;
    /* Find if the derivation chain is: identity ->...-> ytype
     * Example: 
     * identity is ex:ethernet
     * xleaf <type>fast-ethernet</type>
     * yleaf type identityref{base interface-type;}
     */
    /* Just get the object corresponding to the base identity */
    if ((ybaseid = yang_find_identity_nsc(ys_spec(yleaf), baseidentity, nsc)) == NULL)
        goto nomatch;
    /* Get its list of derived identities  */
    idrefvec = yang_cvec_get(ybaseid);
    /* Get and split the leaf id reference */
    if ((node = xml_body(xleaf)) == NULL) /* It may not be empty */
        goto nomatch;
    if (nodeid_split(node, &prefix, &id) < 0)
        goto done;
    /* Get its module (prefixes are not used here) */
    if (prefix == NULL)
        ymod = ys_module(yleaf);
    else{ /* from prefix to name */
        ymod = yang_find_module_by_prefix_yspec(ys_spec(yleaf), prefix);
    }
    if (ymod == NULL)
        goto nomatch;
    /* self special case, ie that the xleaf has a ref to itself */
    if (self &&
        ymod == ys_module(ybaseid) &&
        strcmp(baseid, id) == 0){
        ; /* match */
    }
    else {
        /* Allocate cbuf */
        if ((cb = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        cprintf(cb, "%s:%s", yang_argument_get(ymod), id);
        if (cvec_find(idrefvec, cbuf_get(cb)) == NULL)
            goto nomatch;
    }
    retval = 1;
 done:
    if (baseid)
        free(baseid);
    if (cb)
        cbuf_free(cb);
    if (id)
        free(id);
    if (prefix)
        free(prefix);
    return retval;
 nomatch:
    retval = 0;
    goto done;
}

/*! Eval xpath function derived-from(-and-self)
 *
 * @param[in]  xc   Incoming context
 * @param[in]  xs   XPath node tree
 * @param[in]  nsc  XML Namespace context
 * @param[in]  localonly Skip prefix and namespace tests (non-standard)
 * @param[in]  self If set, implements derived_from_or_self
 * @param[out] xrp  Resulting context
 * @retval     0    OK
 * @retval    -1    Error
 * @see rfc7950 10.4.1
 *  Returns "true" if any node in the argument "nodes" is a node of type "identityref" and its 
 *  value is an identity that is derived from (see Section 7.18.2) the identity "identity"
 * boolean derived-from(node-set nodes, string identity)
 * @see validate_identityref for similar code other usage
 */
int
xp_function_derived_from(xp_ctx            *xc,
                         struct xpath_tree *xs,
                         cvec              *nsc,
                         int                localonly,
                         int                self,
                         xp_ctx           **xrp)
{
    int        retval = -1;
    xp_ctx    *xr0 = NULL;
    xp_ctx    *xr1 = NULL;
    xp_ctx    *xr = NULL;
    char      *identity = NULL;
    int        i;
    int        ret = 0;

    if (xs == NULL || xs->xs_c0 == NULL || xs->xs_c1 == NULL){
        clicon_err(OE_XML, EINVAL, "derived-from expects but did not get two arguments");
        goto done;
    }
    /* contains two arguments in xs: boolean derived-from(node-set, string) */
    /* This evolves to a set of (identityref) nodes */
    if (xp_eval(xc, xs->xs_c0, nsc, localonly, &xr0) < 0)
        goto done;
    if (xr0->xc_type != XT_NODESET)
        goto done;
    /* This evolves to a string identity */
    if (xp_eval(xc, xs->xs_c1, nsc, localonly, &xr1) < 0)
        goto done;
    if (ctx2string(xr1, &identity) < 0)
        goto done;
    /* Allocate a return struct of type boolean */
    if ((xr = malloc(sizeof(*xr))) == NULL){
        clicon_err(OE_UNIX, errno, "malloc");
        goto done;
    }
    memset(xr, 0, sizeof(*xr));
    xr->xc_type = XT_BOOL;
    /* ANY node is an identityref and its value an identity that is derived ... */
    for (i=0; i<xr0->xc_size; i++){
        if ((ret = derived_from_one(identity, nsc, xr0->xc_nodeset[i], self)) < 0)
            goto done;
        if (ret == 1)
            break;
    }
    xr->xc_bool = ret;
    *xrp = xr;
    xr = NULL;
    retval = 0;
 done:
    if (xr0)
        ctx_free(xr0);
    if (xr1)
        ctx_free(xr1);
    if (identity)
        free(identity);
    return retval;
}

/*! Returns true if the first node value has a bit set
 *
 * The bit-is-set() function returns "true" if the first node in
 * document order in the argument "nodes" is a node of type "bits" and
 * its value has the bit "bit-name" set; otherwise, it returns "false".
 *
 * Signature: boolean bit-is-set(node-set nodes, string bit-name)

 * @param[in]  xc   Incoming context
 * @param[in]  xs   XPath node tree
 * @param[in]  nsc  XML Namespace context
 * @param[in]  localonly Skip prefix and namespace tests (non-standard)
 * @param[out] xrp  Resulting context
 * @retval     0    OK
 * @retval    -1    Error
 * Example: interface[bit-is-set(flags, 'UP')
 * @see RFC 7950 Sec 10.6
 * typecheck is not made but assume this is done by validate
 * XXX: lacks proper parsing of bits, just  use strstr, see eg cv_validate1
 */
int
xp_function_bit_is_set(xp_ctx            *xc,
                       struct xpath_tree *xs,
                       cvec              *nsc,
                       int                localonly,
                       xp_ctx           **xrp)
{
    int     retval = -1;
    xp_ctx *xr = NULL;
    xp_ctx *xr0 = NULL;
    xp_ctx *xr1 = NULL;
    char   *s1 = NULL;
    cxobj  *x;
    char   *body;

    if (xs == NULL || xs->xs_c0 == NULL || xs->xs_c1 == NULL){
        clicon_err(OE_XML, EINVAL, "contains expects but did not get two arguments");
        goto done;
    }
    /* First node-set argument */
    if (xp_eval(xc, xs->xs_c0, nsc, localonly, &xr0) < 0)
        goto done;
    /* Second string argument */
    if (xp_eval(xc, xs->xs_c1, nsc, localonly, &xr1) < 0)
        goto done;
    if (ctx2string(xr1, &s1) < 0)
        goto done;
    if ((xr = malloc(sizeof(*xr))) == NULL){
        clicon_err(OE_UNIX, errno, "malloc");
        goto done;
    }
    memset(xr, 0, sizeof(*xr));
    xr->xc_type = XT_BOOL;
    /* The first node in document order in the argument "nodes" 
     * is a node of type "bits") and # NOT IMPLEMENTED
     * its value has the bit "bit-name" set
    */
    if (xr0->xc_size &&
        (x = xr0->xc_nodeset[0]) != NULL &&
        (body = xml_body(x)) != NULL){
        xr->xc_bool = strstr(body, s1) != NULL;
    }
    *xrp = xr;
    retval = 0;
 done:
    if (xr0)
        ctx_free(xr0);
    if (xr1)
        ctx_free(xr1);
    if (s1)
        free(s1);
    return retval;
}

/*! Return a number equal to the context position from the expression evaluation context.
 *
 * Signature: number position(node-set)
 * @param[in]  xc   Incoming context
 * @param[in]  xs   XPath node tree
 * @param[in]  nsc  XML Namespace context
 * @param[in]  localonly Skip prefix and namespace tests (non-standard)
 * @param[out] xrp  Resulting context
 * @retval     0    OK
 * @retval    -1    Error
 */
int
xp_function_position(xp_ctx            *xc,
                     struct xpath_tree *xs,
                     cvec              *nsc,
                     int                localonly,
                     xp_ctx           **xrp)
{
    int         retval = -1;
    xp_ctx     *xr = NULL;

    if ((xr = malloc(sizeof(*xr))) == NULL){
        clicon_err(OE_UNIX, errno, "malloc");
        goto done;
    }
    memset(xr, 0, sizeof(*xr));
    xr->xc_initial = xc->xc_initial;
    xr->xc_type = XT_NUMBER;
    xr->xc_number = xc->xc_position;
    *xrp = xr;
    retval = 0;
 done:
    return retval;
}

/*! The count function returns the number of nodes in the argument node-set.
 *
 * Signature: number count(node-set)
 */
int
xp_function_count(xp_ctx            *xc,
                  struct xpath_tree *xs,
                  cvec              *nsc,
                  int                localonly,
                  xp_ctx           **xrp)
{
    int         retval = -1;
    xp_ctx     *xr = NULL;
    xp_ctx     *xr0 = NULL;

    if (xs == NULL || xs->xs_c0 == NULL){
        clicon_err(OE_XML, EINVAL, "count expects but did not get one argument");
        goto done;
    }
    if (xp_eval(xc, xs->xs_c0, nsc, localonly, &xr0) < 0)
        goto done;
    if ((xr = malloc(sizeof(*xr))) == NULL){
        clicon_err(OE_UNIX, errno, "malloc");
        goto done;
    }
    memset(xr, 0, sizeof(*xr));
    xr->xc_type = XT_NUMBER;
    xr->xc_number = xr0->xc_size;
    *xrp = xr;
    retval = 0;
 done:
    if (xr0)
        ctx_free(xr0);
    return retval;
}

/*! The name function returns a string of a QName
 *
 * The name function returns a string containing a QName representing the expanded-name
 * of the node in the argument node-set that is first in document order. 
 * Signature: string name(node-set?)
 * XXX: should return expanded-name, should namespace be included?
 */
int
xp_function_name(xp_ctx            *xc,
                 struct xpath_tree *xs,
                 cvec              *nsc,
                 int                localonly,
                 xp_ctx           **xrp)
{
    int         retval = -1;
    xp_ctx     *xr = NULL;
    xp_ctx     *xr0 = NULL;
    char       *s0 = NULL;
    int         i;
    cxobj      *x;

    if (xs == NULL || xs->xs_c0 == NULL){
        clicon_err(OE_XML, EINVAL, "not expects but did not get one argument");
        goto done;
    }
    if (xp_eval(xc, xs->xs_c0, nsc, localonly, &xr0) < 0)
        goto done;
    if ((xr = malloc(sizeof(*xr))) == NULL){
        clicon_err(OE_UNIX, errno, "malloc");
        goto done;
    }
    memset(xr, 0, sizeof(*xr));
    xr->xc_type = XT_STRING;
    for (i=0; i<xr0->xc_size; i++){
        if ((x = xr0->xc_nodeset[i]) == NULL)
            continue;
        if ((xr->xc_string = strdup(xml_name(x))) == NULL){
            clicon_err(OE_UNIX, errno, "strdup");
            goto done;
        }
        break;
    }
    *xrp = xr;
    retval = 0;
 done:
    if (xr0)
        ctx_free(xr0);
    if (s0)
        free(s0);
    return retval;
}

/*! Eval xpath function contains
 *
 * @param[in]  xc   Incoming context
 * @param[in]  xs   XPath node tree
 * @param[in]  nsc  XML Namespace context
 * @param[in]  localonly Skip prefix and namespace tests (non-standard)
 * @param[out] xrp  Resulting context
 * @retval     0    OK
 * @retval    -1    Error
 * @see https://www.w3.org/TR/xpath-10/#NT-FunctionName 4.2 String Functions
 */
int
xp_function_contains(xp_ctx            *xc,
                     struct xpath_tree *xs,
                     cvec              *nsc,
                     int                localonly,
                     xp_ctx           **xrp)
{
    int                retval = -1;
    xp_ctx            *xr0 = NULL;
    xp_ctx            *xr1 = NULL;
    xp_ctx            *xr = NULL;
    char              *s0 = NULL;
    char              *s1 = NULL;

    if (xs == NULL || xs->xs_c0 == NULL || xs->xs_c1 == NULL){
        clicon_err(OE_XML, EINVAL, "contains expects but did not get two arguments");
        goto done;
    }
    /* contains two arguments in xs: boolean contains(string, string) */
    if (xp_eval(xc, xs->xs_c0, nsc, localonly, &xr0) < 0)
        goto done;
    if (ctx2string(xr0, &s0) < 0)
        goto done;
    if (xp_eval(xc, xs->xs_c1, nsc, localonly, &xr1) < 0)
        goto done;
    if (ctx2string(xr1, &s1) < 0)
        goto done;
    if ((xr = malloc(sizeof(*xr))) == NULL){
        clicon_err(OE_UNIX, errno, "malloc");
        goto done;
    }
    memset(xr, 0, sizeof(*xr));
    xr->xc_type = XT_BOOL;
    xr->xc_bool = (strstr(s0, s1) != NULL);
    *xrp = xr;
    xr = NULL;
    retval = 0;
 done:
    if (xr0)
        ctx_free(xr0);
    if (xr1)
        ctx_free(xr1);
    if (s0)
        free(s0);
    if (s1)
        free(s1);
    return retval;
}

/*! The boolean function converts its argument to a boolean
 *
 * Conversion is as follows:
 * - a number is true if and only if it is neither positive or negative zero nor NaN
 * - a node-set is true if and only if it is non-empty
 * - a string is true if and only if its length is non-zero
 * - an object of a type other than the four basic types is converted to a boolean in a way that 
 *   is dependent on that type
 * Signature: boolean boolean(object)
 */
int
xp_function_boolean(xp_ctx            *xc,
                    struct xpath_tree *xs,
                    cvec              *nsc,
                    int                localonly,
                    xp_ctx           **xrp)
{
    int         retval = -1;
    xp_ctx     *xr = NULL;
    xp_ctx     *xr0 = NULL;
    int         bool;

    if (xs == NULL || xs->xs_c0 == NULL){
        clicon_err(OE_XML, EINVAL, "not expects but did not get one argument");
        goto done;
    }
    if (xp_eval(xc, xs->xs_c0, nsc, localonly, &xr0) < 0)
        goto done;
    bool = ctx2boolean(xr0);
    if ((xr = malloc(sizeof(*xr))) == NULL){
        clicon_err(OE_UNIX, errno, "malloc");
        goto done;
    }
    memset(xr, 0, sizeof(*xr));
    xr->xc_type = XT_BOOL;
    xr->xc_bool = bool;
    *xrp = xr;
    retval = 0;
 done:
    if (xr0)
        ctx_free(xr0);
    return retval;
}

/*! The not function returns true if its argument is false, and false otherwise.
 *
 * Signature: boolean not(boolean)
 */
int
xp_function_not(xp_ctx            *xc,
                struct xpath_tree *xs,
                cvec              *nsc,
                int                localonly,
                xp_ctx           **xrp)
{
    if (xp_function_boolean(xc, xs, nsc, localonly, xrp) < 0)
        return -1;
    (*xrp)->xc_bool = !(*xrp)->xc_bool;
    return 0;
}

/*! The true function returns true.
 *
 * Signature: boolean true()
 */
int
xp_function_true(xp_ctx            *xc,
                 struct xpath_tree *xs,
                 cvec              *nsc,
                 int                localonly,
                 xp_ctx           **xrp)
{
    int         retval = -1;
    xp_ctx     *xr = NULL;

    if ((xr = malloc(sizeof(*xr))) == NULL){
        clicon_err(OE_UNIX, errno, "malloc");
        goto done;
    }
    memset(xr, 0, sizeof(*xr));
    xr->xc_type = XT_BOOL;
    xr->xc_bool = 1;
    *xrp = xr;
    retval = 0;
 done:
    return retval;
}

/*! The false function returns false.
 *
 * Signature: boolean false()
 */
int
xp_function_false(xp_ctx            *xc,
                  struct xpath_tree *xs,
                  cvec              *nsc,
                  int                localonly,
                  xp_ctx           **xrp)
{
    int         retval = -1;
    xp_ctx     *xr = NULL;

    if ((xr = malloc(sizeof(*xr))) == NULL){
        clicon_err(OE_UNIX, errno, "malloc");
        goto done;
    }
    memset(xr, 0, sizeof(*xr));
    xr->xc_type = XT_BOOL;
    xr->xc_bool = 0;
    *xrp = xr;
    retval = 0;
 done:
    return retval;
}

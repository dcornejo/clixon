/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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

 * TEXT / curly-brace syntax parsing and translations
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <syslog.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_queue.h"
#include "clixon_string.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_options.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_xml_io.h"
#include "clixon_xml_sort.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xml_bind.h"
#include "clixon_text_syntax.h"
#include "clixon_text_syntax_parse.h"

/* Size of json read buffer when reading from file*/
#define BUFLEN 1024

/* Name of xml top object created by parse functions
 * See also DATASTORE_TOP_SYMBOL which is the clixon datastore top symbol. By default also config
 */

#define TEXT_TOP_SYMBOL "top"

/*! x is element and has eactly one child which in turn has none 
 *
 * @see child_type in clixon_json.c
 */
static int
tleaf(cxobj *x)
{
    cxobj *xc;

    if (xml_type(x) != CX_ELMNT)
        return 0;
    if (xml_child_nr_notype(x, CX_ATTR) != 1)
        return 0;
    /* From here exactly one noattr child, get it */
    xc = NULL;
    while ((xc = xml_child_each(x, xc, -1)) != NULL)
        if (xml_type(xc) != CX_ATTR)
            break;
    if (xc == NULL)
        return -1; /* n/a */
    return (xml_child_nr_notype(xc, CX_ATTR) == 0);
}

/*! Translate XML to a "pseudo-code" textual format using a callback - internal function
 *
 * @param[in]     xn       XML object to print
 * @param[in]     fn       Callback to make print function
 * @param[in]     f        File to print to
 * @param[in]     level    Print PRETTYPRINT_INDENT spaces per level in front of each line
 * @param[in]     autocliext How to handle autocli extensions: 0: ignore 1: follow
 * @param[in,out] leafl    Leaflist state for keeping track of when [] ends
 * @param[in,out] leaflname Leaflist state for [] 
 * @retval        0        OK
 * @retval       -1        Error
 * leaflist state:
 * 0: No leaflist
 * 1: In leaflist
 * @see text2cbuf to buffer (slower)
 */
static int
text2file(cxobj            *xn,
          clicon_output_cb *fn,
          FILE             *f,
          int               level,
          int               autocliext,
          int              *leafl,
          char            **leaflname)

{
    int        retval = -1;
    cxobj     *xc = NULL;
    int        children=0;
    int        exist = 0;
    yang_stmt *yn;
    char      *value;
    cg_var    *cvi;
    cvec      *cvk = NULL; /* vector of index keys */
    cbuf      *cbb = NULL;
#ifndef TEXT_SYNTAX_NOPREFIX
    yang_stmt *yp = NULL;
    yang_stmt *ymod;
    yang_stmt *ypmod;
    char      *prefix = NULL;
#endif
    if (xn == NULL || fn == NULL){
        clicon_err(OE_XML, EINVAL, "xn or fn is NULL");
        goto done;
    }
    if ((yn = xml_spec(xn)) != NULL){
        if (autocliext){
            if (yang_extension_value(yn, "hide-show", CLIXON_AUTOCLI_NS, &exist, NULL) < 0)
                goto done;
            if (exist)
                goto ok;
        }
#ifndef TEXT_SYNTAX_NOPREFIX
        /* Find out prefix if needed: topmost or new module a la API-PATH */
        if (ys_real_module(yn, &ymod) < 0)
            goto done;
        if ((yp = yang_parent_get(yn)) != NULL &&
            yp != ymod){
            if (ys_real_module(yp, &ypmod) < 0)
                goto done;
            if (ypmod != ymod)
                prefix = yang_argument_get(ymod);
        }
        else
            prefix = yang_argument_get(ymod);
#endif
        if (yang_keyword_get(yn) == Y_LIST){
            if ((cvk = yang_cvec_get(yn)) == NULL){
                clicon_err(OE_YANG, 0, "No keys");
                goto done;
            }
        }
    }
    if (*leafl && yn){
        if (yang_keyword_get(yn) == Y_LEAF_LIST && strcmp(*leaflname, yang_argument_get(yn)) == 0)
            ;
        else{
            *leafl = 0;
            *leaflname = NULL;
            (*fn)(f, "%*s\n", PRETTYPRINT_INDENT*(level), "]");
        }
    }
    xc = NULL;     /* count children (elements and bodies, not attributes) */
    while ((xc = xml_child_each(xn, xc, -1)) != NULL)
        if (xml_type(xc) == CX_ELMNT || xml_type(xc) == CX_BODY)
            children++;
    if (children == 0){ /* If no children print line */
        switch (xml_type(xn)){
        case CX_BODY:{
            if ((cbb = cbuf_new()) == NULL){
                clicon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            value = xml_value(xn);
            if (index(value, ' ') != NULL)
                cprintf(cbb, "\"%s\"", value);
            else
                cprintf(cbb, "%s", value);
            if (*leafl)                            /* Skip keyword if leaflist */
                (*fn)(f, "%*s%s\n", PRETTYPRINT_INDENT*level, "", cbuf_get(cbb));
            else
                (*fn)(f, "%s;\n", cbuf_get(cbb));
            break;
        }
        case CX_ELMNT:
            (*fn)(f, "%*s%s", PRETTYPRINT_INDENT*level, "", xml_name(xn));
            cvi = NULL;             /* Lists only */
            while ((cvi = cvec_each(cvk, cvi)) != NULL) {
                if ((xc = xml_find_type(xn, NULL, cv_string_get(cvi), CX_ELMNT)) != NULL)
                    (*fn)(f, " %s", xml_body(xc));
            }
            (*fn)(f, ";\n");
            break;
        default:
            break;
        }
        goto ok;
    }
    if (*leafl == 0){
        (*fn)(f, "%*s", PRETTYPRINT_INDENT*level, "");
#ifndef TEXT_SYNTAX_NOPREFIX
        if (prefix)
            (*fn)(f, "%s:", prefix);
#endif
        (*fn)(f, "%s", xml_name(xn));
    }
    cvi = NULL;         /* Lists only */
    while ((cvi = cvec_each(cvk, cvi)) != NULL) {
        if ((xc = xml_find_type(xn, NULL, cv_string_get(cvi), CX_ELMNT)) != NULL)
            (*fn)(f, " %s", xml_body(xc));
    }
    if (yn && yang_keyword_get(yn) == Y_LEAF_LIST && *leafl){
        ;
    }
    else if (yn && yang_keyword_get(yn) == Y_LEAF_LIST && *leafl == 0){
        *leafl = 1;
        *leaflname = yang_argument_get(yn);
        (*fn)(f, " [\n");
    }
    else if (!tleaf(xn))
        (*fn)(f, " {\n");
    else
        (*fn)(f, " ");
    xc = NULL;
    while ((xc = xml_child_each(xn, xc, -1)) != NULL){
        if (xml_type(xc) == CX_ELMNT || xml_type(xc) == CX_BODY){
            if (yn && yang_key_match(yn, xml_name(xc), NULL))
                continue; /* Skip keys, already printed */
            if (text2file(xc, fn, f, level+1, autocliext, leafl, leaflname) < 0)
                break;
        }
    }
    /* Stop leaf-list printing (ie []) if no longer leaflist and same name */
    if (yn && yang_keyword_get(yn) != Y_LEAF_LIST && *leafl != 0){
        *leafl = 0;
        (*fn)(f, "%*s\n", PRETTYPRINT_INDENT*(level+1), "]");
    }
    if (!tleaf(xn))
        (*fn)(f, "%*s}\n", PRETTYPRINT_INDENT*level, "");
 ok:
    retval = 0;
 done:
    if (cbb)
        cbuf_free(cbb);
    return retval;
}

#ifndef TEXT_SYNTAX_NOPREFIX
static char *
get_prefix(yang_stmt *yn)
{
    char      *prefix = NULL;
    yang_stmt *yp = NULL;
    yang_stmt *ymod;
    yang_stmt *ypmod;

    /* Find out prefix if needed: topmost or new module a la API-PATH */
    if (ys_real_module(yn, &ymod) < 0)
        return NULL;
    if ((yp = yang_parent_get(yn)) != NULL &&
        yp != ymod){
        if (ys_real_module(yp, &ypmod) < 0)
            return NULL;
        if (ypmod != ymod)
            prefix = yang_argument_get(ymod);
    }
    else
        prefix = yang_argument_get(ymod);
    return prefix;
}
#endif

/*! Translate XML to a "pseudo-code" textual format using a callback - internal function
 *
 * @param[in]     xn       XML object to print
 * @param[in]     fn       Callback to make print function
 * @param[in]     f        File to print to
 * @param[in]     level    Print PRETTYPRINT_INDENT spaces per level in front of each line
 * @param[in]     prefix   Add string to beginning of each line (or NULL)
 * @param[in]     autocliext How to handle autocli extensions: 0: ignore 1: follow
 * @param[in,out] leafl    Leaflist state for keeping track of when [] ends
 * @param[in,out] leaflname Leaflist state for [] 
 * @retval        0        OK
 * @retval       -1        Error
 * leaflist state:
 * 0: No leaflist
 * 1: In leaflist
 * @see text2file but to file (faster)
 */
static int
text2cbuf(cbuf  *cb,
          cxobj *xn,
          int    level,
          char   *prepend,
          int    autocliext,
          int   *leafl,
          char **leaflname)
{
    int        retval = -1;
    cxobj     *xc = NULL;
    int        children=0;
    int        exist = 0;
    yang_stmt *yn;
    char      *value;
    cg_var    *cvi;
    cvec      *cvk = NULL; /* vector of index keys */
    cbuf      *cbb = NULL;
    int        level1;
    char      *prefix = NULL;

    if (xn == NULL || cb == NULL){
        clicon_err(OE_XML, EINVAL, "xn or cb is NULL");
        goto done;
    }
    level1 = level*PRETTYPRINT_INDENT;
    if (prepend)
        level1 -= strlen(prepend);
    if ((yn = xml_spec(xn)) != NULL){
        if (autocliext){
            if (yang_extension_value(yn, "hide-show", CLIXON_AUTOCLI_NS, &exist, NULL) < 0)
                goto done;
            if (exist)
                goto ok;
        }
#ifndef TEXT_SYNTAX_NOPREFIX
        prefix = get_prefix(yn);
#endif
        if (yang_keyword_get(yn) == Y_LIST){
            if ((cvk = yang_cvec_get(yn)) == NULL){
                clicon_err(OE_YANG, 0, "No keys");
                goto done;
            }
        }
    }
    if (*leafl && yn){
        if (yang_keyword_get(yn) == Y_LEAF_LIST && strcmp(*leaflname, yang_argument_get(yn)) == 0)
            ;
        else{
            *leafl = 0;
            *leaflname = NULL;
            if (prepend)
                cprintf(cb, "%s", prepend);
            cprintf(cb, "%*s\n", level1, "]");
        }
    }
    xc = NULL;     /* count children (elements and bodies, not attributes) */
    while ((xc = xml_child_each(xn, xc, -1)) != NULL)
        if (xml_type(xc) == CX_ELMNT || xml_type(xc) == CX_BODY)
            children++;
    if (children == 0){ /* If no children print line */
        switch (xml_type(xn)){
        case CX_BODY:{
            if ((cbb = cbuf_new()) == NULL){
                clicon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            value = xml_value(xn);
            if (index(value, ' ') != NULL)
                cprintf(cbb, "\"%s\"", value);
            else
                cprintf(cbb, "%s", value);
            if (*leafl){                            /* Skip keyword if leaflist */
                if (prepend)
                    cprintf(cb, "%s", prepend);
                cprintf(cb, "%*s%s\n", level1, "", cbuf_get(cbb));
            }
            else
                cprintf(cb, "%s;\n", cbuf_get(cbb));
            break;
        }
        case CX_ELMNT:
            if (prepend)
                cprintf(cb, "%s", prepend);
            cprintf(cb, "%*s%s", level1, "", xml_name(xn));
            cvi = NULL;             /* Lists only */
            while ((cvi = cvec_each(cvk, cvi)) != NULL) {
                if ((xc = xml_find_type(xn, NULL, cv_string_get(cvi), CX_ELMNT)) != NULL)
                    cprintf(cb, " %s", xml_body(xc));
            }
            cprintf(cb, ";\n");
            break;
        default:
            break;
        }
        goto ok;
    }
    if (*leafl == 0){
        if (prepend)
            cprintf(cb, "%s", prepend);
        cprintf(cb, "%*s", level1, "");
        if (prefix)
            cprintf(cb, "%s:", prefix);
        cprintf(cb, "%s", xml_name(xn));
    }
    cvi = NULL;         /* Lists only */
    while ((cvi = cvec_each(cvk, cvi)) != NULL) {
        if ((xc = xml_find_type(xn, NULL, cv_string_get(cvi), CX_ELMNT)) != NULL)
            cprintf(cb, " %s", xml_body(xc));
    }
    if (yn && yang_keyword_get(yn) == Y_LEAF_LIST && *leafl){
        ;
    }
    else if (yn && yang_keyword_get(yn) == Y_LEAF_LIST && *leafl == 0){
        *leafl = 1;
        *leaflname = yang_argument_get(yn);
        cprintf(cb, " [\n");
    }
    else if (!tleaf(xn))
        cprintf(cb, " {\n");
    else
        cprintf(cb, " ");
    xc = NULL;
    while ((xc = xml_child_each(xn, xc, -1)) != NULL){
        if (xml_type(xc) == CX_ELMNT || xml_type(xc) == CX_BODY){
            if (yn && yang_key_match(yn, xml_name(xc), NULL))
                continue; /* Skip keys, already printed */
            if (text2cbuf(cb, xc, level+1, prepend, autocliext, leafl, leaflname) < 0)
                break;
        }
    }
    /* Stop leaf-list printing (ie []) if no longer leaflist and same name */
    if (yn && yang_keyword_get(yn) != Y_LEAF_LIST && *leafl != 0){
        *leafl = 0;
        if (prepend)
            cprintf(cb, "%s", prepend);
        cprintf(cb, "%*s\n", level1 + PRETTYPRINT_INDENT, "]");
    }
    if (!tleaf(xn)){
        if (prepend)
            cprintf(cb, "%s", prepend);
        cprintf(cb, "%*s}\n", level1, "");
    }
 ok:
    retval = 0;
 done:
    if (cbb)
        cbuf_free(cbb);
    return retval;
}

/*! Translate XML to a "pseudo-code" textual format using a callback
 *
 * @param[in]  f        File to print to
 * @param[in]  xn       XML object to print
 * @param[in]  level    Print PRETTYPRINT_INDENT spaces per level in front of each line
 * @param[in]  fn       File print function (if NULL, use fprintf)
 * @param[in]  skiptop  0: Include top object 1: Skip top-object, only children, 
 * @param[in]  autocliext How to handle autocli extensions: 0: ignore 1: follow
 * @retval     0        OK
 * @retval    -1        Error
 */
int
clixon_text2file(FILE             *f,
                 cxobj            *xn,
                 int               level,
                 clicon_output_cb *fn,
                 int               skiptop,
                 int               autocliext)
{
    int    retval = 1;
    cxobj *xc;
    int    leafl = 0;
    char  *leaflname = NULL;

    if (fn == NULL)
        fn = fprintf;
    if (skiptop){
        xc = NULL;
        while ((xc = xml_child_each(xn, xc, CX_ELMNT)) != NULL)
            if (text2file(xc, fn, f, level, autocliext, &leafl, &leaflname) < 0)
                goto done;
    }
    else {
        if (text2file(xn, fn, f, level, autocliext, &leafl, &leaflname) < 0)
            goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Translate internal cxobj tree to a "curly" textual format to cbufs
 *
 * @param[out] cb      Cligen buffer to write to
 * @param[in]  xn       XML object to print
 * @param[in]  level    Print PRETTYPRINT_INDENT spaces per level in front of each line
 * @param[in]  skiptop  0: Include top object 1: Skip top-object, only children, 
 * @param[in]  autocliext How to handle autocli extensions: 0: ignore 1: follow
 * @retval     0        OK
 * @retval    -1        Error
 */
int
clixon_text2cbuf(cbuf             *cb,
                 cxobj            *xn,
                 int               level,
                 int               skiptop,
                 int               autocliext)
{
    int    retval = 1;
    cxobj *xc;
    int    leafl = 0;
    char  *leaflname = NULL;

    if (skiptop){
        xc = NULL;
        while ((xc = xml_child_each(xn, xc, CX_ELMNT)) != NULL)
            if (text2cbuf(cb, xc, level, NULL, autocliext, &leafl, &leaflname) < 0)
                goto done;
    }
    else {
        if (text2cbuf(cb, xn, level, NULL, autocliext, &leafl, &leaflname) < 0)
            goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Print list keys
 */
static int
text_diff_keys(cbuf      *cb,
               cxobj     *x,
               yang_stmt *y)
{
    cvec   *cvk;
    cg_var *cvi;
    char   *keyname;
    char   *keyval;

    if (y && yang_keyword_get(y) == Y_LIST){
        cvk = yang_cvec_get(y);
        cvi = NULL;
        while ((cvi = cvec_each(cvk, cvi)) != NULL) {
            keyname = cv_string_get(cvi);
            keyval = xml_find_body(x, keyname);
            cprintf(cb, " %s", keyval);
        }
    }
    return 0;
}

/*! Print TEXT diff of two cxobj trees into a cbuf
 *
 * YANG dependent
 * @param[out] cb      CLIgen buffer
 * @param[in]  x0      First XML tree
 * @param[in]  x1      Second XML tree
 * @param[in]  level   How many spaces to insert before each line
 * @param[in]  skiptop  0: Include top object 1: Skip top-object, only children, 
 * @retval     0       OK
 * @retval    -1       Error
 * @cod
 *    cbuf *cb = cbuf_new();
 *    if (clixon_text_diff2cbuf(cb, 0, x0, x1) < 0)
 *       err();
 * @endcode
 * @see clixon_xml_diff2cbuf
 * XXX Leaf-list +/- is not correct
 * For example, it should be:
 *    value [
 * +     97
 * -     99
 *    ]
 * But is:
 * +  value [
 * +     97
 * -     99
 */
static int
text_diff2cbuf(cbuf  *cb,
               cxobj *x0,
               cxobj *x1,
               int    level,
               int    skiptop)
{
    int        retval = -1;
    cxobj     *x0c = NULL; /* x0 child */
    cxobj     *x1c = NULL; /* x1 child */
    yang_stmt *yc0;
    yang_stmt *yc1;
    char      *b0;
    char      *b1;
    int        eq;
    int        nr=0;
    int        level1;
    yang_stmt *y0;
    char      *prefix = NULL;
    int        leafl = 0; // XXX
    char      *leaflname = NULL; // XXX

    level1 = level*PRETTYPRINT_INDENT;
    if ((y0 = xml_spec(x0)) != NULL){
#ifndef TEXT_SYNTAX_NOPREFIX
        prefix = get_prefix(y0);
#endif
    }
    /* Traverse x0 and x1 in lock-step */
    x0c = x1c = NULL;
    x0c = xml_child_each(x0, x0c, CX_ELMNT);
    x1c = xml_child_each(x1, x1c, CX_ELMNT);
    for (;;){
        /* Check if one or both subtrees are NULL */
        if (x0c == NULL && x1c == NULL)
            goto ok;
        else if (x0c == NULL){
            if (nr==0 && skiptop==0){
                cprintf(cb, "%*s", level1, "");
                if (prefix)
                    cprintf(cb, "%s:", prefix);
                cprintf(cb, "%s", xml_name(x1));
                text_diff_keys(cb, x1, y0);
                cprintf(cb, " {\n");
                nr++;
            }
            if (text2cbuf(cb, x1c, level+1, "+", 0, &leafl, &leaflname) < 0)
                goto done;
            x1c = xml_child_each(x1, x1c, CX_ELMNT);
            continue;
        }
        else if (x1c == NULL){
            if (nr==0 && skiptop==0){
                cprintf(cb, "%*s", level1, "");
                if (prefix)
                    cprintf(cb, "%s:", prefix);
                cprintf(cb, "%s", xml_name(x0));
                text_diff_keys(cb, x0, y0);
                cprintf(cb, "{\n");
                nr++;
            }
            if (text2cbuf(cb, x0c, level+1, "-", 0, &leafl, &leaflname) < 0)
                goto done;
            x0c = xml_child_each(x0, x0c, CX_ELMNT);
            continue;
        }
        /* Both x0c and x1c exists, check if they are yang-equal. */
        eq = xml_cmp(x0c, x1c, 0, 0, NULL);
        yc0 = xml_spec(x0c);
        yc1 = xml_spec(x1c);

        if (eq < 0){
            if (nr==0 && skiptop==0){
                cprintf(cb, "%*s", level1, "");
                if (prefix)
                    cprintf(cb, "%s:", prefix);
                cprintf(cb, "%s", xml_name(x0));
                text_diff_keys(cb, x0, y0);
                cprintf(cb, " {\n");
                nr++;
            }
            if (text2cbuf(cb, x0c, level+1, "-", 0, &leafl, &leaflname) < 0)
                goto done;
            x0c = xml_child_each(x0, x0c, CX_ELMNT);
            continue;
        }
        else if (eq > 0){
            if (nr==0 && skiptop==0){
                cprintf(cb, "%*s", level1, "");
                if (prefix)
                    cprintf(cb, "%s:", prefix);
                cprintf(cb, "%s", xml_name(x1));
                text_diff_keys(cb, x1, y0);
                cprintf(cb, " {\n");
                nr++;
            }
            if (text2cbuf(cb, x1c, level+1, "+", 0, &leafl, &leaflname) < 0)
                goto done;
            x1c = xml_child_each(x1, x1c, CX_ELMNT);
            continue;
        }
        else{ /* equal */
            if (yc0 && yc1 && yc0 != yc1){ /* choice */
                if (nr==0 && skiptop==0){
                    cprintf(cb, "%*s", level1, "");
                    if (prefix)
                        cprintf(cb, "%s:", prefix);
                    cprintf(cb, "%s {\n", xml_name(x0));
                    nr++;
                }
                if (text2cbuf(cb, x0c, level+1, "-", 0, &leafl, &leaflname) < 0)
                    goto done;
                if (text2cbuf(cb, x1c, level+1, "+", 0, &leafl, &leaflname) < 0)
                    goto done;
            }
            else if (yc0 && yang_keyword_get(yc0) == Y_LEAF){
                b0 = xml_body(x0c);
                b1 = xml_body(x1c);
                if (b0 == NULL && b1 == NULL)
                    ;
                else if (b0 == NULL || b1 == NULL
                         || strcmp(b0, b1) != 0){
                    if (nr==0 && skiptop == 0){
                        cprintf(cb, "%*s", level1, "");
                        if (prefix)
                            cprintf(cb, "%s:", prefix);
                        cprintf(cb, "%s", xml_name(x0));
                        text_diff_keys(cb, x0, y0);
                        cprintf(cb, " {\n");
                        nr++;
                    }
                    cprintf(cb, "-%*s%s %s;\n", level1+PRETTYPRINT_INDENT-1, "", xml_name(x0c), b0);
                    cprintf(cb, "+%*s%s %s;\n", level1+PRETTYPRINT_INDENT-1, "", xml_name(x1c), b1);
                }
            }
            else if (text_diff2cbuf(cb, x0c, x1c, level+1, 0) < 0)
                goto done;
        }
        /* Get next */
        x0c = xml_child_each(x0, x0c, CX_ELMNT);
        x1c = xml_child_each(x1, x1c, CX_ELMNT);
    } /* for */
 ok:
    if (nr)
        cprintf(cb, "%*s}\n", level1, "");
    retval = 0;
 done:
    return retval;
}

/*! Print TEXT diff of two cxobj trees into a cbuf
 *
 * YANG dependent
 * @param[out] cb      CLIgen buffer
 * @param[in]  x0      First XML tree
 * @param[in]  x1      Second XML tree
 * @retval     0       Ok
 * @retval    -1       Error
 * @cod
 *    cbuf *cb = cbuf_new();
 *    if (clixon_text_diff2cbuf(cb, 0, x0, x1) < 0)
 *       err();
 * @endcode
 * @see clixon_xml_diff2cbuf
 */
int
clixon_text_diff2cbuf(cbuf  *cb,
                     cxobj  *x0,
                     cxobj  *x1)
{
    return text_diff2cbuf(cb, x0, x1, 0, 1);
}

/*! Look for YANG lists nodes and convert bodies to keys
 *
 * This is a compromise between making the text parser (1) YANG aware or (2) not.
 * (1) The reason for is that some constructs such as "list <keyval1> {" does not
 * contain enough info (eg name of XML tag for <keyval1>)
 * (2) The reason against is of principal of making the parser design simpler in a bottom-up mode
 * The compromise between (1) and (2) is to first parse without YANG (2)  and then call a special
 * function after YANG binding to populate key tags properly.
 * @param[in]  xn   XML node
 * @retval     0    OK
 * @retval    -1    Error
 * @see text_mark_bodies where marking of bodies made transformed here
 */
static int
text_populate_list(cxobj *xn)
{
    int        retval = -1;
    yang_stmt *yn;
    yang_stmt *yc;
    cxobj     *xc;
    cxobj     *xb;
    cvec      *cvk; /* vector of index keys */
    cg_var    *cvi = NULL;
    char      *namei;

    if ((yn = xml_spec(xn)) == NULL)
        goto ok;
    if (yang_keyword_get(yn) == Y_LIST){
        cvk = yang_cvec_get(yn);
        /* Loop over bodies and keys and create key leafs 
         */
        cvi = NULL;
        xb = NULL;
        while ((xb = xml_find_type(xn, NULL, NULL, CX_BODY)) != NULL) {
            if (!xml_flag(xb, XML_FLAG_BODYKEY))
                continue;
            xml_flag_reset(xb, XML_FLAG_BODYKEY);
            if ((cvi = cvec_next(cvk, cvi)) == NULL){
                clicon_err(OE_XML, 0, "text parser, key and body mismatch");
                goto done;
            }
            namei = cv_string_get(cvi);
            if ((xc = xml_new(namei, xn, CX_ELMNT)) == NULL)
                goto done;
            yc = yang_find(yn, Y_LEAF, namei);
            xml_spec_set(xc, yc);
            if ((xml_addsub(xc, xb)) < 0)
                goto done;

        }
        if (xml_sort(xn) < 0)
            goto done;
    }
    xc = NULL;
    while ((xc = xml_child_each(xn, xc, CX_ELMNT)) != NULL) {
        if (text_populate_list(xc) < 0)
            goto done;
    }
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Parse a string containing text syntax and return an XML tree
 *
 * @param[in]  str    Input string containing JSON
 * @param[in]  rfc7951 Do sanity checks according to RFC 7951 JSON Encoding of Data Modeled with YANG
 * @param[in]  yb     How to bind yang to XML top-level when parsing (if rfc7951)
 * @param[in]  yspec  Yang specification (if rfc 7951)
 * @param[out] xt     XML top of tree typically w/o children on entry (but created)
 * @param[out] xerr   Reason for invalid returned as netconf err msg 
 * @retval     1      OK and valid
 * @retval     0      Invalid (only if yang spec)
 * @retval    -1      Error with clicon_err called
 * @see _xml_parse for XML variant
 * @note Parsing requires YANG, which means yb must be YB_MODULE/_NEXT
 */
static int
_text_syntax_parse(char      *str,
                   yang_bind  yb,
                   yang_stmt *yspec,
                   cxobj     *xt,
                   cxobj    **xerr)
{
    int                     retval = -1;
    clixon_text_syntax_yacc ts = {0,};
    int                     ret;
    cxobj                  *x;
    cbuf                   *cberr = NULL;
    int                     failed = 0; /* yang assignment */
    cxobj                  *xc;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s %d %s", __FUNCTION__, yb, str);
    if (yb != YB_MODULE && yb != YB_MODULE_NEXT){
        clicon_err(OE_YANG, EINVAL, "yb must be YB_MODULE or YB_MODULE_NEXT");
        return -1;
    }
    ts.ts_parse_string = str;
    ts.ts_linenum = 1;
    ts.ts_xtop = xt;
    ts.ts_yspec = yspec;
    if (clixon_text_syntax_parsel_init(&ts) < 0)
        goto done;
    if (clixon_text_syntax_parseparse(&ts) != 0) { /* yacc returns 1 on error */
        clicon_log(LOG_NOTICE, "TEXT SYNTAX error: line %d", ts.ts_linenum);
        if (clicon_errno == 0)
            clicon_err(OE_JSON, 0, "TEXT SYNTAX parser error with no error code (should not happen)");
        goto done;
    }

    x = NULL;
    while ((x = xml_child_each(ts.ts_xtop, x, CX_ELMNT)) != NULL) {
        /* Populate, ie associate xml nodes with yang specs
         */
        switch (yb){
        case YB_MODULE_NEXT:
            if ((ret = xml_bind_yang(NULL, x, YB_MODULE, yspec, xerr)) < 0)
                goto done;
            if (ret == 0)
                failed++;
            break;
        case YB_MODULE:
            /* xt:<top>     nospec
             * x:   <a> <-- populate from modules
             */
            if ((ret = xml_bind_yang0(NULL, x, YB_MODULE, yspec, xerr)) < 0)
                goto done;
            if (ret == 0)
                failed++;
            break;
        default: /* shouldnt happen */
            break;
        } /* switch */
        /* Look for YANG lists nodes and convert bodies to keys */
        xc = NULL;
        while ((xc = xml_child_each(x, xc, CX_ELMNT)) != NULL)
            if (text_populate_list(xc) < 0)
                goto done;
    }
    if (failed)
        goto fail;

    /* Sort the complete tree after parsing. Sorting is not really meaningful if Yang 
       not bound */
    if (yb != YB_NONE)
        if (xml_sort_recurse(xt) < 0)
            goto done;
    retval = 1;
 done:
    clixon_debug(CLIXON_DBG_DEFAULT, "%s retval:%d", __FUNCTION__, retval);
    if (cberr)
        cbuf_free(cberr);
    clixon_text_syntax_parsel_exit(&ts);
    return retval;
 fail: /* invalid */
    retval = 0;
    goto done;
}

/*! Parse string containing TEXT syntax and return an XML tree
 *
 * @param[in]     str   String containing TEXT syntax
 * @param[in]     yb    How to bind yang to XML top-level when parsing
 * @param[in]     yspec Yang specification, mandatory to make module->xmlns translation
 * @param[in,out] xt    Top object, if not exists, on success it is created with name 'top'
 * @param[out]    xerr  Reason for invalid returned as netconf err msg 
 * @retval        1     OK and valid
 * @retval        0     Invalid (only if yang spec) w xerr set
 * @retval       -1     Error with clicon_err called
 * @code
 *  cxobj *x = NULL;
 *  if (clixon_text_syntax_parse_string(str, YB_MODULE, yspec, &x, &xerr) < 0)
 *    err;
 *  xml_free(x);
 * @endcode
 * @note  you need to free the xml parse tree after use, using xml_free()
 * @see clixon_text_syntax_parse_file   From a file
 */
int
clixon_text_syntax_parse_string(char      *str,
                                yang_bind  yb,
                                yang_stmt *yspec,
                                cxobj    **xt,
                                cxobj    **xerr)
{
    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    if (xt==NULL){
        clicon_err(OE_XML, EINVAL, "xt is NULL");
        return -1;
    }
    if (*xt == NULL){
        if ((*xt = xml_new("top", NULL, CX_ELMNT)) == NULL)
            return -1;
    }
    return _text_syntax_parse(str, yb, yspec, *xt, xerr);
}

/*! Read a TEXT syntax definition from file and parse it into a parse-tree. 
 *
 * File will be parsed as follows:
 *   (1) parsed according to TEXT syntax; # Only this check if yspec is NULL
 *   (2) sanity checked wrt yang  
 *   (4) an xml parse tree will be returned
 * 
 * @param[in]     fp    File descriptor to the TEXT syntax file
 * @param[in]     yb    How to bind yang to XML top-level when parsing
 * @param[in]     yspec Yang specification, or NULL
 * @param[in,out] xt    Pointer to (XML) parse tree. If empty, create.
 * @param[out]    xerr  Reason for invalid returned as netconf err msg 
 * @retval        1     OK and valid
 * @retval        0     Invalid (only if yang spec) w xerr set
 * @retval       -1     Error with clicon_err called
 *
 * @code
 *  cxobj *xt = NULL;
 *  if (clixon_text_syntax_parse_file(stdin, YB_MODULE, yspec, &xt) < 0)
 *    err;
 *  xml_free(xt);
 * @endcode
 * @note  you need to free the xml parse tree after use, using xml_free()
 * @note, If xt empty, a top-level symbol will be added so that <tree../> will be:  <top><tree.../></tree></top>
 * @note May block on file I/O
 * @note Parsing requires YANG, which means yb must be YB_MODULE/_NEXT
 *
 * @see clixon_text_syntax_parse_string
 */
int
clixon_text_syntax_parse_file(FILE      *fp,
                              yang_bind  yb,
                              yang_stmt *yspec,
                              cxobj    **xt,
                              cxobj    **xerr)
{
    int       retval = -1;
    int       ret;
    char     *textbuf = NULL;
    int       textbuflen = BUFLEN; /* start size */
    int       oldtextbuflen;
    char     *ptr;
    char      ch;
    int       len = 0;

    if (xt == NULL){
        clicon_err(OE_XML, EINVAL, "xt is NULL");
        return -1;
    }
    if ((textbuf = malloc(textbuflen)) == NULL){
        clicon_err(OE_XML, errno, "malloc");
        goto done;
    }
    memset(textbuf, 0, textbuflen);
    ptr = textbuf;
    while (1){
        if ((ret = fread(&ch, 1, 1, fp)) < 0){
            clicon_err(OE_XML, errno, "read");
            break;
        }
        if (ret != 0)
            textbuf[len++] = ch;
        if (ret == 0){
            if (*xt == NULL)
                if ((*xt = xml_new(TEXT_TOP_SYMBOL, NULL, CX_ELMNT)) == NULL)
                    goto done;
            if (len){
                if ((ret = _text_syntax_parse(ptr, yb, yspec, *xt, xerr)) < 0)
                    goto done;
                if (ret == 0)
                    goto fail;
            }
            break;
        }
        if (len >= textbuflen-1){ /* Space: one for the null character */
            oldtextbuflen = textbuflen;
            textbuflen *= 2;
            if ((textbuf = realloc(textbuf, textbuflen)) == NULL){
                clicon_err(OE_XML, errno, "realloc");
                goto done;
            }
            memset(textbuf+oldtextbuflen, 0, textbuflen-oldtextbuflen);
            ptr = textbuf;
        }
    }
    retval = 1;
 done:
    if (retval < 0 && *xt){
        free(*xt);
        *xt = NULL;
    }
    if (textbuf)
        free(textbuf);
    return retval;
 fail:
    retval = 0;
    goto done;
}

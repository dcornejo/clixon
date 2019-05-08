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
/*
 * Note that the functions in this file are accessible from the plugins
 */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <regex.h>
#include <netinet/in.h>
#include <limits.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_backend_transaction.h"
#include "backend_plugin.h"

/* Access functions for transaction-data handle in callbacks
 * Expressed in a transition from an current -> wanted state.
 * For example, adding a database symbol 'a' in candidate and commiting
 * would give running in source and 'a' and candidate in 'target'.
 */
/*! Get transaction id
 * @param[in]  td   transaction_data
 * @retval     id   transaction id
 */
uint64_t
transaction_id(transaction_data td)
{
    return ((transaction_data_t *)td)->td_id;
}

/*! Get plugin/application specific callback argument
 * @param[in]  td   transaction_data
 * @retval     arg  callback argument
 * @note NYI
 */
void *
transaction_arg(transaction_data td)
{
  return ((transaction_data_t *)td)->td_arg;
}

/*! Get source database xml tree
 * @param[in]  td   transaction_data
 * @retval     src  source xml tree containing original state
 */
cxobj *
transaction_src(transaction_data td)
{
  return ((transaction_data_t *)td)->td_src;
}

/*! Get target database xml tree
 * @param[in]  td   transaction_data
 * @retval     xml  target xml tree containing wanted state
 */
cxobj *
transaction_target(transaction_data td)
{
  return ((transaction_data_t *)td)->td_target;
}

/*! Get delete xml vector, ie vector of xml nodes that are deleted src->target
 * @param[in]  td   transaction_data
 * @retval     vec  Vector of xml nodes
 */
cxobj **
transaction_dvec(transaction_data td)
{
  return ((transaction_data_t *)td)->td_dvec;
}

/*! Get length of delete xml vector
 * @param[in]  td   transaction_data
 * @retval     len  Length of vector of xml nodes
 * @see transaction_dvec
 */
size_t
transaction_dlen(transaction_data td)
{
  return ((transaction_data_t *)td)->td_dlen;
}

/*! Get add xml vector, ie vector of xml nodes that are added src->target
 * @param[in]  td   transaction_data
 * @retval     vec  Vector of xml nodes
 */
cxobj **
transaction_avec(transaction_data td)
{
  return ((transaction_data_t *)td)->td_avec;
}

/*! Get length of add xml vector
 * @param[in]  td   transaction_data
 * @retval     len  Length of vector of xml nodes
 * @see transaction_avec
 */
size_t
transaction_alen(transaction_data td)
{
  return ((transaction_data_t *)td)->td_alen;
}

/*! Get source changed xml vector, ie vector of xml nodes that changed
 * @param[in]  td    transaction_data
 * @retval     vec   Vector of xml nodes
 * These are only nodes of type LEAF. 
 * For each node in this vector which contains the original value, there
 * is a node in tcvec with the changed value
 * @see transaction_dcvec
 */
cxobj **
transaction_scvec(transaction_data td)
{
  return ((transaction_data_t *)td)->td_scvec;
}

/*! Get target changed xml vector, ie vector of xml nodes that changed
 * @param[in]  td    transaction_data
 * @retval     vec   Vector of xml nodes
 * These are only nodes of type LEAF. 
 * For each node in this vector which contains the original value, there
 * is a node in tcvec with the changed value
 * @see transaction_scvec
 */
cxobj **
transaction_tcvec(transaction_data td)
{
  return ((transaction_data_t *)td)->td_tcvec;
}

/*! Get length of changed xml vector
 * @param[in]  td   transaction_data
 * @retval     len Length of vector of xml nodes
 * This is the length of both the src change vector and the target change vector
 */
size_t
transaction_clen(transaction_data td)
{
  return ((transaction_data_t *)td)->td_clen;
}

/*! Print transaction on FILE for debug
 * @see transaction_log
 */
int
transaction_print(FILE               *f,
		  transaction_data   th)
{
    cxobj *xn;
    int i;
    transaction_data_t *td;

    td = (transaction_data_t *)th;

    fprintf(f, "Transaction id: 0x%" PRIu64 "\n", td->td_id);
    fprintf(f, "Removed\n=========\n");
    for (i=0; i<td->td_dlen; i++){
	xn = td->td_dvec[i];
	xml_print(f, xn);
    }
    fprintf(f, "Added\n=========\n");
    for (i=0; i<td->td_alen; i++){
	xn = td->td_avec[i];
	xml_print(f, xn);
    }
    fprintf(stderr, "Changed\n=========\n");
    for (i=0; i<td->td_clen; i++){
	xn = td->td_scvec[i];
	xml_print(f, xn);
	xn = td->td_tcvec[i];
	xml_print(f, xn);
    }
    return 0;
}

int
transaction_log(clicon_handle      h,
		transaction_data   th,
		int                level,
		const char        *op)
{
    cxobj              *xn;
    int                 i;
    transaction_data_t *td;
    cbuf               *cb = NULL;

    td = (transaction_data_t *)th;
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_CFG, errno, "cbuf_new");
	goto done;
    }
    for (i=0; i<td->td_dlen; i++){
	xn = td->td_dvec[i];
	clicon_xml2cbuf(cb, xn, 0, 0);
    }
    if (i)
	clicon_log(level, "%s %" PRIu64 " %s del: %s",
		   __FUNCTION__,  td->td_id, op, cbuf_get(cb));
    cbuf_reset(cb);
    for (i=0; i<td->td_alen; i++){
	xn = td->td_avec[i];
	clicon_xml2cbuf(cb, xn, 0, 0);
    }
    if (i)
	clicon_log(level, "%s %" PRIu64 " %s add: %s", __FUNCTION__, td->td_id, op, cbuf_get(cb));
    cbuf_reset(cb);
    for (i=0; i<td->td_clen; i++){
	if (td->td_scvec){
	    xn = td->td_scvec[i];
	    clicon_xml2cbuf(cb, xn, 0, 0);
	}
	xn = td->td_tcvec[i];
	clicon_xml2cbuf(cb, xn, 0, 0);
    }
    if (i)
	clicon_log(level, "%s %" PRIu64 " %s change: %s", __FUNCTION__, td->td_id, op, cbuf_get(cb));
 done:
    if (cb)
	cbuf_free(cb);
    return 0;
}

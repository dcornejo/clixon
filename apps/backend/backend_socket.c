/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC (Netgate)

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
#include <grp.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/param.h>
#define __USE_GNU   /* for ucred */
#define _GNU_SOURCE /* for ucred */
#include <sys/socket.h>
#ifdef HAVE_LOCAL_PEERCRED
#include <sys/ucred.h>
#endif
#include <sys/param.h>
#include <sys/types.h>

#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "backend_socket.h"
#include "clixon_backend_client.h"
#include "backend_client.h"
#include "backend_handle.h"

/*! Open an INET stream socket and bind it to a file descriptor
 *
 * @param[in]  h    Clixon handle
 * @param[in]  dst  IPv4 address (see inet_pton(3))
 * @retval     s    Socket file descriptor (see socket(2))
 * @retval    -1    Error
 */
static int
config_socket_init_ipv4(clicon_handle h,
                        char         *dst)
{
    int                s;
    struct sockaddr_in addr;
    uint16_t           port;
    int                one = 1;

    port = clicon_sock_port(h);

    /* create inet socket */
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        clicon_err(OE_UNIX, errno, "socket");
        return -1;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void*)&one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(addr.sin_family, dst, &addr.sin_addr) != 1){
        clicon_err(OE_UNIX, errno, "inet_pton: %s (Expected IPv4 address. Check settings of CLICON_SOCK_FAMILY and CLICON_SOCK)", dst);
        goto err; /* Could check getaddrinfo */
    }
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0){
        clicon_err(OE_UNIX, errno, "bind");
        goto err;
    }
    clixon_debug(CLIXON_DBG_DEFAULT, "Listen on server socket at %s:%hu", dst, port);
    if (listen(s, 5) < 0){
        clicon_err(OE_UNIX, errno, "listen");
        goto err;
    }
    return s;
  err:
    close(s);
    return -1;
}

/*! Open a UNIX domain socket and bind it to a file descriptor
 *
 * The socket is accessed via CLICON_SOCK option, has 770 permissions
 * and group according to CLICON_SOCK_GROUP option.
 * @param[in]  h    Clixon handle
 * @param[in]  sock Unix file-system path
 * @retval     s    Socket file descriptor (see socket(2))
 * @retval    -1    Error
 */
static int
config_socket_init_unix(clicon_handle h,
                        char         *sock)
{
    int                s;
    struct sockaddr_un addr;
    mode_t             old_mask;
    char              *config_group;
    gid_t              gid;
    struct stat        st;

    if (lstat(sock, &st) == 0 && unlink(sock) < 0){
        clicon_err(OE_UNIX, errno, "unlink(%s)", sock);
        return -1;
    }
    /* then find configuration group (for clients) and find its groupid */
    if ((config_group = clicon_sock_group(h)) == NULL){
        clicon_err(OE_FATAL, 0, "clicon_sock_group option not set");
        return -1;
    }
    if (group_name2gid(config_group, &gid) < 0)
        return -1;
#if 0
    if (gid == 0)
        clicon_log(LOG_WARNING, "%s: No such group: %s", __FUNCTION__, config_group);
#endif
    /* create unix socket */
    if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        clicon_err(OE_UNIX, errno, "socket");
        return -1;
    }
//    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void*)&one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path)-1);
    old_mask = umask(S_IRWXO | S_IXGRP | S_IXUSR);
    if (bind(s, (struct sockaddr *)&addr, SUN_LEN(&addr)) < 0){
        clicon_err(OE_UNIX, errno, "bind");
        umask(old_mask);
        goto err;
    }
    umask(old_mask);
    /* change socket path file group */
    if (lchown(sock, -1, gid) < 0){
        clicon_err(OE_UNIX, errno, "lchown(%s, %s)", sock, config_group);
        goto err;
    }
    clixon_debug(CLIXON_DBG_DEFAULT, "Listen on server socket at %s", addr.sun_path);
    if (listen(s, 5) < 0){
        clicon_err(OE_UNIX, errno, "listen");
        goto err;
    }
    return s;
  err:
    close(s);
    return -1;
}

/*! Open backend socket, the one clients send requests to, either ip or unix
 *
 * @param[in]  h    Clixon handle
 * @retval     s    Socket file descriptor (see socket(2))
 * @retval    -1    Error
 */
int
backend_socket_init(clicon_handle h)
{
    char *sock; /* unix path or ip address string */

    if ((sock = clicon_sock_str(h)) == NULL){
        clicon_err(OE_FATAL, 0, "CLICON_SOCK option not set");
        return -1;
    }
    switch (clicon_sock_family(h)){
    case AF_UNIX:
        return config_socket_init_unix(h, sock);
        break;
    case AF_INET:
        return config_socket_init_ipv4(h, sock);
        break;
    default:
        clicon_err(OE_UNIX, EINVAL, "No such address family: %d",
                   clicon_sock_family(h));
        break;
    }
    return -1;
}

/*! Accept new socket client
 *
 * @param[in]  fd   Socket (unix or ip)
 * @param[in]  arg  typecast clicon_handle
 * @retval     0    OK
 * @retval    -1    Error
 */
int
backend_accept_client(int   fd,
                      void *arg)
{
    int                  retval = -1;
    clicon_handle        h = (clicon_handle)arg;
    int                  s;
    struct sockaddr      from = {0,};
    socklen_t            len;
    struct client_entry *ce;
    char                *name = NULL;
#ifdef HAVE_SO_PEERCRED        /* Linux. */
    socklen_t            clen;
    struct ucred         cr = {0,};
#elif defined(HAVE_GETPEEREID) /* FreeBSD */
    uid_t                euid;
    uid_t                guid;
#endif

    clixon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
    len = sizeof(from);
    if ((s = accept(fd, &from, &len)) < 0){
        clicon_err(OE_UNIX, errno, "accept");
        goto done;
    }
    if ((ce = backend_client_add(h, &from)) == NULL)
        goto done;

    /*
     * Get credentials of connected peer - only for unix socket 
     */
    switch (from.sa_family){
    case AF_UNIX:
#if defined(HAVE_SO_PEERCRED)
        clen =  sizeof(cr);
        if(getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &clen) < 0){
            clicon_err(OE_UNIX, errno, "getsockopt");
            goto done;
        }
        if (uid2name(cr.uid, &name) < 0)
            goto done;
#elif defined(HAVE_GETPEEREID)
        if (getpeereid(s, &euid, &guid) < 0)
            goto done;
        if (uid2name(euid, &name) < 0)
            goto done;
#else
#error "Need getsockopt O_PEERCRED or getpeereid for unix socket peer cred"
#endif
        if (name != NULL){
            if ((ce->ce_username = name) == NULL){
                clicon_err(OE_UNIX, errno, "strdup");
                name = NULL;
                goto done;
            }
            name = NULL;
        }
        break;
    case AF_INET:
        break;
    case AF_INET6:
    default:
        break;
    }
    ce->ce_s = s;

    /*
     * Here we register callbacks for actual data socket 
     */
    if (clixon_event_reg_fd(s, from_client, (void*)ce, "local netconf client socket") < 0)
        goto done;
    retval = 0;
 done:
    if (name)
        free(name);
    return retval;
}

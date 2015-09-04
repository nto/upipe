/*
 * Copyright (c) 2015 Arnaud de Turckheim <quarium@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/** @file
 * @short Upipe tcp source module
 */

#include <upipe/uuri.h>
#include <upipe/uref_block.h>
#include <upipe/uref_block_flow.h>
#include <upipe/uref_clock.h>
#include <upipe/upipe.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_void.h>
#include <upipe/upipe_helper_upump_mgr.h>
#include <upipe/upipe_helper_upump.h>
#include <upipe/upipe_helper_ubuf_mgr.h>
#include <upipe/upipe_helper_uref_mgr.h>
#include <upipe/upipe_helper_uclock.h>
#include <upipe/upipe_helper_output.h>
#include <upipe/upipe_helper_output_size.h>
#include <upipe-modules/upipe_tcp_source.h>

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

/** @showvalue default size of buffers when unspecified */
#define UBUF_DEFAULT_SIZE       4096

/** @hidden */
static int upipe_tcpsrc_check(struct upipe *upipe, struct uref *flow_format);
/** @hidden */
static void upipe_tcpsrc_close(struct upipe *upipe);

static inline int upipe_tcpsrc_throw_accepted(struct upipe *upipe)
{
    upipe_dbg(upipe, "throw accepted");
    return upipe_throw(upipe, UPROBE_TCPSRC_ACCEPTED, UPIPE_TCPSRC_SIGNATURE);
}

enum upipe_tcpsrc_state {
    /** socket is not initialized */
    STATE_NONE = 0,
    /** wait for an incoming connection */
    STATE_ACCEPTING,
    /** connection is established */
    STATE_ACCEPTED,
};

/** @internal @This converts @tt {enum upipe_tcpsrc_state} to a string.
 *
 * @param state a tcp socket state
 * @return a string or NULL
 */
static inline const char *upipe_tcpsrc_state_str(enum upipe_tcpsrc_state state)
{
    switch (state) {
    case STATE_NONE: return "none";
    case STATE_ACCEPTING: return "accepting";
    case STATE_ACCEPTED: return "accepted";
    }
    return NULL;
}

/** @internal @This stores the private context of a tcp socket source pipe. */
struct upipe_tcpsrc {
    /** public upipe structure */
    struct upipe upipe;
    /** refcount structure */
    struct urefcount urefcount;
    /** upump manager */
    struct upump_mgr *upump_mgr;
    /** upump for tcp socket */
    struct upump *upump;
    /** ubuf manager */
    struct ubuf_mgr *ubuf_mgr;
    /** urequest for ubuf manager */
    struct urequest ubuf_mgr_request;
    /** uref manager */
    struct uref_mgr *uref_mgr;
    /** urequest for uref manager */
    struct urequest uref_mgr_request;
    /** uclock */
    struct uclock *uclock;
    /** urequest for uclock */
    struct urequest uclock_request;
    /** flow format */
    struct uref *flow_format;
    /** output pipe */
    struct upipe *output;
    /** output state */
    enum upipe_helper_output_state output_state;
    /** request list */
    struct uchain requests;
    /** read size */
    unsigned int output_size;
    /** tcp socket */
    int fd;
    /** tcp uri */
    char *uri;
    /** state */
    enum upipe_tcpsrc_state state;
};

UPIPE_HELPER_UPIPE(upipe_tcpsrc, upipe, UPIPE_TCPSRC_SIGNATURE);
UPIPE_HELPER_UREFCOUNT(upipe_tcpsrc, urefcount, upipe_tcpsrc_free);
UPIPE_HELPER_VOID(upipe_tcpsrc);
UPIPE_HELPER_UPUMP_MGR(upipe_tcpsrc, upump_mgr);
UPIPE_HELPER_UPUMP(upipe_tcpsrc, upump, upump_mgr);
UPIPE_HELPER_OUTPUT(upipe_tcpsrc, output, flow_format, output_state, requests)
UPIPE_HELPER_UBUF_MGR(upipe_tcpsrc, ubuf_mgr, flow_format, ubuf_mgr_request,
                      upipe_tcpsrc_check,
                      upipe_tcpsrc_register_output_request,
                      upipe_tcpsrc_unregister_output_request);
UPIPE_HELPER_UREF_MGR(upipe_tcpsrc, uref_mgr, uref_mgr_request,
                      upipe_tcpsrc_check,
                      upipe_tcpsrc_register_output_request,
                      upipe_tcpsrc_unregister_output_request);
UPIPE_HELPER_UCLOCK(upipe_tcpsrc, uclock, uclock_request,
                    upipe_tcpsrc_check,
                    upipe_tcpsrc_register_output_request,
                    upipe_tcpsrc_unregister_output_request);
UPIPE_HELPER_OUTPUT_SIZE(upipe_tcpsrc, output_size);

/** @internal @This allocates a tcp socket source pipe.
 *
 * @param mgr pointer to upipe manager
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_tcpsrc_alloc(struct upipe_mgr *mgr,
                                        struct uprobe *uprobe,
                                        uint32_t signature,
                                        va_list args)
{
    struct upipe *upipe =
        upipe_tcpsrc_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(upipe == NULL))
        return NULL;

    upipe_tcpsrc_init_urefcount(upipe);
    upipe_tcpsrc_init_ubuf_mgr(upipe);
    upipe_tcpsrc_init_uref_mgr(upipe);
    upipe_tcpsrc_init_upump_mgr(upipe);
    upipe_tcpsrc_init_upump(upipe);
    upipe_tcpsrc_init_output(upipe);
    upipe_tcpsrc_init_uclock(upipe);
    upipe_tcpsrc_init_output_size(upipe, UBUF_DEFAULT_SIZE);

    struct upipe_tcpsrc *upipe_tcpsrc = upipe_tcpsrc_from_upipe(upipe);
    upipe_tcpsrc->fd = -1;
    upipe_tcpsrc->uri = NULL;
    upipe_tcpsrc->state = STATE_NONE;

    upipe_throw_ready(upipe);

    return upipe;
}

/** @internal @This frees a tcp socket source pipe.
 *
 * @param upipe public description structure of the pipe
 */
static void upipe_tcpsrc_free(struct upipe *upipe)
{
    struct upipe_tcpsrc *upipe_tcpsrc = upipe_tcpsrc_from_upipe(upipe);

    upipe_throw_dead(upipe);

    upipe_tcpsrc_close(upipe);
    free(upipe_tcpsrc->uri);
    upipe_tcpsrc_clean_output_size(upipe);
    upipe_tcpsrc_clean_uclock(upipe);
    upipe_tcpsrc_clean_output(upipe);
    upipe_tcpsrc_clean_upump(upipe);
    upipe_tcpsrc_clean_upump_mgr(upipe);
    upipe_tcpsrc_clean_uref_mgr(upipe);
    upipe_tcpsrc_clean_ubuf_mgr(upipe);
    upipe_tcpsrc_clean_urefcount(upipe);
    upipe_tcpsrc_free_void(upipe);
}

/** @internal @This sets the socket state.
 *
 * @param upipe description structure of the pipe
 * @param state the socket state to set
 * @return an error code
 */
static void upipe_tcpsrc_set_state(struct upipe *upipe,
                                   enum upipe_tcpsrc_state state)
{
    struct upipe_tcpsrc *upipe_tcpsrc = upipe_tcpsrc_from_upipe(upipe);

    if (state == upipe_tcpsrc->state)
        return;

    upipe_dbg_va(upipe, "%s -> %s",
                 upipe_tcpsrc_state_str(upipe_tcpsrc->state),
                 upipe_tcpsrc_state_str(state));
    upipe_tcpsrc->state = state;

    if (state == STATE_ACCEPTED)
        upipe_tcpsrc_throw_accepted(upipe);
}

/** @internal @This reads data from the source and outputs it.
 *
 * @param upump description structure of the read watcher
 */
static void upipe_tcpsrc_worker(struct upump *upump)
{
    struct upipe *upipe = upump_get_opaque(upump, struct upipe *);
    struct upipe_tcpsrc *upipe_tcpsrc = upipe_tcpsrc_from_upipe(upipe);

    struct uref *uref = uref_block_alloc(upipe_tcpsrc->uref_mgr,
                                         upipe_tcpsrc->ubuf_mgr,
                                         upipe_tcpsrc->output_size);
    if (unlikely(uref == NULL)) {
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return;
    }

    uint8_t *buffer;
    int output_size = -1;
    if (unlikely(!ubase_check(uref_block_write(
                    uref, 0, &output_size, &buffer)))) {
        uref_free(uref);
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return;
    }
    assert(output_size == upipe_tcpsrc->output_size);

    ssize_t ret = read(upipe_tcpsrc->fd, buffer, upipe_tcpsrc->output_size);
    uref_block_unmap(uref, 0);
    if (unlikely(ret < 0)) {
        uref_free(uref);
        switch (errno) {
            case EINTR:
            case EAGAIN:
#if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
#endif
                /* not an issue, try again later */
                return;
            default:
                break;
        }
        upipe_err_va(upipe, "read error from %s (%m)", upipe_tcpsrc->uri);
        upipe_tcpsrc_set_upump(upipe, NULL);
        upipe_throw_source_end(upipe);
    }
    else {
        if (unlikely(ret != upipe_tcpsrc->output_size))
            uref_block_resize(uref, 0, ret);
        if (unlikely(ret == 0))
            uref_block_set_end(uref);
        if (unlikely(upipe_tcpsrc->uclock != NULL))
            uref_clock_set_cr_sys(uref, uclock_now(upipe_tcpsrc->uclock));
        upipe_use(upipe);
        upipe_tcpsrc_output(upipe, uref, &upipe_tcpsrc->upump);
        if (unlikely(ret == 0)) {
            upipe_notice_va(upipe, "end of tcp socket %s", upipe_tcpsrc->uri);
            upipe_tcpsrc_set_upump(upipe, NULL);
            upipe_throw_source_end(upipe);
        }
        upipe_release(upipe);
    }
}

/** @internal @This accepts the first connection on the socket.
 *
 * @param upump description structure of the accept watcher
 */
static void upipe_tcpsrc_accept(struct upump *upump)
{
    struct upipe *upipe = upump_get_opaque(upump, struct upipe *);
    struct upipe_tcpsrc *upipe_tcpsrc = upipe_tcpsrc_from_upipe(upipe);

    upipe_dbg(upipe, "accepting...");
    int fd = accept(upipe_tcpsrc->fd, NULL, NULL);
    if (fd < 0) {
        upipe_warn(upipe, "accept: %m");
        return;
    }
    upipe_tcpsrc_set_upump(upipe, NULL);
    close(upipe_tcpsrc->fd);
    upipe_tcpsrc->fd = fd;
    upipe_tcpsrc_set_state(upipe, STATE_ACCEPTED);
    upipe_tcpsrc_check(upipe, NULL);
}

/** @internal @This checks if the pump may be allocated.
 *
 * @param upipe description structure of the pipe
 * @param flow_format amended flow format
 * @return an error code
 */
static int upipe_tcpsrc_check(struct upipe *upipe, struct uref *flow_format)
{
    struct upipe_tcpsrc *upipe_tcpsrc = upipe_tcpsrc_from_upipe(upipe);

    if (flow_format != NULL)
        upipe_tcpsrc_store_flow_def(upipe, flow_format);

    upipe_tcpsrc_check_upump_mgr(upipe);
    if (unlikely(upipe_tcpsrc->upump_mgr == NULL))
        return UBASE_ERR_NONE;

    if (unlikely(upipe_tcpsrc->uref_mgr == NULL)) {
        upipe_tcpsrc_require_uref_mgr(upipe);
        return UBASE_ERR_NONE;
    }

    if (upipe_tcpsrc->ubuf_mgr == NULL) {
        struct uref *flow_format =
            uref_block_flow_alloc_def(upipe_tcpsrc->uref_mgr, NULL);
        uref_block_flow_set_size(flow_format, upipe_tcpsrc->output_size);
        if (unlikely(flow_format == NULL)) {
            upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
            return UBASE_ERR_ALLOC;
        }
        upipe_tcpsrc_require_ubuf_mgr(upipe, flow_format);
        return UBASE_ERR_NONE;
    }

    if (upipe_tcpsrc->uclock == NULL &&
        urequest_get_opaque(&upipe_tcpsrc->uclock_request, struct upipe *)
            != NULL)
        return UBASE_ERR_NONE;

    if (upipe_tcpsrc->fd != -1 && upipe_tcpsrc->upump == NULL) {
        struct upump *upump;

        switch (upipe_tcpsrc->state) {
        case STATE_NONE:
            upipe_err(upipe, "invalid state");
            return UBASE_ERR_INVALID;

        case STATE_ACCEPTING:
            upump = upump_alloc_fd_read(upipe_tcpsrc->upump_mgr,
                                        upipe_tcpsrc_accept,
                                        upipe, upipe_tcpsrc->fd);
            break;

        case STATE_ACCEPTED:
            upump = upump_alloc_fd_read(upipe_tcpsrc->upump_mgr,
                                        upipe_tcpsrc_worker, upipe,
                                        upipe_tcpsrc->fd);
            break;
        }

        if (unlikely(upump == NULL)) {
            upipe_throw_fatal(upipe, UBASE_ERR_UPUMP);
            return UBASE_ERR_UPUMP;
        }
        upipe_tcpsrc_set_upump(upipe, upump);
        upump_start(upump);
    }
    return UBASE_ERR_NONE;
}

/** @internal @This sets the flow format.
 *
 * @param upipe public description structure of the pipe
 * @param flow_def flow definition packet
 * @return an error code
 */
static int upipe_tcpsrc_set_flow_def(struct upipe *upipe,
                                     struct uref *flow_def)
{
    struct uref *flow_def_dup = uref_dup(flow_def);
    if (unlikely(flow_def_dup == NULL)) {
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return UBASE_ERR_ALLOC;
    }
    upipe_tcpsrc_store_flow_def(upipe, flow_def_dup);
    return UBASE_ERR_NONE;
}

/** @internal @This closes the tcp socket.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_tcpsrc_close(struct upipe *upipe)
{
    struct upipe_tcpsrc *upipe_tcpsrc = upipe_tcpsrc_from_upipe(upipe);

    upipe_tcpsrc_set_upump(upipe, NULL);
    ubase_clean_fd(&upipe_tcpsrc->fd);
    ubase_clean_str(&upipe_tcpsrc->uri);
    upipe_tcpsrc_set_state(upipe, STATE_NONE);
}

/** @internal @This opens a tcp socket.
 *
 * @param upipe description structure of the pipe
 * @param uri the tcp socket uri
 * @return an error code
 */
static int upipe_tcpsrc_open(struct upipe *upipe, const char *uri)
{
    struct upipe_tcpsrc *upipe_tcpsrc = upipe_tcpsrc_from_upipe(upipe);
    int ret;

    struct uuri uuri = uuri_null();
    UBASE_RETURN(uuri_from_str(&uuri, uri));
    if (ustring_cmp(uuri.scheme, ustring_from_str("tcp")))
        return UBASE_ERR_INVALID;
    char host[uuri.authority.host.len + 1];
    UBASE_RETURN(ustring_cpy(uuri.authority.host, host, sizeof (host)));
    char port[uuri.authority.port.len + 1];
    UBASE_RETURN(ustring_cpy(uuri.authority.port, port, sizeof (port)));

    /* get socket information */
    struct addrinfo hints;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;
    upipe_verbose_va(upipe, "getaddrinfo to %s:%s", host, port);
    struct addrinfo *info = NULL;
    if (unlikely((ret = getaddrinfo(host, port, &hints, &info)) != 0)) {
        upipe_err_va(upipe, "getaddrinfo: %s", gai_strerror(ret));
        return UBASE_ERR_EXTERNAL;
    }

    /* connect to first working ressource */
    int fd = -1;
    for (struct addrinfo *res = info; res; res = res->ai_next) {
        fd = socket(res->ai_family,
                    res->ai_socktype | SOCK_NONBLOCK,
                    res->ai_protocol);
        if (unlikely(fd < 0))
            continue;

        if (unlikely(bind(fd, res->ai_addr, res->ai_addrlen) != 0)) {
            ubase_clean_fd(&fd);
            continue;
        }

        if (unlikely(listen(fd, 1) < 0)) {
            ubase_clean_fd(&fd);
            continue;
        }
    }
    freeaddrinfo(info);
    if (fd < 0) {
        upipe_err(upipe, "could not connect to any ressource");
        return UBASE_ERR_EXTERNAL;
    }
    upipe_tcpsrc->fd = fd;
    upipe_tcpsrc->uri = strdup(uri);
    if (unlikely(upipe_tcpsrc->uri == NULL)) {
        upipe_tcpsrc_close(upipe);
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return UBASE_ERR_ALLOC;
    }
    upipe_tcpsrc_set_state(upipe, STATE_ACCEPTING);
    return UBASE_ERR_NONE;
}

/** @internal @This sets the uri of the tcp socket.
 *
 * @param upipe description structure of the pipe
 * @param uri relative or absolute uri of the tcp socket
 * @return an error code
 */
static int upipe_tcpsrc_set_uri(struct upipe *upipe, const char *uri)
{
    upipe_tcpsrc_close(upipe);
    if (unlikely(uri == NULL))
        return UBASE_ERR_NONE;

    return upipe_tcpsrc_open(upipe, uri);
}

/** @internal @This gets the uri of the tcp socket.
 *
 * @param upipe description structure of the pipe
 * @param uri_p filled in with the uri of the tcp socket
 * @return an error code
 */
static int upipe_tcpsrc_get_uri(struct upipe *upipe, const char **uri_p)
{
    struct upipe_tcpsrc *upipe_tcpsrc = upipe_tcpsrc_from_upipe(upipe);
    if (uri_p)
        *uri_p = upipe_tcpsrc->uri;
    return UBASE_ERR_NONE;
}

/** @internal @This sets the socket.
 *
 * @param upipe description structure of the pipe
 * @param fd the opened socket to use or -1
 * @return an error code
 */
static int _upipe_tcpsrc_set_fd(struct upipe *upipe, int fd)
{
    struct upipe_tcpsrc *upipe_tcpsrc = upipe_tcpsrc_from_upipe(upipe);

    upipe_tcpsrc_close(upipe);
    if (unlikely(fd < 0))
        return UBASE_ERR_NONE;

    upipe_tcpsrc->fd = dup(fd);
    if (unlikely(upipe_tcpsrc->fd < 0))
        return UBASE_ERR_EXTERNAL;
    upipe_tcpsrc_set_state(upipe, STATE_ACCEPTED);
    return UBASE_ERR_NONE;
}

/** @internal @This gets the socket.
 *
 * @param upipe description structure of the pipe
 * @param fd_p pointer filled with the socket
 * @return an error code
 */
static int _upipe_tcpsrc_get_fd(struct upipe *upipe, int *fd_p)
{
    struct upipe_tcpsrc *upipe_tcpsrc = upipe_tcpsrc_from_upipe(upipe);
    if (fd_p) {
        if (upipe_tcpsrc->state == STATE_ACCEPTED)
            *fd_p = upipe_tcpsrc->fd;
        else
            *fd_p = -1;
    }
    return UBASE_ERR_NONE;
}

/** @internal @This processes control commands on a tcp socket source pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error cod e
 */
static int _upipe_tcpsrc_control(struct upipe *upipe,
                                 int command,
                                 va_list args)
{
    switch (command) {
    case UPIPE_ATTACH_UPUMP_MGR:
        upipe_tcpsrc_set_upump(upipe, NULL);
        return upipe_tcpsrc_attach_upump_mgr(upipe);
    case UPIPE_ATTACH_UCLOCK:
        upipe_tcpsrc_set_upump(upipe, NULL);
        upipe_tcpsrc_require_uclock(upipe);
        return UBASE_ERR_NONE;
    case UPIPE_GET_FLOW_DEF: {
        struct uref **flow_def_p = va_arg(args, struct uref **);
        return upipe_tcpsrc_get_flow_def(upipe, flow_def_p);
    }
    case UPIPE_SET_FLOW_DEF: {
        struct uref *flow_def = va_arg(args, struct uref *);
        return upipe_tcpsrc_set_flow_def(upipe, flow_def);
    }
    case UPIPE_SET_OUTPUT: {
        struct upipe *output = va_arg(args, struct upipe *);
        return upipe_tcpsrc_set_output(upipe, output);
    }
    case UPIPE_GET_OUTPUT: {
        struct upipe **output_p = va_arg(args, struct upipe **);
        return upipe_tcpsrc_get_output(upipe, output_p);
    }
    case UPIPE_GET_OUTPUT_SIZE: {
        unsigned int *p = va_arg(args, unsigned int *);
        return upipe_tcpsrc_get_output_size(upipe, p);
    }
    case UPIPE_SET_OUTPUT_SIZE: {
        unsigned int output_size = va_arg(args, unsigned int);
        return upipe_tcpsrc_set_output_size(upipe, output_size);
    }
    case UPIPE_GET_URI: {
        const char **uri_p = va_arg(args, const char **);
        return upipe_tcpsrc_get_uri(upipe, uri_p);
    }
    case UPIPE_SET_URI: {
        const char *uri = va_arg(args, const char *);
        return upipe_tcpsrc_set_uri(upipe, uri);
    }
    case UPIPE_TCPSRC_SET_FD: {
        UBASE_SIGNATURE_CHECK(args, UPIPE_TCPSRC_SIGNATURE);
        int fd = va_arg(args, int);
        return _upipe_tcpsrc_set_fd(upipe, fd);
    }
    case UPIPE_TCPSRC_GET_FD: {
        UBASE_SIGNATURE_CHECK(args, UPIPE_TCPSRC_SIGNATURE);
        int *fd_p = va_arg(args, int *);
        return _upipe_tcpsrc_get_fd(upipe, fd_p);
    }
    }
    return UBASE_ERR_UNHANDLED;
}

/** @internal @This processes control commands on a tcp socket source pipe, and
 * checks the status of the pipe afterwards.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_tcpsrc_control(struct upipe *upipe, int command, va_list args)
{
    UBASE_RETURN(_upipe_tcpsrc_control(upipe, command, args));

    return upipe_tcpsrc_check(upipe, NULL);
}

/** @internal @This is the static structure for tcp socket manager. */
static struct upipe_mgr upipe_tcpsrc_mgr = {
    .refcount = NULL,
    .signature = UPIPE_TCPSRC_SIGNATURE,

    .upipe_alloc = upipe_tcpsrc_alloc,
    .upipe_control = upipe_tcpsrc_control,
};

/** @This returns the management structure for tcp socket source.
 *
 * @return pointer to the tcp socket source manager
 */
struct upipe_mgr *upipe_tcpsrc_mgr_alloc(void)
{
    return &upipe_tcpsrc_mgr;
}

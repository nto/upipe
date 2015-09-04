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
 * @short Upipe tcp sink module
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
#include <upipe/upipe_helper_uclock.h>
#include <upipe/upipe_helper_input.h>
#include <upipe-modules/upipe_tcp_sink.h>

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

/** @showvalue default size of buffers when unspecified */
#define UBUF_DEFAULT_SIZE       4096

/** @hidden */
static int upipe_tcpsink_check(struct upipe *upipe, struct uref *flow_format);
/** @hidden */
static void upipe_tcpsink_close(struct upipe *upipe);
/** @hidden */
static bool upipe_tcpsink_output(struct upipe *upipe, struct uref *uref,
                                 struct upump **upump_p);

static inline int upipe_tcpsink_throw_connected(struct upipe *upipe)
{
    return upipe_throw(upipe, UPROBE_TCPSINK_CONNECTED,
                       UPIPE_TCPSINK_SIGNATURE);
}

/** @internal @This is the connection state of the socket */
enum upipe_tcpsink_state {
    /** socket is not initialized */
    STATE_NONE = 0,
    /** socket is connecting */
    STATE_CONNECTING,
    /** socket is connected */
    STATE_CONNECTED,
};

/** @internal @This converts @tt {enum upipe_tcpsink_state} to a string.
 *
 * @param state a tcp socket state
 * @return a string or NULL
 */
static inline const char *
upipe_tcpsink_state_str(enum upipe_tcpsink_state state)
{
    switch (state) {
    case STATE_NONE: return "none";
    case STATE_CONNECTING: return "connecting";
    case STATE_CONNECTED: return "connected";
    }
    return NULL;
}

/** @internal @This stores the private context of a tcp socket sink pipe. */
struct upipe_tcpsink {
    /** public upipe structure */
    struct upipe upipe;
    /** refcount structure */
    struct urefcount urefcount;
    /** upump manager */
    struct upump_mgr *upump_mgr;
    /** upump for tcp socket */
    struct upump *upump;
    /** uclock */
    struct uclock *uclock;
    /** urequest for uclock */
    struct urequest uclock_request;
    /** list of input urefs */
    struct uchain urefs;
    /** nb urefs in storage */
    unsigned int nb_urefs;
    /** max urefs in storage */
    unsigned int max_urefs;
    /** list of blockers */
    struct uchain blockers;
    /** tcp socket */
    int fd;
    /** tcp uri */
    char *uri;
    /** socket state */
    enum upipe_tcpsink_state state;
};

UPIPE_HELPER_UPIPE(upipe_tcpsink, upipe, UPIPE_TCPSINK_SIGNATURE);
UPIPE_HELPER_UREFCOUNT(upipe_tcpsink, urefcount, upipe_tcpsink_free);
UPIPE_HELPER_VOID(upipe_tcpsink);
UPIPE_HELPER_UPUMP_MGR(upipe_tcpsink, upump_mgr);
UPIPE_HELPER_UPUMP(upipe_tcpsink, upump, upump_mgr);
UPIPE_HELPER_UCLOCK(upipe_tcpsink, uclock, uclock_request,
                    upipe_tcpsink_check,
                    upipe_throw_provide_request, NULL);
UPIPE_HELPER_INPUT(upipe_tcpsink, urefs, nb_urefs, max_urefs, blockers,
                   upipe_tcpsink_output);

/** @internal @This allocates a tcp socket sink pipe.
 *
 * @param mgr pointer to upipe manager
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_tcpsink_alloc(struct upipe_mgr *mgr,
                                        struct uprobe *uprobe,
                                        uint32_t signature,
                                        va_list args)
{
    struct upipe *upipe =
        upipe_tcpsink_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(upipe == NULL))
        return NULL;

    upipe_tcpsink_init_urefcount(upipe);
    upipe_tcpsink_init_upump_mgr(upipe);
    upipe_tcpsink_init_upump(upipe);
    upipe_tcpsink_init_uclock(upipe);
    upipe_tcpsink_init_input(upipe);

    struct upipe_tcpsink *upipe_tcpsink = upipe_tcpsink_from_upipe(upipe);
    upipe_tcpsink->fd = -1;
    upipe_tcpsink->uri = NULL;
    upipe_tcpsink->state = STATE_NONE;

    upipe_throw_ready(upipe);

    return upipe;
}

/** @internal @This frees a tcp socket sink pipe.
 *
 * @param upipe public description structure of the pipe
 */
static void upipe_tcpsink_free(struct upipe *upipe)
{
    struct upipe_tcpsink *upipe_tcpsink = upipe_tcpsink_from_upipe(upipe);

    upipe_throw_dead(upipe);

    upipe_tcpsink_close(upipe);
    free(upipe_tcpsink->uri);
    upipe_tcpsink_clean_input(upipe);
    upipe_tcpsink_clean_uclock(upipe);
    upipe_tcpsink_clean_upump(upipe);
    upipe_tcpsink_clean_upump_mgr(upipe);
    upipe_tcpsink_clean_urefcount(upipe);
    upipe_tcpsink_free_void(upipe);
}

/** @internal @This sets the socket state.
 *
 * @param upipe description structure of the pipe
 * @param state the socket state to set
 * @return an error code
 */
static void upipe_tcpsink_set_state(struct upipe *upipe,
                                   enum upipe_tcpsink_state state)
{
    struct upipe_tcpsink *upipe_tcpsink = upipe_tcpsink_from_upipe(upipe);

    if (state == upipe_tcpsink->state)
        return;

    upipe_dbg_va(upipe, "%s -> %s",
                 upipe_tcpsink_state_str(upipe_tcpsink->state),
                 upipe_tcpsink_state_str(state));
    upipe_tcpsink->state = state;

    if (state == STATE_CONNECTED)
        upipe_tcpsink_throw_connected(upipe);
}

/** @internal @This reads data from the sink and outputs it.
 *
 * @param upump description structure of the read watcher
 */
static void upipe_tcpsink_worker(struct upump *upump)
{
    struct upipe *upipe = upump_get_opaque(upump, struct upipe *);

    if (upipe_tcpsink_output_input(upipe)) {
        upump_stop(upump);
        upipe_tcpsink_unblock_input(upipe);
        /* All packets have been output, release again the pipe that has been
         * used in @ref upipe_tcpsink_input. */
        upipe_release(upipe);
    }
}

/** @internal @This tries to output data if the socket is ready.
 *
 * @param upipe description structure of the pipe
 * @param uref uref carrying the data
 * @param upump_p reference to pump that generated the buffer
 * @return true if the uref was processed
 */
static bool upipe_tcpsink_output(struct upipe *upipe,
                                 struct uref *uref,
                                 struct upump **upump_p)
{
    struct upipe_tcpsink *upipe_tcpsink = upipe_tcpsink_from_upipe(upipe);

    if (upipe_tcpsink->state == STATE_CONNECTING) {
        int so_error;
        socklen_t socklen = sizeof (so_error);
        if (unlikely(getsockopt(upipe_tcpsink->fd, SOL_SOCKET, SO_ERROR,
                                &so_error, &socklen))) {
            upipe_warn(upipe, "getsockopt: %m");
            uref_free(uref);
            return true;
        }
        if (unlikely(so_error)) {
            upipe_warn(upipe, "connect failed");
            uref_free(uref);
            return true;
        }
        upipe_dbg(upipe, "connected");
        upipe_tcpsink_set_state(upipe, STATE_CONNECTED);
    }

    size_t block_size;
    if (unlikely(!ubase_check(uref_block_size(uref, &block_size)))) {
        upipe_warn(upipe, "cannot read ubuf size");
        uref_free(uref);
        return true;
    }

    int iovec_count = uref_block_iovec_count(uref, 0, -1);
    if (unlikely(iovec_count == -1)) {
        upipe_warn(upipe, "cannot read ubuf buffer");
        uref_free(uref);
        return true;;
    }
    if (unlikely(iovec_count == 0)) {
        uref_free(uref);
        return true;
    }

    struct iovec iovecs_s[iovec_count];
    struct iovec *iovecs = iovecs_s;
    if (unlikely(!ubase_check(uref_block_iovec_read(uref, 0, -1, iovecs)))) {
        upipe_warn(upipe, "cannot read ubuf buffer");
        uref_free(uref);
        return true;
    }

    ssize_t ret = writev(upipe_tcpsink->fd, iovecs_s, iovec_count);
    uref_block_iovec_unmap(uref, 0, -1, iovecs);

    if (unlikely(ret == -1)) {
        switch (errno) {
        case EINTR:
            return upipe_tcpsink_output(upipe, uref, upump_p);
        case EAGAIN:
#if EAGAIN != EWOULDBLOCK
        case EWOULDBLOCK:
#endif
            if (upipe_tcpsink->upump)
                upump_start(upipe_tcpsink->upump);
            return false;

        default:
            break;
        }
    }

    uref_free(uref);
    return true;
}

/** @internal @This is called when there is input data.
 *
 * @param upipe description structure of the pipe
 * @param uref uref carrying the data
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_tcpsink_input(struct upipe *upipe,
                                struct uref *uref,
                                struct upump **upump_p)
{
    if (!upipe_tcpsink_check_input(upipe)) {
        upipe_tcpsink_hold_input(upipe, uref);
        upipe_tcpsink_block_input(upipe, upump_p);
    } else if (!upipe_tcpsink_output(upipe, uref, upump_p)) {
        upipe_tcpsink_hold_input(upipe, uref);
        upipe_tcpsink_block_input(upipe, upump_p);
        /* Increment upipe refcount to avoid disappearing before all packets
         * have been sent. */
        upipe_use(upipe);
    }
}

/** @internal @This checks if the pump may be allocated.
 *
 * @param upipe description structure of the pipe
 * @param flow_format amended flow format
 * @return an error code
 */
static int upipe_tcpsink_check(struct upipe *upipe, struct uref *flow_format)
{
    struct upipe_tcpsink *upipe_tcpsink = upipe_tcpsink_from_upipe(upipe);

    if (flow_format != NULL)
        uref_free(flow_format);

    upipe_tcpsink_check_upump_mgr(upipe);
    if (unlikely(upipe_tcpsink->upump_mgr == NULL))
        return UBASE_ERR_NONE;

    if (upipe_tcpsink->uclock == NULL &&
        urequest_get_opaque(&upipe_tcpsink->uclock_request, struct upipe *)
            != NULL)
        return UBASE_ERR_NONE;

    if (upipe_tcpsink->fd != -1 && upipe_tcpsink->upump == NULL) {
        struct upump *upump = upump_alloc_fd_write(upipe_tcpsink->upump_mgr,
                                                   upipe_tcpsink_worker, upipe,
                                                   upipe_tcpsink->fd);
        if (unlikely(upump == NULL)) {
            upipe_throw_fatal(upipe, UBASE_ERR_UPUMP);
            return UBASE_ERR_UPUMP;
        }
        upipe_tcpsink_set_upump(upipe, upump);
    }
    return UBASE_ERR_NONE;
}

/** @internal @This sets the flow format.
 *
 * @param upipe public description structure of the pipe
 * @param flow_def flow definition packet
 * @return an error code
 */
static int upipe_tcpsink_set_flow_def(struct upipe *upipe,
                                     struct uref *flow_def)
{
    UBASE_RETURN(uref_flow_match_def(flow_def, "block."));
    return UBASE_ERR_NONE;
}

/** @internal @This closes the tcp socket.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_tcpsink_close(struct upipe *upipe)
{
    struct upipe_tcpsink *upipe_tcpsink = upipe_tcpsink_from_upipe(upipe);

    upipe_tcpsink_set_upump(upipe, NULL);
    if (likely(upipe_tcpsink->fd >= 0)) {
        shutdown(upipe_tcpsink->fd, SHUT_WR);
        ubase_clean_fd(&upipe_tcpsink->fd);
    }
    ubase_clean_str(&upipe_tcpsink->uri);
    upipe_tcpsink_set_state(upipe, STATE_NONE);
}

/** @internal @This opens a tcp socket.
 *
 * @param upipe description structure of the pipe
 * @param uri the tcp socket uri
 * @return an error code
 */
static int upipe_tcpsink_open(struct upipe *upipe, const char *uri)
{
    struct upipe_tcpsink *upipe_tcpsink = upipe_tcpsink_from_upipe(upipe);
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
    enum upipe_tcpsink_state state = STATE_NONE;
    for (struct addrinfo *res = info; res; res = res->ai_next) {
        fd = socket(res->ai_family,
                    res->ai_socktype | SOCK_NONBLOCK,
                    res->ai_protocol);
        if (unlikely(fd < 0))
            continue;

        ret = connect(fd, res->ai_addr, res->ai_addrlen);
        if (unlikely(ret != 0) && unlikely(errno != EINPROGRESS)) {
            ubase_clean_fd(&fd);
            continue;
        }
        state = ret ? STATE_CONNECTING : STATE_CONNECTED;
    }
    freeaddrinfo(info);
    if (fd < 0) {
        upipe_err(upipe, "could not connect to any ressource");
        return UBASE_ERR_EXTERNAL;
    }
    switch (state) {
    case STATE_CONNECTED:
        upipe_dbg(upipe, "connected");
        break;
    case STATE_CONNECTING:
        upipe_dbg(upipe, "connecting...");
        break;
    default:
        upipe_err(upipe, "invalid state");
        close(fd);
        return UBASE_ERR_INVALID;
    }
    upipe_tcpsink->fd = fd;
    upipe_tcpsink->uri = strdup(uri);
    if (unlikely(upipe_tcpsink->uri == NULL)) {
        upipe_tcpsink_close(upipe);
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return UBASE_ERR_ALLOC;
    }
    upipe_tcpsink_set_state(upipe, state);
    return UBASE_ERR_NONE;
}

/** @internal @This sets the uri of the tcp socket.
 *
 * @param upipe description structure of the pipe
 * @param uri relative or absolute uri of the tcp socket
 * @return an error code
 */
static int upipe_tcpsink_set_uri(struct upipe *upipe, const char *uri)
{
    upipe_tcpsink_close(upipe);
    if (unlikely(uri == NULL))
        return UBASE_ERR_NONE;

    return upipe_tcpsink_open(upipe, uri);
}

/** @internal @This gets the uri of the tcp socket.
 *
 * @param upipe description structure of the pipe
 * @param uri_p filled in with the uri of the tcp socket
 * @return an error code
 */
static int upipe_tcpsink_get_uri(struct upipe *upipe, const char **uri_p)
{
    struct upipe_tcpsink *upipe_tcpsink = upipe_tcpsink_from_upipe(upipe);
    if (uri_p)
        *uri_p = upipe_tcpsink->uri;
    return UBASE_ERR_NONE;
}

/** @internal @This gets the opened fd.
 *
 * @param upipe description structure of the pipe
 * @param fd_p pointer filled with the socket
 * @return an error code
 */
static int _upipe_tcpsink_get_fd(struct upipe *upipe, int *fd_p)
{
    struct upipe_tcpsink *upipe_tcpsink = upipe_tcpsink_from_upipe(upipe);
    if (fd_p) {
        if (upipe_tcpsink->state == STATE_CONNECTED)
            *fd_p = upipe_tcpsink->fd;
        else
            *fd_p = -1;
    }
    return UBASE_ERR_NONE;
}

/** @internal @This sets the socket.
 *
 * @param upipe description structure of the pipe
 * @param fd the socket to use
 * @return an error code
 */
static int _upipe_tcpsink_set_fd(struct upipe *upipe, int fd)
{
    struct upipe_tcpsink *upipe_tcpsink = upipe_tcpsink_from_upipe(upipe);

    upipe_tcpsink_close(upipe);
    if (unlikely(fd < 0))
        return UBASE_ERR_NONE;

    upipe_tcpsink->fd = dup(fd);
    if (unlikely(upipe_tcpsink->fd < 0))
        return UBASE_ERR_EXTERNAL;
    upipe_tcpsink_set_state(upipe, STATE_CONNECTED);
    return UBASE_ERR_NONE;
}

/** @internal @This processes control commands on a tcp socket sink pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error cod e
 */
static int _upipe_tcpsink_control(struct upipe *upipe,
                                 int command,
                                 va_list args)
{
    switch (command) {
    case UPIPE_ATTACH_UPUMP_MGR:
        upipe_tcpsink_set_upump(upipe, NULL);
        return upipe_tcpsink_attach_upump_mgr(upipe);
    case UPIPE_ATTACH_UCLOCK:
        upipe_tcpsink_set_upump(upipe, NULL);
        upipe_tcpsink_require_uclock(upipe);
        return UBASE_ERR_NONE;
    case UPIPE_REGISTER_REQUEST: {
        struct urequest *request = va_arg(args, struct urequest *);
        return upipe_throw_provide_request(upipe, request);
    }
    case UPIPE_UNREGISTER_REQUEST:
        return UBASE_ERR_NONE;
    case UPIPE_SET_FLOW_DEF: {
        struct uref *flow_def = va_arg(args, struct uref *);
        return upipe_tcpsink_set_flow_def(upipe, flow_def);
    }
    case UPIPE_GET_URI: {
        const char **uri_p = va_arg(args, const char **);
        return upipe_tcpsink_get_uri(upipe, uri_p);
    }
    case UPIPE_SET_URI: {
        const char *uri = va_arg(args, const char *);
        return upipe_tcpsink_set_uri(upipe, uri);
    }
    case UPIPE_TCPSINK_GET_FD: {
        UBASE_SIGNATURE_CHECK(args, UPIPE_TCPSINK_SIGNATURE);
        int *fd_p = va_arg(args, int *);
        return _upipe_tcpsink_get_fd(upipe, fd_p);
    }
    case UPIPE_TCPSINK_SET_FD: {
        UBASE_SIGNATURE_CHECK(args, UPIPE_TCPSINK_SIGNATURE);
        int fd = va_arg(args, int);
        return _upipe_tcpsink_set_fd(upipe, fd);
    }
    }
    return UBASE_ERR_UNHANDLED;
}

/** @internal @This processes control commands on a tcp socket sink pipe, and
 * checks the status of the pipe afterwards.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_tcpsink_control(struct upipe *upipe, int command, va_list args)
{
    UBASE_RETURN(_upipe_tcpsink_control(upipe, command, args));

    return upipe_tcpsink_check(upipe, NULL);
}

/** @internal @This is the static structure for tcp socket manager. */
static struct upipe_mgr upipe_tcpsink_mgr = {
    .refcount = NULL,
    .signature = UPIPE_TCPSINK_SIGNATURE,

    .upipe_alloc = upipe_tcpsink_alloc,
    .upipe_input = upipe_tcpsink_input,
    .upipe_control = upipe_tcpsink_control,
};

/** @This returns the management structure for tcp socket sink.
 *
 * @return pointer to the tcp socket sink manager
 */
struct upipe_mgr *upipe_tcpsink_mgr_alloc(void)
{
    return &upipe_tcpsink_mgr;
}

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
 * @short Upipe TCP source module
 */

#ifndef _UPIPE_MODULES_UPIPE_TCP_SOURCE_H_
/** @hidden */
# define _UPIPE_MODULES_UPIPE_TCP_SOURCE_H_
#ifdef __cplusplus
extern "C" {
#endif

# define UPIPE_TCPSRC_SIGNATURE UBASE_FOURCC('t','s','r','c')

/** @This extends @ref upipe_command with specific tcp source commands. */
enum upipe_tcpsrc_command {
    UPIPE_TCPSRC_SENTINEL = UPIPE_CONTROL_LOCAL,

    /** directly set the socket */
    UPIPE_TCPSRC_SET_FD,
    /** get the socket */
    UPIPE_TCPSRC_GET_FD,
};

/** @This sets the socket to use.
 *
 * @param upipe description structure of the pipe
 * @param fd the opened socket to use or -1
 * @return an error code
 */
static inline int upipe_tcpsrc_set_fd(struct upipe *upipe, int fd)
{
    return upipe_control(upipe, UPIPE_TCPSRC_SET_FD,
                         UPIPE_TCPSRC_SIGNATURE, fd);
}

/** @This gets the socket.
 *
 * @param upipe description structure of the pipe
 * @param fd_p pointer filled with the socket
 * @return an error code
 */
static inline int upipe_tcpsrc_get_fd(struct upipe *upipe, int *fd_p)
{
    return upipe_control(upipe, UPIPE_TCPSRC_GET_FD,
                         UPIPE_TCPSRC_SIGNATURE, fd_p);
}

/** @This extends @ref uprobe_event with specific tcp source events. */
enum upipe_tcpsrc_event {
    UPROBE_TCPSRC_SENTINEL = UPROBE_LOCAL,

    /** the connection is established */
    UPROBE_TCPSRC_ACCEPTED,
};

/** @This returns the management structure for tcp socket source.
 *
 * @return pointer to the tcp socket source manager
 */
struct upipe_mgr *upipe_tcpsrc_mgr_alloc(void);

#ifdef __cplusplus
}
#endif
#endif

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
 * @short Upipe TCP sink module
 */

#ifndef _UPIPE_MODULES_UPIPE_TCP_SINK_H_
/** @hidden */
# define _UPIPE_MODULES_UPIPE_TCP_SINK_H_
#ifdef __cplusplus
extern "C" {
#endif

# define UPIPE_TCPSINK_SIGNATURE UBASE_FOURCC('t','s','r','c')

/** @This extends @ref upipe_command with specific tcp sink commands. */
enum upipe_tcpsink_command {
    UPIPE_TCPSINK_SENTINEL = UPIPE_CONTROL_LOCAL,

    /** get the opened socket */
    UPIPE_TCPSINK_GET_FD,
    /** set the socket to use */
    UPIPE_TCPSINK_SET_FD,
};

/** @This gets the opened fd.
 *
 * @param upipe description structure of the pipe
 * @param fd_p pointer filled with the socket
 * @return an error code
 */
static inline int upipe_tcpsink_get_fd(struct upipe *upipe, int *fd_p)
{
    return upipe_control(upipe, UPIPE_TCPSINK_GET_FD,
                         UPIPE_TCPSINK_SIGNATURE, fd_p);
}

/** @This sets the socket to use.
 *
 * @param upipe description structure of the pipe
 * @param fd the socket to use
 * @return an error code
 */
static inline int upipe_tcpsink_set_fd(struct upipe *upipe, int fd)
{
    return upipe_control(upipe, UPIPE_TCPSINK_SET_FD,
                         UPIPE_TCPSINK_SIGNATURE, fd);
}

/** @This extends @ref uprobe_event with specific tcp sink events. */
enum upipe_tcpsink_event {
    UPROBE_TCPSINK_SENTINEL = UPROBE_LOCAL,

    /** the connection is established */
    UPROBE_TCPSINK_CONNECTED,
};

/** @This returns the management structure for tcp socket source.
 *
 * @return pointer to the tcp socket source manager
 */
struct upipe_mgr *upipe_tcpsink_mgr_alloc(void);

#ifdef __cplusplus
}
#endif
#endif

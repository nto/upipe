#include <upump-ev/upump_ev.h>
#include <upipe/umem.h>
#include <upipe/umem_alloc.h>
#include <upipe/udict.h>
#include <upipe/udict_inline.h>
#include <upipe/uref.h>
#include <upipe/uref_std.h>
#include <upipe/uprobe_prefix.h>
#include <upipe/uprobe_stdio_color.h>
#include <upipe/uprobe_uref_mgr.h>
#include <upipe/uprobe_upump_mgr.h>
#include <upipe/uprobe_ubuf_mem.h>
#include <upipe/upipe.h>
#include <upipe-modules/upipe_file_source.h>
#include <upipe-modules/upipe_file_sink.h>
#include <upipe-modules/upipe_tcp_source.h>
#include <upipe-modules/upipe_tcp_sink.h>

#include <ev.h>

#define UDICT_POOL_DEPTH 0
#define UREF_POOL_DEPTH 0
#define UBUF_POOL_DEPTH 0
#define UPUMP_POOL 0
#define UPUMP_BLOCKER_POOL 0

static struct upipe *upipe_fsrc = NULL;

static int catch_fsrc(struct uprobe *uprobe,
                      struct upipe *upipe,
                      int event,
                      va_list args)
{
    switch (event) {
    case UPROBE_SOURCE_END:
        upipe_release(upipe_fsrc);
        upipe_fsrc = NULL;
        return UBASE_ERR_NONE;
    }

    return uprobe_throw_next(uprobe, upipe, event, args);
}

int main(int argc, char *argv[])
{
    assert(argc > 2);
    const char *input = argv[1];
    const char *output = argv[2];

    struct ev_loop *loop = ev_default_loop(0);
    assert(loop != NULL);
    struct upump_mgr *upump_mgr = upump_ev_mgr_alloc(loop, UPUMP_POOL,
                                                     UPUMP_BLOCKER_POOL);
    assert(upump_mgr != NULL);

    struct umem_mgr *umem_mgr = umem_alloc_mgr_alloc();
    assert(umem_mgr != NULL);
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH,
                                                         umem_mgr, -1, -1);
    assert(udict_mgr != NULL);
    struct uref_mgr *uref_mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr,
                                                   0);
    /* main probe */
    struct uprobe *logger =
        uprobe_stdio_color_alloc(NULL, stdout, UPROBE_LOG_VERBOSE);
    assert(logger != NULL);
    logger = uprobe_uref_mgr_alloc(logger, uref_mgr);
    assert(logger != NULL);
    logger = uprobe_upump_mgr_alloc(logger, upump_mgr);
    assert(logger != NULL);
    logger = uprobe_ubuf_mem_alloc(logger, umem_mgr, UBUF_POOL_DEPTH,
                                   UBUF_POOL_DEPTH);
    assert(logger != NULL);

    /* tcp source */
    struct upipe_mgr *upipe_tcpsrc_mgr = upipe_tcpsrc_mgr_alloc();
    assert(upipe_tcpsrc_mgr != NULL);
    struct upipe *upipe_tcpsrc = upipe_void_alloc(
        upipe_tcpsrc_mgr,
        uprobe_pfx_alloc(uprobe_use(logger),
                         UPROBE_LOG_DEBUG, "tcpsrc"));
    upipe_mgr_release(upipe_tcpsrc_mgr);
    assert(upipe_tcpsrc != NULL);

    /* file sink */
    struct upipe_mgr *upipe_fsink_mgr = upipe_fsink_mgr_alloc();
    assert(upipe_fsink_mgr != NULL);
    struct upipe *upipe_fsink = upipe_void_alloc_output(
        upipe_tcpsrc, upipe_fsink_mgr,
        uprobe_pfx_alloc(uprobe_use(logger),
                         UPROBE_LOG_DEBUG, "fsink"));
    upipe_mgr_release(upipe_fsink_mgr);
    assert(upipe_fsink != NULL);
    ubase_assert(upipe_fsink_set_path(upipe_fsink, output, UPIPE_FSINK_CREATE));
    upipe_release(upipe_fsink);

    ubase_assert(upipe_set_uri(upipe_tcpsrc, "tcp://127.0.0.1:5004"));

    /* file source */
    struct uprobe uprobe_fsrc;
    uprobe_init(&uprobe_fsrc, catch_fsrc, uprobe_use(logger));
    struct upipe_mgr *upipe_fsrc_mgr = upipe_fsrc_mgr_alloc();
    assert(upipe_fsrc_mgr != NULL);
    upipe_fsrc = upipe_void_alloc(
        upipe_fsrc_mgr,
        uprobe_pfx_alloc(uprobe_use(&uprobe_fsrc),
                         UPROBE_LOG_DEBUG, "fsrc"));
    upipe_mgr_release(upipe_fsrc_mgr);
    assert(upipe_fsrc != NULL);

    /* tcp sink */
    struct upipe_mgr *upipe_tcpsink_mgr = upipe_tcpsink_mgr_alloc();
    assert(upipe_tcpsink_mgr != NULL);
    struct upipe *upipe_tcpsink = upipe_void_alloc_output(
        upipe_fsrc, upipe_tcpsink_mgr,
        uprobe_pfx_alloc(uprobe_use(logger),
                         UPROBE_LOG_DEBUG, "tcpsink"));
    upipe_mgr_release(upipe_tcpsink_mgr);
    assert(upipe_tcpsink != NULL);
    ubase_assert(upipe_set_uri(upipe_tcpsink, "tcp://127.0.0.1:5004"));
    upipe_release(upipe_tcpsink);
    ubase_assert(upipe_set_uri(upipe_fsrc, input));

    /* run main loop */
    ev_loop(loop, 0);

    /* release */
    upipe_release(upipe_fsrc);
    upipe_release(upipe_tcpsrc);
    uprobe_clean(&uprobe_fsrc);
    upump_mgr_release(upump_mgr);
    uref_mgr_release(uref_mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);
    uprobe_release(logger);
    ev_default_destroy();
    return 0;
}

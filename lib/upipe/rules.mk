lib-targets = libupipe

libupipe-desc = core library

libupipe-includes = \
    uatomic.h \
    ubase.h \
    ubits.h \
    ubuf.h \
    ubuf_block.h \
    ubuf_block_common.h \
    ubuf_block_mem.h \
    ubuf_block_stream.h \
    ubuf_mem.h \
    ubuf_mem_common.h \
    ubuf_pic.h \
    ubuf_pic_common.h \
    ubuf_pic_mem.h \
    ubuf_sound.h \
    ubuf_sound_common.h \
    ubuf_sound_mem.h \
    uclock.h \
    uclock_ptp.h \
    uclock_std.h \
    ucookie.h \
    udeal.h \
    udict.h \
    udict_dump.h \
    udict_inline.h \
    ueventfd.h \
    ufifo.h \
    ulifo.h \
    ulist.h \
    ulist_helper.h \
    ulog.h \
    umem.h \
    umem_alloc.h \
    umem_pool.h \
    umutex.h \
    upipe.h \
    upipe_dump.h \
    upipe_helper_bin_input.h \
    upipe_helper_bin_output.h \
    upipe_helper_dvb_string.h \
    upipe_helper_flow.h \
    upipe_helper_flow_def.h \
    upipe_helper_flow_def_check.h \
    upipe_helper_flow_format.h \
    upipe_helper_iconv.h \
    upipe_helper_inner.h \
    upipe_helper_input.h \
    upipe_helper_output.h \
    upipe_helper_output_size.h \
    upipe_helper_subpipe.h \
    upipe_helper_sync.h \
    upipe_helper_ubuf_mgr.h \
    upipe_helper_uclock.h \
    upipe_helper_upipe.h \
    upipe_helper_uprobe.h \
    upipe_helper_upump.h \
    upipe_helper_upump_mgr.h \
    upipe_helper_uref_mgr.h \
    upipe_helper_uref_stream.h \
    upipe_helper_urefcount.h \
    upipe_helper_urefcount_real.h \
    upipe_helper_void.h \
    upool.h \
    uprobe.h \
    uprobe_dejitter.h \
    uprobe_helper.h \
    uprobe_helper_alloc.h \
    uprobe_helper_uprobe.h \
    uprobe_helper_urefcount.h \
    uprobe_loglevel.h \
    uprobe_prefix.h \
    uprobe_select_flows.h \
    uprobe_source_mgr.h \
    uprobe_stdio.h \
    uprobe_syslog.h \
    uprobe_transfer.h \
    uprobe_ubuf_mem.h \
    uprobe_ubuf_mem_pool.h \
    uprobe_uclock.h \
    uprobe_upump_mgr.h \
    uprobe_uref_mgr.h \
    upump.h \
    upump_blocker.h \
    upump_common.h \
    uqueue.h \
    uref.h \
    uref_attr.h \
    uref_block.h \
    uref_block_flow.h \
    uref_clock.h \
    uref_dump.h \
    uref_event.h \
    uref_flow.h \
    uref_http.h \
    uref_m3u.h \
    uref_m3u_flow.h \
    uref_m3u_master.h \
    uref_m3u_playlist.h \
    uref_m3u_playlist_flow.h \
    uref_pic.h \
    uref_pic_flow.h \
    uref_pic_flow_formats.h \
    uref_program_flow.h \
    uref_sound.h \
    uref_sound_flow.h \
    uref_sound_flow_formats.h \
    uref_std.h \
    uref_uri.h \
    uref_void.h \
    uref_void_flow.h \
    urefcount.h \
    urefcount_helper.h \
    urequest.h \
    uring.h \
    ustring.h \
    uuri.h

libupipe-src = \
    ubuf_block_mem.c \
    ubuf_mem.c \
    ubuf_mem_common.c \
    ubuf_pic.c \
    ubuf_pic_common.c \
    ubuf_pic_mem.c \
    ubuf_sound_common.c \
    ubuf_sound_mem.c \
    uclock_ptp.c \
    uclock_std.c \
    ucookie.c \
    udict_inline.c \
    umem_alloc.c \
    umem_pool.c \
    upipe_dump.c \
    uprobe.c \
    uprobe_dejitter.c \
    uprobe_loglevel.c \
    uprobe_prefix.c \
    uprobe_select_flows.c \
    uprobe_source_mgr.c \
    uprobe_stdio.c \
    uprobe_syslog.c \
    uprobe_transfer.c \
    uprobe_ubuf_mem.c \
    uprobe_ubuf_mem_pool.c \
    uprobe_uclock.c \
    uprobe_upump_mgr.c \
    uprobe_uref_mgr.c \
    upump_common.c \
    uref_std.c \
    uref_uri.c \
    ustring.c \
    uuri.c

libupipe-ldlibs = -lm

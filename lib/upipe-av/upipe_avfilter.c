/*
 * Copyright (C) 2018 OpenHeadend S.A.R.L.
 * Copyright (C) 2019-2025 EasyTools
 *
 * Authors: Clément Vasseur
 *          Arnaud de Turckheim
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
 * @short Upipe avfilter module
 */

#define _GNU_SOURCE

#include "upipe/upipe.h"
#include "upipe/uclock.h"
#include "upipe/uref_pic.h"
#include "upipe/uref_pic_flow.h"
#include "upipe/uref_sound.h"
#include "upipe/uref_sound_flow.h"
#include "upipe/uref_clock.h"
#include "upipe/upipe_helper_upipe.h"
#include "upipe/upipe_helper_void.h"
#include "upipe/upipe_helper_flow.h"
#include "upipe/upipe_helper_flow_def.h"
#include "upipe/upipe_helper_urefcount.h"
#include "upipe/upipe_helper_output.h"
#include "upipe/upipe_helper_subpipe.h"
#include "upipe/upipe_helper_uclock.h"
#include "upipe/upipe_helper_upump_mgr.h"
#include "upipe/upipe_helper_upump.h"
#include "upipe/upipe_helper_sync.h"
#include "upipe-av/upipe_avfilter.h"
#include "upipe-av/upipe_av_pixfmt.h"
#include "upipe-av/upipe_av_samplefmt.h"
#include "upipe-av/uref_avfilter_flow.h"
#include "upipe-av/ubuf_av.h"

#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/hwcontext.h>
#include "upipe_av_internal.h"

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 30, 100)
# define pkt_duration duration
#endif

/** @This is the sub pipe private structure of the avfilter pipe. */
struct upipe_avfilt_sub {
    /** refcount management structure */
    struct urefcount urefcount;
    /** public upipe structure */
    struct upipe upipe;
    /** chain in the super pipe list */
    struct uchain uchain;
    /** allocation flow definition */
    struct uref *flow_def_alloc;
    /** output flow def */
    struct uref *flow_def;
    /** input flow definition */
    struct uref *flow_def_input;
    /** flow definition attributes */
    struct uref *flow_def_attr;
    /** output pipe */
    struct upipe *output;
    /** output internal state */
    enum upipe_helper_output_state output_state;
    /** registered requests on output */
    struct uchain requests;
    /** AVFrame buffer manager */
    struct ubuf_mgr *ubuf_mgr;
    /** uclock request */
    struct urequest uclock_request;
    /** uclock */
    struct uclock *uclock;
    /** upump manager */
    struct upump_mgr *upump_mgr;
    /** upump */
    struct upump *upump;
    /** sub pipe name */
    const char *name;
    /** avfilter buffer source */
    AVFilterContext *buffer_ctx;
    /** system clock offset */
    uint64_t pts_sys_offset;
    /** prog clock offset */
    uint64_t pts_prog_offset;
    /** first pts_prog */
    uint64_t first_pts_prog;
    /** last pts */
    uint64_t last_pts_prog;
    /** last duration */
    uint64_t last_duration;
    /** list of retained uref */
    struct uchain urefs;
    /** warn once on unconfigured filter graph */
    bool warn_not_configured;
    /** latency */
    uint64_t latency;
};

/** @hidden */
static void upipe_avfilt_reset(struct upipe *upipe);
/** @hidden */
static inline void upipe_avfilt_sub_flush_cb(struct upump *upump);

UPIPE_HELPER_UPIPE(upipe_avfilt_sub, upipe, UPIPE_AVFILT_SUB_SIGNATURE)
UPIPE_HELPER_FLOW(upipe_avfilt_sub, NULL)
UPIPE_HELPER_FLOW_DEF(upipe_avfilt_sub, flow_def_input, flow_def_attr);
UPIPE_HELPER_UREFCOUNT(upipe_avfilt_sub, urefcount, upipe_avfilt_sub_free)
UPIPE_HELPER_OUTPUT(upipe_avfilt_sub, output, flow_def, output_state, requests)
UPIPE_HELPER_UCLOCK(upipe_avfilt_sub, uclock, uclock_request,
                    NULL,
                    upipe_avfilt_sub_register_output_request,
                    upipe_avfilt_sub_unregister_output_request)
UPIPE_HELPER_UPUMP_MGR(upipe_avfilt_sub, upump_mgr);
UPIPE_HELPER_UPUMP(upipe_avfilt_sub, upump, upump_mgr);

/** upipe_avfilt structure */
struct upipe_avfilt {
    /** refcount management structure */
    struct urefcount urefcount;

    /** output pipe */
    struct upipe *output;
    /** output flow def */
    struct uref *flow_def;
    /** output internal state */
    enum upipe_helper_output_state output_state;
    /** registered requests on output */
    struct uchain requests;

    /** input flow */
    struct uref *flow_def_input;
    /** attributes added by the pipe */
    struct uref *flow_def_attr;

    /** sub pipe manager */
    struct upipe_mgr sub_mgr;
    /** sub pipe list */
    struct uchain subs;

    /** dictionary for options */
    AVDictionary *options;

    /** filter graph description */
    char *filters_desc;
    /** avfilter filter graph */
    AVFilterGraph *filter_graph;
    /** filter graph is configured? */
    bool configured;

    /** reference to hardware device context for filters */
    AVBufferRef *hw_device_ctx;

    /** AVFrame ubuf manager */
    struct ubuf_mgr *ubuf_mgr;

    /** avfilter buffer source */
    AVFilterContext *buffer_ctx;
    /** avfilter buffer sink */
    AVFilterContext *buffersink_ctx;
    /** uref from input */
    struct uref *uref;

    /** public upipe structure */
    struct upipe upipe;
};

/** @hidden */
static int upipe_avfilt_init_filters(struct upipe *upipe);
/** @hidden */
static void upipe_avfilt_clean_filters(struct upipe *upipe);
/** @hidden */
static void upipe_avfilt_update_outputs(struct upipe *upipe);

UPIPE_HELPER_UPIPE(upipe_avfilt, upipe, UPIPE_AVFILT_SIGNATURE)
UPIPE_HELPER_VOID(upipe_avfilt)
UPIPE_HELPER_UREFCOUNT(upipe_avfilt, urefcount, upipe_avfilt_free)
UPIPE_HELPER_OUTPUT(upipe_avfilt, output, flow_def, output_state, requests)
UPIPE_HELPER_FLOW_DEF(upipe_avfilt, flow_def_input, flow_def_attr)
UPIPE_HELPER_SUBPIPE(upipe_avfilt, upipe_avfilt_sub, sub,
                     sub_mgr, subs, uchain);
UPIPE_HELPER_SYNC(upipe_avfilt, configured);

/** @internal @This is the avbuffer free callback.
 *
 * @param opaque pointer to the uref
 * @param data avbuffer data
 */
static void buffer_free_pic_cb(void *opaque, uint8_t *data)
{
    struct uref *uref = opaque;

    uint64_t buffers;
    if (unlikely(!ubase_check(uref_attr_get_priv(uref, &buffers))))
        return;
    if (--buffers) {
        uref_attr_set_priv(uref, buffers);
        return;
    }

    const char *chroma;
    uref_pic_foreach_plane(uref, chroma) {
        uref_pic_plane_unmap(uref, chroma, 0, 0, -1, -1);
    }

    uref_free(uref);
}

/** @internal @This is the avbuffer free callback for uref sound.
 *
 * @param opaque pointer to the uref
 * @param data avbuffer data
 */
static void buffer_free_sound_cb(void *opaque, uint8_t *data)
{
    struct uref *uref = opaque;

    uint64_t buffers;
    if (unlikely(!ubase_check(uref_attr_get_priv(uref, &buffers))))
        return;
    if (--buffers) {
        uref_attr_set_priv(uref, buffers);
        return;
    }

    const char *channel;
    uref_sound_foreach_plane(uref, channel) {
        uref_sound_plane_unmap(uref, channel, 0, -1);
    }

    uref_free(uref);
}

/** @internal @This makes an urational from an AVRational.
 *
 * @param v AVRational to convert
 * @return an urational
 */
static inline struct urational urational(AVRational v)
{
    return (struct urational){ .num = v.num, .den = v.den };
}

/** @internal @This builds the video flow definition packet.
 *
 * @param flow_def output flow def
 * @param buffer_ctx buffersink context
 * @param frame first frame
 * @return an error code
 */
static int build_video_flow_def(struct uref *flow_def,
                                AVFilterContext *buffer_ctx,
                                const AVFrame *frame)
{
    enum AVPixelFormat pix_fmt = av_buffersink_get_format(buffer_ctx);
    int width = av_buffersink_get_w(buffer_ctx);
    int height = av_buffersink_get_h(buffer_ctx);
    AVRational fps = av_buffersink_get_frame_rate(buffer_ctx);
    if (!fps.den)
        fps = av_inv_q(av_buffersink_get_time_base(buffer_ctx));
    AVRational sar = av_buffersink_get_sample_aspect_ratio(buffer_ctx);
    enum AVColorRange color_range = frame->color_range;
    int matrix_coefficients = frame->colorspace;
#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(9, 16, 100)
    color_range = av_buffersink_get_color_range(buffer_ctx);
    matrix_coefficients = av_buffersink_get_colorspace(buffer_ctx);
#endif

    if (width <= 0 || height <= 0)
        return UBASE_ERR_INVALID;

    AVBufferRef *hw_frames_ctx = av_buffersink_get_hw_frames_ctx(buffer_ctx);
    if (hw_frames_ctx != NULL) {
        AVHWFramesContext *hw_frames =
            (AVHWFramesContext *) hw_frames_ctx->data;
        UBASE_RETURN(uref_pic_flow_set_surface_type_va(flow_def,
            "av.%s", av_get_pix_fmt_name(pix_fmt)))
        pix_fmt = hw_frames->sw_format;
    }

    UBASE_RETURN(upipe_av_pixfmt_to_flow_def(pix_fmt, flow_def))
    UBASE_RETURN(uref_pic_flow_set_hsize(flow_def, width))
    UBASE_RETURN(uref_pic_flow_set_vsize(flow_def, height))
    UBASE_RETURN(uref_pic_flow_set_fps(flow_def, urational(fps)))
    UBASE_RETURN(uref_pic_flow_set_sar(flow_def, urational(sar)))

    if (!frame->interlaced_frame)
        UBASE_RETURN(uref_pic_set_progressive(flow_def))
    if (color_range == AVCOL_RANGE_JPEG)
        UBASE_RETURN(uref_pic_flow_set_full_range(flow_def))

    UBASE_RETURN(uref_pic_flow_set_colour_primaries_val(
            flow_def, frame->color_primaries))
    UBASE_RETURN(uref_pic_flow_set_transfer_characteristics_val(
            flow_def, frame->color_trc))
    UBASE_RETURN(uref_pic_flow_set_matrix_coefficients_val(
            flow_def, matrix_coefficients))

    AVFrameSideData *sd = av_frame_get_side_data(
        frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (sd) {
        AVContentLightMetadata *clm = (AVContentLightMetadata *)sd->data;
        UBASE_RETURN(uref_pic_flow_set_max_cll(flow_def, clm->MaxCLL))
        UBASE_RETURN(uref_pic_flow_set_max_fall(flow_def, clm->MaxFALL))
    }
    sd = av_frame_get_side_data(
        frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (sd) {
        AVMasteringDisplayMetadata *mdcv =
            (AVMasteringDisplayMetadata *)sd->data;
        AVRational chroma = { 1, 50000 };
        AVRational luma = { 1, 10000 };
        UBASE_RETURN(uref_pic_flow_set_mastering_display(flow_def,
            &(struct uref_pic_mastering_display){
                .red_x = av_rescale_q(1, mdcv->display_primaries[0][0], chroma),
                .red_y = av_rescale_q(1, mdcv->display_primaries[0][1], chroma),
                .green_x = av_rescale_q(1, mdcv->display_primaries[1][0], chroma),
                .green_y = av_rescale_q(1, mdcv->display_primaries[1][1], chroma),
                .blue_x = av_rescale_q(1, mdcv->display_primaries[2][0], chroma),
                .blue_y = av_rescale_q(1, mdcv->display_primaries[2][1], chroma),
                .white_x = av_rescale_q(1, mdcv->white_point[0], chroma),
                .white_y = av_rescale_q(1, mdcv->white_point[1], chroma),
                .min_luminance = av_rescale_q(1, mdcv->min_luminance, luma),
                .max_luminance = av_rescale_q(1, mdcv->max_luminance, luma),
            }))
    }

    return UBASE_ERR_NONE;
}

/** @internal @This builds the audio flow definition packet.
 *
 * @param flow_def output flow def
 * @param buffer_ctx buffersink context
 * @param frame first frame
 * @return an error code
 */
static int build_audio_flow_def(struct uref *flow_def,
                                AVFilterContext *buffer_ctx,
                                const AVFrame *frame)
{
    enum AVSampleFormat sample_fmt = av_buffersink_get_format(buffer_ctx);
    int channels = av_buffersink_get_channels(buffer_ctx);
    int sample_rate = av_buffersink_get_sample_rate(buffer_ctx);
    uref_sound_flow_set_rate(flow_def, sample_rate);
    return upipe_av_samplefmt_to_flow_def(flow_def, sample_fmt, channels);
}

/** @internal @This builds the flow definition packet.
 *
 * @param flow_def output flow def
 * @param buffer_ctx buffersink context
 * @param frame first frame
 * @return an error code
 */
static int build_flow_def(struct uref *flow_def, AVFilterContext *ctx,
                          const AVFrame *frame)
{
    if (!flow_def || !ctx || !frame)
        return UBASE_ERR_INVALID;

    switch (av_buffersink_get_type(ctx)) {
        case AVMEDIA_TYPE_VIDEO:
            return build_video_flow_def(flow_def, ctx, frame);

        case AVMEDIA_TYPE_AUDIO:
            return build_audio_flow_def(flow_def, ctx, frame);

        default:
            break;
    }

    return UBASE_ERR_UNHANDLED;
}

/** @internal @This builds the output flow definition packet.
 *
 * @param upipe description structure of the pipe
 * @param frame first output frame
 * @return the output flow definition or NULL in case of error
 */
static struct uref *upipe_avfilt_sub_build_flow_def(struct upipe *upipe,
                                                    const AVFrame *frame)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);
    AVFilterContext *ctx = upipe_avfilt_sub->buffer_ctx;

    struct uref *flow_def =
        uref_sibling_alloc_control(upipe_avfilt_sub->flow_def_alloc);
    if (unlikely(flow_def == NULL))
        return NULL;

    int ret = build_flow_def(flow_def, ctx, frame);
    if (unlikely(!ubase_check(ret))) {
        uref_free(flow_def);
        upipe_err_va(upipe, "unknown buffersink type");
        return NULL;
    }
    uref_clock_set_latency(flow_def, upipe_avfilt_sub->latency);
    return flow_def;
}

/** @internal @This waits for the next uref to output.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_avfilt_sub_wait(struct upipe *upipe, uint64_t timeout)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);
    if (!upipe_avfilt_sub->upump_mgr)
        return;
    if (upipe_avfilt_sub->upump)
        return;
    return upipe_avfilt_sub_wait_upump(upipe, timeout, upipe_avfilt_sub_flush_cb);
}

/** @internal @This outputs a frame.
 *
 * @param upipe description structure of the pipe
 * @param frame AVFrame to output
 * @return an uref or NULL
 */
static struct uref *
upipe_avfilt_sub_frame_to_uref(struct upipe *upipe, AVFrame *frame)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);

    struct uref *flow_def = upipe_avfilt_sub_build_flow_def(upipe, frame);
    if (unlikely(flow_def == NULL)) {
        upipe_throw_error(upipe, UBASE_ERR_ALLOC);
        return NULL;
    }
    upipe_avfilt_sub_store_flow_def(upipe, flow_def);

    if (unlikely(!upipe_avfilt_sub->ubuf_mgr)) {
        upipe_warn(upipe, "no ubuf manager for now");
        return NULL;
    }

    enum AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;

    struct ubuf *ubuf = NULL;
    if (ubase_check(uref_flow_match_def(
                upipe_avfilt_sub->flow_def, UREF_PIC_FLOW_DEF))) {
        ubuf = ubuf_pic_av_alloc(upipe_avfilt_sub->ubuf_mgr, frame);
        media_type = AVMEDIA_TYPE_VIDEO;
    }
    else if (ubase_check(uref_flow_match_def(
                upipe_avfilt_sub->flow_def, UREF_SOUND_FLOW_DEF))) {
        ubuf = ubuf_sound_av_alloc(upipe_avfilt_sub->ubuf_mgr, frame);
        media_type = AVMEDIA_TYPE_AUDIO;
    }
    else {
        upipe_warn(upipe, "unsupported flow format");
        return NULL;
    }

    if (unlikely(ubuf == NULL)) {
        upipe_throw_error(upipe, UBASE_ERR_ALLOC);
        return NULL;
    }

    struct uref *uref = uref_sibling_alloc_control(
        upipe_avfilt_sub->flow_def_alloc);
    if (unlikely(!uref)) {
        ubuf_free(ubuf);
        upipe_throw_error(upipe, UBASE_ERR_ALLOC);
        return NULL;
    }
    uref_attach_ubuf(uref, ubuf);

    /* get system time */
    uint64_t now = upipe_avfilt_sub_now(upipe);

    /* set pts orig */
    AVRational time_base = av_buffersink_get_time_base(
        upipe_avfilt_sub->buffer_ctx);
    uint64_t pts_orig = av_rescale_q(frame->pts, time_base,
                                     av_make_q(1, UCLOCK_FREQ));
    uint64_t pts_prog = pts_orig + upipe_avfilt_sub->pts_prog_offset;
    if (upipe_avfilt_sub->last_pts_prog != UINT64_MAX) {
        if (pts_prog <= upipe_avfilt_sub->last_pts_prog) {
            upipe_warn(upipe, "pts in the past, resetting");
            upipe_avfilt_sub->pts_sys_offset = UINT64_MAX;
            upipe_avfilt_sub->pts_prog_offset +=
                upipe_avfilt_sub->last_pts_prog - pts_prog + 1;
            pts_prog += upipe_avfilt_sub->last_pts_prog - pts_prog + 1;
        }
    }
    upipe_avfilt_sub->last_pts_prog = pts_prog;

    if (upipe_avfilt_sub->pts_sys_offset == UINT64_MAX) {
        upipe_avfilt_sub->pts_sys_offset = now;
        upipe_avfilt_sub->first_pts_prog = pts_prog;
    }
    uint64_t pts_sys = UINT64_MAX;
    if (upipe_avfilt_sub->pts_sys_offset != UINT64_MAX) {
        pts_sys = upipe_avfilt_sub->pts_sys_offset +
            pts_prog - upipe_avfilt_sub->first_pts_prog;
    }

    if (pts_sys < now && now - pts_sys > upipe_avfilt_sub->latency) {
        upipe_avfilt_sub->latency = now - pts_sys;
        uref_clock_set_latency(upipe_avfilt_sub->flow_def,
                               upipe_avfilt_sub->latency);
        struct uref *flow_def = upipe_avfilt_sub->flow_def;
        upipe_avfilt_sub->flow_def = NULL;
        upipe_avfilt_sub_store_flow_def(upipe, flow_def);
    }

    uref_clock_set_pts_orig(uref, pts_orig);
    uref_clock_set_pts_prog(uref, pts_prog);
    if (pts_sys != UINT64_MAX)
        uref_clock_set_pts_sys(uref, pts_sys);

    uint64_t duration = 0;
    switch (media_type) {
        case AVMEDIA_TYPE_VIDEO:
            duration = av_rescale_q(frame->pkt_duration, time_base,
                                    av_make_q(1, UCLOCK_FREQ));

            if (!frame->interlaced_frame)
                UBASE_ERROR(upipe, uref_pic_set_progressive(uref))
            else if (frame->top_field_first)
                UBASE_ERROR(upipe, uref_pic_set_tff(uref))

            if (frame->key_frame)
                UBASE_ERROR(upipe, uref_pic_set_key(uref))

            break;
        case AVMEDIA_TYPE_AUDIO:
            duration = frame->nb_samples * UCLOCK_FREQ / frame->sample_rate;
            break;

        default:
            upipe_err(upipe, "unsupported media type");
            break;
    }
    upipe_avfilt_sub->last_duration = duration;
    UBASE_ERROR(upipe, uref_clock_set_duration(uref, duration));

    upipe_verbose_va(upipe, "output frame %ix%i pts_prog=%f "
                     "pts_sys=%f duration=%f",
                     frame->width, frame->height,
                     (double) pts_prog / UCLOCK_FREQ,
                     (double) pts_sys / UCLOCK_FREQ,
                     (double) duration / UCLOCK_FREQ);

    return uref;
}

/** @internal @This checks for frame to output.
 *
 * @param upipe description structure of the pipe
 * @return an uref or NULL
 */
static struct uref *upipe_avfilt_sub_pop(struct upipe *upipe)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_sub_mgr(upipe->mgr);

    if (upipe_avfilt_sub->flow_def_input)
        return NULL;

    if (unlikely(!upipe_avfilt->configured))
        return NULL;

    AVFrame *frame = av_frame_alloc();
    if (unlikely(!frame)) {
        upipe_err_va(upipe, "cannot allocate av frame");
        upipe_throw_error(upipe, UBASE_ERR_ALLOC);
        return NULL;
    }

    /* pull filtered frames from the filtergraph */
    struct uref *uref = NULL;
    int err = av_buffersink_get_frame(upipe_avfilt_sub->buffer_ctx, frame);
    if (err < 0) {
        if (err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
            upipe_err_va(upipe, "cannot get frame from filter graph: %s",
                av_err2str(err));
                upipe_throw_error(upipe, UBASE_ERR_EXTERNAL);
        }
    } else {
        uref = upipe_avfilt_sub_frame_to_uref(upipe, frame);
        av_frame_unref(frame);
    }
    av_frame_free(&frame);
    return uref;
}

/** @internal @This outputs the retained urefs.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_avfilt_sub_flush(struct upipe *upipe)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);
    struct uref *uref;

    upipe_avfilt_sub_set_upump(upipe, NULL);

    uint64_t now = upipe_avfilt_sub_now(upipe);
    struct uchain *uchain, *uchain_tmp;
    ulist_delete_foreach(&upipe_avfilt_sub->urefs, uchain, uchain_tmp) {
        uref = uref_from_uchain(uchain);
        uint64_t pts_sys = UINT64_MAX;
        uref_clock_get_pts_sys(uref, &pts_sys);
        if (now == UINT64_MAX || pts_sys == UINT64_MAX || pts_sys <= now) {
            ulist_delete(uchain);
            upipe_avfilt_sub_output(upipe, uref, &upipe_avfilt_sub->upump);
        }
        else {
            upipe_avfilt_sub_wait(upipe, pts_sys - now);
            return;
        }
    }

    uref = upipe_avfilt_sub_pop(upipe);
    if (uref) {
        ulist_add(&upipe_avfilt_sub->urefs, uref_to_uchain(uref));
        upipe_avfilt_sub_flush(upipe);
    }
}

/** @internal @This is the callback to output retained urefs.
 *
 * @param upump description structure of the pump
 */
static void upipe_avfilt_sub_flush_cb(struct upump *upump)
{
    struct upipe *upipe = upump_get_opaque(upump, struct upipe *);
    return upipe_avfilt_sub_flush(upipe);
}

/** @internal @This create an AVFilter context from the input flow definition.
 *
 * @param upipe description structure of the pipe or sub pipe
 * @param flow_def input flow definition to build filter from
 * @param buffer_ctx filled with the AVFilter context on success
 * @return an error code
 */
static int build_input_filter(struct upipe *upipe, struct uref *flow_def,
                              AVFilterContext **buffer_ctx)
{
    struct upipe_avfilt *upipe_avfilt = NULL;
    struct upipe_avfilt_sub *upipe_avfilt_sub = NULL;
    const char *name = "input";

    if (!upipe || !upipe->mgr || !flow_def) {
        return UBASE_ERR_INVALID;
    } else if (upipe->mgr->signature == UPIPE_AVFILT_SIGNATURE) {
        upipe_avfilt = upipe_avfilt_from_upipe(upipe);
    } else if (upipe->mgr->signature == UPIPE_AVFILT_SUB_SIGNATURE) {
        upipe_avfilt_sub = upipe_avfilt_sub_from_upipe(upipe);
        upipe_avfilt = upipe_avfilt_from_sub_mgr(upipe->mgr);
        name = upipe_avfilt_sub->name;
    } else {
        return UBASE_ERR_INVALID;
    }

    AVFilterGraph *filter_graph = upipe_avfilt->filter_graph;
    if (!filter_graph || !name)
        return UBASE_ERR_INVALID;

    enum AVMediaType type;
    if (ubase_check(uref_flow_match_def(flow_def, UREF_PIC_FLOW_DEF)))
        type = AVMEDIA_TYPE_VIDEO;
    else if (ubase_check(uref_flow_match_def(flow_def, UREF_SOUND_FLOW_DEF)))
        type = AVMEDIA_TYPE_AUDIO;
    else {
        upipe_err(upipe, "unsupported media type");
        return UBASE_ERR_UNHANDLED;
    }

    const char *filter_name = type == AVMEDIA_TYPE_VIDEO ? "buffer" : "abuffer";
    const AVFilter *filter = avfilter_get_by_name(filter_name);
    if (filter == NULL) {
        upipe_err_va(upipe, "no filter found for %s", filter_name);
        return UBASE_ERR_EXTERNAL;
    }

    AVFilterContext *ctx =
        avfilter_graph_alloc_filter(filter_graph, filter, name);
    if (ctx == NULL) {
        upipe_err_va(upipe, "cannot create %s filter for %s", filter_name, name);
        return UBASE_ERR_EXTERNAL;
    }

    AVBufferSrcParameters *p = av_buffersrc_parameters_alloc();
    if (p == NULL) {
        upipe_err_va(upipe, "cannot alloc %s params for %s", filter_name, name);
        avfilter_free(ctx);
        return UBASE_ERR_EXTERNAL;
    }
    p->time_base.num = 1;
    p->time_base.den = UCLOCK_FREQ;
    switch (type) {
        case AVMEDIA_TYPE_VIDEO: {
            const char *chroma_map[UPIPE_AV_MAX_PLANES];
            uint64_t width = 0;
            uint64_t height = 0;
            struct urational sar = { 0, 0 };
            struct urational fps = { 0, 0 };

            p->format =
                upipe_av_pixfmt_from_flow_def(flow_def, NULL, chroma_map);
            uref_pic_flow_get_hsize(flow_def, &width);
            uref_pic_flow_get_vsize(flow_def, &height);
            uref_pic_flow_get_sar(flow_def, &sar);
            uref_pic_flow_get_fps(flow_def, &fps);
            p->width = width;
            p->height = height;
#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(9, 16, 100)
            int matrix_coefficients;
            bool full_range;

            uref_pic_flow_get_matrix_coefficients_val(
                flow_def, &matrix_coefficients);
            full_range = ubase_check(uref_pic_flow_get_full_range(flow_def));
            p->color_space = matrix_coefficients;
            p->color_range = full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
#endif
            if (sar.num) {
                p->sample_aspect_ratio.num = sar.num;
                p->sample_aspect_ratio.den = sar.den;
            }
            if (fps.num) {
                p->frame_rate.num = fps.num;
                p->frame_rate.den = fps.den;
            }
            break;
        }
        case AVMEDIA_TYPE_AUDIO: {
            uint8_t channels;
            uint64_t sample_rate = 0;

            p->format = upipe_av_samplefmt_from_flow_def(flow_def, &channels);
            uref_sound_flow_get_rate(flow_def, &sample_rate);
            p->sample_rate = sample_rate;
            av_channel_layout_default(&p->ch_layout, channels);
            break;
        }
        default:
            break;
    }

    int err = av_buffersrc_parameters_set(ctx, p);
    av_free(p);
    if (err < 0) {
        upipe_err_va(upipe, "cannot set %s params for %s: %s", filter_name,
                     name, av_err2str(err));
        avfilter_free(ctx);
        return UBASE_ERR_EXTERNAL;
    }
    if (avfilter_init_dict(ctx, NULL)) {
        avfilter_free(ctx);
        return UBASE_ERR_EXTERNAL;
    }

    if (buffer_ctx)
        *buffer_ctx = ctx;
    else
        avfilter_free(ctx);
    return UBASE_ERR_NONE;
}

static int upipe_avfilt_sub_create_filter(struct upipe *upipe)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_sub_mgr(upipe->mgr);
    struct uref *flow_def = upipe_avfilt_sub->flow_def_input;

    if (unlikely(!flow_def))
        return UBASE_ERR_INVALID;

    if (unlikely(upipe_avfilt_sub->buffer_ctx))
        return UBASE_ERR_BUSY;

    if (unlikely(!upipe_avfilt->filter_graph))
        return UBASE_ERR_INVALID;

    return build_input_filter(upipe, flow_def, &upipe_avfilt_sub->buffer_ctx);
}

/** @internal @This allocates and initializes a avfilter sub pipe.
 *
 * @param mgr pointer to upipe manager
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return an allocated and initialized sub pipe or NULL
 */
static struct upipe *upipe_avfilt_sub_alloc(struct upipe_mgr *mgr,
                                            struct uprobe *uprobe,
                                            uint32_t signature,
                                            va_list args)
{
    struct uref *flow_def = NULL;
    struct upipe *upipe =
        upipe_avfilt_sub_alloc_flow(mgr, uprobe, signature, args, &flow_def);
    if (unlikely(!upipe))
        return NULL;

    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_sub_mgr(upipe->mgr);

    upipe_avfilt_sub_init_urefcount(upipe);
    upipe_avfilt_sub_init_flow_def(upipe);
    upipe_avfilt_sub_init_sub(upipe);
    upipe_avfilt_sub_init_output(upipe);
    upipe_avfilt_sub_init_uclock(upipe);
    upipe_avfilt_sub_init_upump_mgr(upipe);
    upipe_avfilt_sub_init_upump(upipe);

    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);
    upipe_avfilt_sub->flow_def_alloc = flow_def;
    upipe_avfilt_sub->ubuf_mgr = ubuf_mgr_use(upipe_avfilt->ubuf_mgr);
    upipe_avfilt_sub->name = NULL;
    upipe_avfilt_sub->pts_prog_offset = 0;
    upipe_avfilt_sub->pts_sys_offset = UINT64_MAX;
    upipe_avfilt_sub->last_pts_prog = UINT64_MAX;
    upipe_avfilt_sub->last_duration = 0;
    upipe_avfilt_sub->buffer_ctx = NULL;
    upipe_avfilt_sub->warn_not_configured = true;
    upipe_avfilt_sub->latency = 0;
    ulist_init(&upipe_avfilt_sub->urefs);

    upipe_throw_ready(upipe);

    int ret = uref_avfilt_flow_get_name(flow_def, &upipe_avfilt_sub->name);
    if (unlikely(!ubase_check(ret))) {
        upipe_warn(upipe, "no avfilter name set");
        upipe_release(upipe);
        return NULL;
    }

    upipe_avfilt_reset(upipe_avfilt_to_upipe(upipe_avfilt));

    return upipe;
}

/** @internal @This is called when there is no more reference on the sub pipe.
 *
 * @param upipe description structure of the sub pipe
 */
static void upipe_avfilt_sub_free(struct upipe *upipe)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_sub_mgr(upipe->mgr);

    struct uchain *uchain;
    while ((uchain = ulist_pop(&upipe_avfilt_sub->urefs)))
        uref_free(uref_from_uchain(uchain));

    upipe_throw_dead(upipe);

    uref_free(upipe_avfilt_sub->flow_def_alloc);
    ubuf_mgr_release(upipe_avfilt_sub->ubuf_mgr);
    upipe_avfilt_sub_clean_upump(upipe);
    upipe_avfilt_sub_clean_upump_mgr(upipe);
    upipe_avfilt_sub_clean_uclock(upipe);
    upipe_avfilt_sub_clean_output(upipe);
    upipe_avfilt_sub_clean_sub(upipe);
    upipe_avfilt_sub_clean_urefcount(upipe);
    upipe_avfilt_clean_filters(upipe_avfilt_to_upipe(upipe_avfilt));
    upipe_avfilt_init_filters(upipe_avfilt_to_upipe(upipe_avfilt));
    upipe_avfilt_sub_clean_flow_def(upipe);
    upipe_avfilt_sub_free_flow(upipe);
}

/** @internal @This converts an uref pic to an avframe.
 *
 * @param upipe description structure of the pipe
 * @param uref input uref to convert
 * @param flow_def input flow def
 * @param media media parameters
 * @param frame filled with the conversion
 * @return an error code
 */
static int upipe_avfilt_avframe_from_uref_pic(struct upipe *upipe,
                                              struct uref *uref,
                                              struct uref *flow_def,
                                              AVFrame *frame)
{
    if (unlikely(
            !ubase_check(uref_flow_match_def(flow_def, UREF_PIC_FLOW_DEF))))
        goto inval;

    uint64_t width = 0;
    uint64_t height = 0;
    uref_pic_flow_get_hsize(flow_def, &width);
    uref_pic_flow_get_vsize(flow_def, &height);

    size_t hsize, vsize;
    if (unlikely(!ubase_check(uref_pic_size(uref, &hsize, &vsize, NULL)) ||
                 hsize != width || vsize != height))
        goto inval;

    const char *chroma_map[UPIPE_AV_MAX_PLANES];
    enum AVPixelFormat pix_fmt =
        upipe_av_pixfmt_from_flow_def(flow_def, NULL, chroma_map);

    for (int i = 0; i < UPIPE_AV_MAX_PLANES && chroma_map[i]; i++) {
        const uint8_t *data;
        size_t stride;
        uint8_t vsub;
        if (unlikely(!ubase_check(uref_pic_plane_read(uref, chroma_map[i], 0, 0,
                                                      -1, -1, &data)) ||
                     !ubase_check(uref_pic_plane_size(
                         uref, chroma_map[i], &stride, NULL, &vsub, NULL))))
            goto inval;
        frame->data[i] = (uint8_t *)data;
        frame->linesize[i] = stride;
        frame->buf[i] = av_buffer_create(frame->data[i],
                                         stride * vsize / vsub,
                                         buffer_free_pic_cb, uref,
                                         AV_BUFFER_FLAG_READONLY);
        if (frame->buf[i] == NULL) {
            uref_pic_plane_unmap(uref, chroma_map[i], 0, 0, -1, -1);
            goto inval;
        }

        /* use this as an avcodec refcount */
        uref_attr_set_priv(uref, i + 1);
    }

    frame->extended_data = frame->data;
    frame->width = hsize;
    frame->height = vsize;
    frame->format = pix_fmt;

    int err = upipe_av_set_frame_properties(upipe, frame, flow_def, uref);
    if (!ubase_check(err)) {
        return err;
    }

    uint64_t pts = UINT64_MAX;
    if (ubase_check(uref_clock_get_pts_prog(uref, &pts)))
        frame->pts = pts;

    uint64_t duration = UINT64_MAX;
    if (ubase_check(uref_clock_get_duration(uref, &duration)))
        frame->pkt_duration = duration;

    upipe_verbose_va(upipe, " input frame %ix%i pts=%f duration=%f",
                     frame->width, frame->height,
                     (double) pts / UCLOCK_FREQ,
                     (double) duration / UCLOCK_FREQ);

    return UBASE_ERR_NONE;

inval:
    upipe_warn(upipe, "invalid buffer received");
    return UBASE_ERR_INVALID;
}

/** @internal @This converts an uref sound to an avframe.
 *
 * @param upipe description structure of the pipe
 * @param uref input uref to convert
 * @param media media parameters
 * @param frame filled with the conversion
 * @return an error code
 */
static int upipe_avfilt_avframe_from_uref_sound(struct upipe *upipe,
                                                struct uref *uref,
                                                struct uref *flow_def,
                                                AVFrame *frame)
{
    if (unlikely(
            !ubase_check(uref_flow_match_def(flow_def, UREF_SOUND_FLOW_DEF))))
        goto inval;

    size_t size;
    uint8_t sample_size;
    UBASE_RETURN(uref_sound_size(uref, &size, &sample_size));

    unsigned i = 0;
    const char *channel;
    uref_sound_foreach_plane(uref, channel) {
        const uint8_t *data;

        if (unlikely(!ubase_check(uref_sound_plane_read_uint8_t(
                        uref, channel, 0, -1, &data)))) {
            upipe_warn_va(upipe, "fail to read channel %s", channel);
            continue;
        }

        frame->data[i] = (uint8_t *)data;
        frame->linesize[i] = size * sample_size;
        frame->buf[i] = av_buffer_create(frame->data[i],
                                         size * sample_size,
                                         buffer_free_sound_cb, uref,
                                         AV_BUFFER_FLAG_READONLY);
        uref_attr_set_priv(uref, i + 1);
        i++;
    }

    uint64_t pts = UINT64_MAX;
    if (ubase_check(uref_clock_get_pts_prog(uref, &pts)))
        frame->pts = pts;

    uint64_t duration = UINT64_MAX;
    if (ubase_check(uref_clock_get_duration(uref, &duration)))
        frame->pkt_duration = duration;

    uint8_t channels = 0;
    uint64_t sample_rate = 0;
    uref_sound_flow_get_rate(flow_def, &sample_rate);
    frame->extended_data = frame->data;
    frame->nb_samples = size;
    frame->format = upipe_av_samplefmt_from_flow_def(flow_def, &channels);
    frame->sample_rate = sample_rate;
    av_channel_layout_default(&frame->ch_layout, channels);

    upipe_verbose_va(upipe, " input frame pts=%f duration=%f",
                     (double) pts / UCLOCK_FREQ,
                     (double) duration / UCLOCK_FREQ);

    return UBASE_ERR_NONE;

inval:
    upipe_warn(upipe, "invalid buffer received");
    return UBASE_ERR_INVALID;
}

/** @internal @This converts an uref to an avframe.
 *
 * @param upipe description structure of the pipe
 * @param uref input uref to convert
 * @param flow_def input flow def
 * @param media media parameters
 * @param frame filled with the conversion
 * @return an error code
 */
static int upipe_avfilt_avframe_from_uref(struct upipe *upipe,
                                          struct uref *uref,
                                          struct uref *flow_def,
                                          AVFrame *frame)
{
    if (unlikely(!flow_def)) {
        upipe_warn(upipe, "no flow def set");
        return UBASE_ERR_INVALID;
    } else if (ubase_check(uref_flow_match_def(flow_def, UREF_PIC_FLOW_DEF)))
        return upipe_avfilt_avframe_from_uref_pic(
            upipe, uref, flow_def, frame);
    else if (ubase_check(uref_flow_match_def(flow_def, UREF_SOUND_FLOW_DEF)))
        return upipe_avfilt_avframe_from_uref_sound(
            upipe, uref, flow_def, frame);

    const char *def = "(none)";
    uref_flow_get_def(flow_def, &def);
    upipe_warn_va(upipe, "unsupported flow def %s", def);
    return UBASE_ERR_INVALID;
}

/** @internal @This handles the input buffer.
 *
 * @param upipe description structure of the subpipe
 * @param uref input buffer to handle
 * @param upump_p reference to the pump that generated the buffer
 */
static void upipe_avfilt_sub_input(struct upipe *upipe,
                                  struct uref *uref,
                                  struct upump **upump_p)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_sub_mgr(upipe->mgr);

    if (unlikely(!upipe_avfilt_sub->flow_def_input)) {
        upipe_err(upipe, "receive buffer in an output sub pipe");
        uref_free(uref);
        return;
    }

    if (unlikely(!upipe_avfilt->configured)) {
        if (upipe_avfilt_sub->warn_not_configured)
            upipe_warn(upipe, "filter graph is not configured");
        upipe_avfilt_sub->warn_not_configured = false;
        uref_free(uref);
        return;
    }
    upipe_avfilt_sub->warn_not_configured = true;

    AVFrame *frame = av_frame_alloc();
    if (unlikely(!frame)) {
        upipe_err_va(upipe, "cannot allocate av frame");
        uref_free(uref);
        upipe_throw_error(upipe, UBASE_ERR_ALLOC);
        return;
    }

    if (!ubase_check(ubuf_av_get_avframe(uref->ubuf, frame))) {
        int ret = upipe_avfilt_avframe_from_uref(upipe, uref,
                                                 upipe_avfilt_sub->flow_def_input,
                                                 frame);
        if (unlikely(!ubase_check(ret))) {
            upipe_throw_error(upipe, ret);
            uref_free(uref);
            av_frame_free(&frame);
            return;
        }
    }
    else {
        uint64_t pts = UINT64_MAX;
        if (ubase_check(uref_clock_get_pts_prog(uref, &pts)))
            frame->pts = pts;

        uint64_t duration = UINT64_MAX;
        if (ubase_check(uref_clock_get_duration(uref, &duration)))
            frame->pkt_duration = duration;

        uref_free(uref);
    }

    int err = av_buffersrc_write_frame(upipe_avfilt_sub->buffer_ctx, frame);
    av_frame_free(&frame);
    if (unlikely(err < 0)) {
        upipe_err_va(upipe, "cannot write frame to filter graph: %s",
                     av_err2str(err));
        upipe_throw_error(upipe, UBASE_ERR_EXTERNAL);
        return;
    }

    upipe_avfilt_update_outputs(upipe_avfilt_to_upipe(upipe_avfilt));
}

/** @internal @This sets the input sub pipe flow definition.
 *
 * @param upipe description structure of the pipe
 * @param flow_def new input flow definition
 * @return an error code
 */
static int upipe_avfilt_sub_set_flow_def(struct upipe *upipe,
                                         struct uref *flow_def)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_sub_mgr(upipe->mgr);

    if (upipe_avfilt_sub->flow_def_input &&
        !udict_cmp(upipe_avfilt_sub->flow_def_input->udict, flow_def->udict))
        /* nothing changed */
        return UBASE_ERR_NONE;

    struct uref *flow_def_dup = uref_dup(flow_def);
    UBASE_ALLOC_RETURN(flow_def_dup);

    upipe_avfilt_clean_filters(upipe_avfilt_to_upipe(upipe_avfilt));
    upipe_avfilt_sub_store_flow_def_input(upipe, flow_def_dup);
    upipe_avfilt_init_filters(upipe_avfilt_to_upipe(upipe_avfilt));

    return UBASE_ERR_NONE;
}

/** @internal @This handles the avfilter sub pipe control commands.
 *
 * @param upipe description structure of the sub pipe
 * @param command control command to handle
 * @param args optional arguments of the control command
 * @return an error code
 */
static int upipe_avfilt_sub_control_real(struct upipe *upipe, int cmd, va_list args)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);

    UBASE_HANDLED_RETURN(upipe_avfilt_sub_control_super(upipe, cmd, args));
    if (!upipe_avfilt_sub->flow_def_input)
        UBASE_HANDLED_RETURN(upipe_avfilt_sub_control_output(upipe, cmd, args));

    switch (cmd) {
        case UPIPE_ATTACH_UPUMP_MGR:
            upipe_avfilt_sub_set_upump(upipe, NULL);
            return upipe_avfilt_sub_attach_upump_mgr(upipe);
        case UPIPE_ATTACH_UCLOCK:
            upipe_avfilt_sub_require_uclock(upipe);
            return UBASE_ERR_NONE;
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_avfilt_sub_set_flow_def(upipe, flow_def);
        }
    }

    return UBASE_ERR_UNHANDLED;
}

/** @internal @This checks the internal state of the sub pipe.
 *
 * @param upipe description structure of the sub pipe
 * @return an error code
 */
static int upipe_avfilt_sub_check(struct upipe *upipe)
{
    struct upipe_avfilt_sub *upipe_avfilt_sub =
        upipe_avfilt_sub_from_upipe(upipe);
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_sub_mgr(upipe->mgr);

    if (!upipe_avfilt_sub->flow_def_input)
        upipe_avfilt_sub_check_upump_mgr(upipe);

    if (!upipe_avfilt_sub->upump_mgr)
        return UBASE_ERR_NONE;

    if (upipe_avfilt->filter_graph)
        upipe_avfilt_sub_wait(upipe, 0);

    return UBASE_ERR_NONE;
}

/** @internal @This handles the avfilter sub pipe control commands and checks the
 * internal state.
 *
 * @param upipe description structure of the sub pipe
 * @param command control command to handle
 * @param args optional arguments of the control command
 * @return an error code
 */
static int upipe_avfilt_sub_control(struct upipe *upipe, int cmd, va_list args)
{
    UBASE_RETURN(upipe_avfilt_sub_control_real(upipe, cmd, args));
    return upipe_avfilt_sub_check(upipe);
}

/** @internal @This sets the configured value and throws an event is the value
 * has changed..
 *
 * @param upipe description structure of the pipe
 * @param configured true if the filter is configured, false otherwise
 * @return an error code
 */
static int upipe_avfilt_set_configured(struct upipe *upipe, bool configured)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);

    if (upipe_avfilt->configured == configured)
        return UBASE_ERR_NONE;

    upipe_notice_va(upipe, "filter %s is %s",
                    upipe_avfilt->filters_desc ?: "(none)",
                    configured ? "configured" :
                    "not configured");
    if (configured)
        return upipe_avfilt_sync_acquired(upipe);
    return upipe_avfilt_sync_lost(upipe);
}

/** @internal @This updates the outputs if needed.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_avfilt_update_outputs(struct upipe *upipe)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);
    struct uchain *uchain;
    ulist_foreach(&upipe_avfilt->subs, uchain) {
        struct upipe_avfilt_sub *sub = upipe_avfilt_sub_from_uchain(uchain);
        if (!sub->flow_def_input)
            upipe_avfilt_sub_wait(upipe_avfilt_sub_to_upipe(sub), 0);
    }
}

/** @internal @This cleans the avfilter graph.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_avfilt_clean_filters(struct upipe *upipe)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);

    if (!upipe_avfilt->filter_graph)
        return;

    struct uchain *uchain;
    ulist_foreach(&upipe_avfilt->subs, uchain) {
        struct upipe_avfilt_sub *sub = upipe_avfilt_sub_from_uchain(uchain);
        sub->buffer_ctx = NULL;
    }
    avfilter_graph_free(&upipe_avfilt->filter_graph);
    upipe_avfilt->buffer_ctx = NULL;
    upipe_avfilt_set_configured(upipe, false);
}

/** @internal @This initializes the avfilter graph.
 * This must be called when all the sub pipes have been created.
 *
 * @param upipe description structure of the pipe
 * @return an error code
 */
static int upipe_avfilt_init_filters(struct upipe *upipe)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);
    struct uchain *uchain;
    int ret = UBASE_ERR_EXTERNAL, err;
    AVFilterInOut *inputs = NULL;
    AVFilterInOut *outputs = NULL;

    if (!upipe_avfilt->filters_desc)
        return UBASE_ERR_NONE;

    if (upipe_avfilt->filter_graph)
        return UBASE_ERR_NONE;

    upipe_avfilt->filter_graph = avfilter_graph_alloc();

    AVDictionaryEntry *option = NULL;
    while ((option = av_dict_get(upipe_avfilt->options,
                                 "", option, AV_DICT_IGNORE_SUFFIX))) {
        err = av_opt_set(upipe_avfilt->filter_graph,
                         option->key, option->value,
                         AV_OPT_SEARCH_CHILDREN);
        if (unlikely(err < 0)) {
            upipe_err_va(upipe, "can't set option %s:%s (%s)",
                         option->key, option->value, av_err2str(err));
            goto end;
        }
    }

    ulist_foreach(&upipe_avfilt->subs, uchain) {
        struct upipe_avfilt_sub *sub = upipe_avfilt_sub_from_uchain(uchain);
        if (!sub->flow_def_input)
            continue;

        ret = upipe_avfilt_sub_create_filter(upipe_avfilt_sub_to_upipe(sub));
        if (unlikely(!ubase_check(ret))) {
            upipe_err_va(upipe, "create filter for input %s failed", sub->name);
            goto end;
        }
    }

    ret = UBASE_ERR_EXTERNAL;

    AVFilterInOut *prev_output = NULL;
    ulist_foreach(&upipe_avfilt->subs, uchain) {
        struct upipe_avfilt_sub *sub = upipe_avfilt_sub_from_uchain(uchain);
        if (!sub->flow_def_input)
            continue;

        AVFilterInOut *inout = avfilter_inout_alloc();
        if (unlikely(!inout)) {
            upipe_err(upipe, "cannot allocate inout");
            goto end;
        }
        inout->name = av_strdup(sub->name);
        inout->filter_ctx = sub->buffer_ctx;
        inout->pad_idx = 0;
        inout->next = NULL;
        if (prev_output)
            prev_output->next = inout;
        else
            outputs = inout;
        prev_output = inout;
    }

    upipe_dbg_va(upipe, "reconfiguring filter %s", upipe_avfilt->filters_desc);
    if ((err = avfilter_graph_parse_ptr(upipe_avfilt->filter_graph,
                                        upipe_avfilt->filters_desc,
                                        &inputs, &outputs,
                                        NULL)) < 0) {
        upipe_err_va(upipe, "cannot parse filter graph: %s",
                     av_err2str(err));
        goto end;
    }

    for (AVFilterInOut *input = inputs; input; input = input->next) {
        int type = avfilter_pad_get_type(input->filter_ctx->output_pads,
                                         input->pad_idx);
        upipe_dbg_va(upipe, "input \"%s\" type %s is not ready",
                     input->name, av_get_media_type_string(type));
        goto end;
    }

    char *filters = NULL;
    for (AVFilterInOut *output = outputs; output; output = output->next) {
        int type = avfilter_pad_get_type(output->filter_ctx->output_pads,
                                         output->pad_idx);
        upipe_dbg_va(upipe, "configure output \"%s\" type %s",
                     output->name, av_get_media_type_string(type));

        struct upipe_avfilt_sub *upipe_avfilt_sub = NULL;
        ulist_foreach(&upipe_avfilt->subs, uchain) {
            struct upipe_avfilt_sub *sub = upipe_avfilt_sub_from_uchain(uchain);
            if (!sub->flow_def_input && sub->name && output->name &&
                !strcmp(sub->name, output->name))
                upipe_avfilt_sub = sub;
        }
        if (!upipe_avfilt_sub) {
            upipe_notice_va(upipe, "output %s is not created yet",
                            output->name);
            free(filters);
            goto end;
        }

        const char *filter = NULL;
        switch (type) {
            case AVMEDIA_TYPE_VIDEO:
                filter = "buffersink";
                break;
            case AVMEDIA_TYPE_AUDIO:
                filter = "abuffersink";
                break;
            default:
                break;
        }

        char *tmp = NULL;
        if (asprintf(&tmp, "%s%s[%s]%s@%s", filters ?: "",
                     filters ? "; " : "",
                     output->name, filter, output->name) < 0) {
            upipe_err(upipe, "fail to allocate string");
            free(filters);
            goto end;
        }
        free(filters);
        filters = tmp;
    }

    err = avfilter_graph_parse_ptr(upipe_avfilt->filter_graph,
                                   filters, NULL, &outputs, NULL);
    free(filters);
    if (err < 0) {
        upipe_err_va(upipe, "cannot parse filter graph: %s",
                     av_err2str(err));
        goto end;
    }

    AVFilterGraph *graph = upipe_avfilt->filter_graph;
    ulist_foreach(&upipe_avfilt->subs, uchain) {
        struct upipe_avfilt_sub *sub = upipe_avfilt_sub_from_uchain(uchain);
        if (sub->flow_def_input)
            continue;

        bool found = false;
        for (unsigned i = 0; i < graph->nb_filters; i++) {
            AVFilterContext *f = graph->filters[i];
            const char *name = f->name;
            if (strncmp(name, "buffersink@", strlen("buffersink@")) &&
                strncmp(name, "abuffersink@", strlen("abuffersink@")))
                continue;
            name = strchr(name, '@');
            name++;
            if (!strcmp(sub->name, name)) {
                sub->buffer_ctx = f;
                found = true;
                break;
            }
        }
        if (!found) {
            upipe_err_va(upipe, "%s not found", sub->name);
            goto end;
        }
    }

    if ((err = avfilter_graph_config(upipe_avfilt->filter_graph, NULL)) < 0) {
        upipe_err_va(upipe, "cannot configure filter graph: %s",
                     av_err2str(err));
        goto end;
    }

    ret = UBASE_ERR_NONE;
    upipe_avfilt_set_configured(upipe, true);

    upipe_avfilt_update_outputs(upipe_avfilt_to_upipe(upipe_avfilt));

end:
    if (!ubase_check(ret))
        upipe_avfilt_clean_filters(upipe);
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

/** @internal @This reinitialises the filter.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_avfilt_reset(struct upipe *upipe)
{
    upipe_avfilt_clean_filters(upipe);
    upipe_avfilt_init_filters(upipe);
}

/** @internal @This sets the filter graph description.
 *
 * @param upipe description structure of the pipe
 * @param filters_desc filter graph description
 * @return an error code
 */
static int _upipe_avfilt_set_filters_desc(struct upipe *upipe,
                                          const char *filters_desc)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);
    char *filters_desc_dup = strdup(filters_desc);
    UBASE_ALLOC_RETURN(filters_desc_dup);
    free(upipe_avfilt->filters_desc);
    upipe_avfilt->filters_desc = filters_desc_dup;
    upipe_avfilt_reset(upipe);
    return UBASE_ERR_NONE;
}

/** @This sets the hardware configuration.
 *
 * @param upipe description structure of the pipe
 * @param hw_type hardware type
 * @param hw_device hardware device (use NULL for default)
 * @return an error code
 */
static int _upipe_avfilt_set_hw_config(struct upipe *upipe,
                                       const char *hw_type,
                                       const char *hw_device)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);

    if (hw_type == NULL)
        return UBASE_ERR_INVALID;
    enum AVHWDeviceType hw_device_type =
        av_hwdevice_find_type_by_name(hw_type);
    if (hw_device_type == AV_HWDEVICE_TYPE_NONE)
        return UBASE_ERR_INVALID;

    av_buffer_unref(&upipe_avfilt->hw_device_ctx);
    int err = av_hwdevice_ctx_create(&upipe_avfilt->hw_device_ctx,
                                     hw_device_type,
                                     hw_device,
                                     NULL, 0);
    if (err < 0) {
        upipe_err_va(upipe, "cannot create hw device context: %s",
                     av_err2str(err));
        return UBASE_ERR_EXTERNAL;
    }

    return UBASE_ERR_NONE;
}

/** @This sends a command to one or more filter instances
 *
 * @param upipe description structure of the pipe
 * @param target the filter(s) to which the command should be sent "all" sends
 * to all filters otherwise it can be a filter or filter instance name which
 * will send the command to all matching filters.
 * @param command the command to send
 * @param arg the arguments of the command
 * @return an error code
 */
static int _upipe_avfilt_send_command(struct upipe *upipe, const char *target,
                                      const char *command, const char *arg)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);

    if (unlikely(!upipe_avfilt->configured))
        return UBASE_ERR_INVALID;

    int err = avfilter_graph_send_command(upipe_avfilt->filter_graph, target,
                                          command, arg, NULL, 0, 0);
    if (err < 0)
        return UBASE_ERR_EXTERNAL;

    return UBASE_ERR_NONE;
}

/** @internal @This sets the input pipe flow definition.
 *
 * @param upipe description structure of the pipe
 * @param flow_def new input flow definition
 * @return an error code
 */
static int upipe_avfilt_set_flow_def(struct upipe *upipe,
                                     struct uref *flow_def)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);

    if (upipe_avfilt->filters_desc == NULL)
        return UBASE_ERR_INVALID;

    if (upipe_avfilt->flow_def_input &&
        !udict_cmp(upipe_avfilt->flow_def_input->udict, flow_def->udict))
        /* nothing changed */
        return UBASE_ERR_NONE;

    struct uref *flow_def_dup = uref_dup(flow_def);
    UBASE_ALLOC_RETURN(flow_def_dup);

    upipe_avfilt_clean_filters(upipe_avfilt_to_upipe(upipe_avfilt));
    upipe_avfilt_store_flow_def_input(upipe, NULL);

    struct uref *uref = upipe_avfilt_store_flow_def_input(upipe, flow_def_dup);
    if (uref != NULL)
        /* output flow definition will be set when outputting frames */
        uref_free(uref);

    if (upipe_avfilt->filter_graph == NULL) {
        upipe_avfilt->filter_graph = avfilter_graph_alloc();
        if (upipe_avfilt->filter_graph == NULL) {
            upipe_err(upipe, "cannot allocate filtergraph");
            return UBASE_ERR_ALLOC;
        }

        avfilter_graph_set_auto_convert(upipe_avfilt->filter_graph,
                                        AVFILTER_AUTO_CONVERT_NONE);
    }

    int ret = build_input_filter(upipe, flow_def, &upipe_avfilt->buffer_ctx);
    if (unlikely(!ubase_check(ret))) {
        const char *def = "(none)";
        uref_flow_get_def(flow_def, &def);
        upipe_warn_va(upipe, "unsupported flow def %s", def);
        return ret;
    }

    upipe_notice_va(upipe, "configuring filter %s", upipe_avfilt->filters_desc);

    AVFilterInOut *outputs = avfilter_inout_alloc();
    if (outputs == NULL) {
        upipe_err(upipe, "cannot allocate inout for input");
        return UBASE_ERR_ALLOC;
    }
    outputs->name = av_strdup("in");
    outputs->filter_ctx = upipe_avfilt->buffer_ctx;
    outputs->pad_idx = 0;
    outputs->next = NULL;

    int err;
    if ((err = avfilter_graph_parse_ptr(upipe_avfilt->filter_graph,
                                        upipe_avfilt->filters_desc,
                                        NULL, &outputs,
                                        NULL)) < 0) {
        upipe_err_va(upipe, "cannot parse filter graph: %s",
                     av_err2str(err));
        goto end;
    }

    if (outputs != NULL) {
        const char *filters = NULL;
        switch (avfilter_pad_get_type(outputs->filter_ctx->output_pads,
                                      outputs->pad_idx)) {
            case AVMEDIA_TYPE_VIDEO:
                filters = "[out]buffersink";
                break;
            case AVMEDIA_TYPE_AUDIO:
                filters = "[out]abuffersink";
                break;
            default:
                upipe_err(upipe, "unknown output media type");
                err = -1;
                goto end;
        }

        if ((err = avfilter_graph_parse_ptr(upipe_avfilt->filter_graph,
                                            filters, NULL, &outputs,
                                            NULL)) < 0) {
            upipe_err_va(upipe, "cannot parse filter graph: %s",
                         av_err2str(err));
            goto end;
        }

        AVFilterGraph *graph = upipe_avfilt->filter_graph;
        upipe_avfilt->buffersink_ctx = graph->filters[graph->nb_filters - 1];
    }

end:
    avfilter_inout_free(&outputs);

    return err < 0 ? UBASE_ERR_INVALID : UBASE_ERR_NONE;
}

/** @internal @This initializes the source buffer filter with parameters
 * from the first incoming frame.
 *
 * @param upipe description structure of the pipe
 * @param frame input frame
 * @return an error code
 */
static int upipe_avfilt_init_buffer_from_first_frame(struct upipe *upipe,
                                                     AVFrame *frame)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);

    if (upipe_avfilt->filter_graph == NULL) {
        if (!upipe_avfilt->flow_def_input)
            return UBASE_ERR_INVALID;

        // the filter description was reset, so we need to reinitialize the
        // filter
        struct uref *flow_def_input = upipe_avfilt->flow_def_input;
        upipe_avfilt->flow_def_input = NULL;
        int ret = upipe_avfilt_set_flow_def(upipe, flow_def_input);
        uref_free(flow_def_input);
        if (unlikely(!ubase_check(ret)))
            return ret;
    }

    AVBufferSrcParameters *p = av_buffersrc_parameters_alloc();
    if (p == NULL) {
        upipe_err(upipe, "cannot alloc buffer parameters");
        return UBASE_ERR_ALLOC;
    }
    p->format = frame->format;
    p->width = frame->width;
    p->height = frame->height;
    p->hw_frames_ctx = frame->hw_frames_ctx;

#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(9, 16, 100)
    p->color_space = frame->colorspace;
    p->color_range = frame->color_range;
#endif

    int err = av_buffersrc_parameters_set(upipe_avfilt->buffer_ctx, p);
    av_free(p);
    if (err < 0) {
        upipe_err_va(upipe, "cannot set buffer parameters: %s",
                     av_err2str(err));
        return UBASE_ERR_EXTERNAL;
    }

    AVBufferRef *device_ctx = upipe_avfilt->hw_device_ctx;
    if (device_ctx == NULL && frame->hw_frames_ctx != NULL) {
        AVHWFramesContext *hw_frames =
            (AVHWFramesContext *) frame->hw_frames_ctx->data;
        device_ctx = hw_frames->device_ref;
    }

    if (device_ctx != NULL) {
        AVFilterGraph *graph = upipe_avfilt->filter_graph;
        for (int i = 0; i < graph->nb_filters; i++) {
            av_buffer_unref(&graph->filters[i]->hw_device_ctx);
            graph->filters[i]->hw_device_ctx = av_buffer_ref(device_ctx);
            if (graph->filters[i]->hw_device_ctx == NULL) {
                upipe_err(upipe, "cannot alloc hw device context");
                return UBASE_ERR_ALLOC;
            }
        }
    }

    return UBASE_ERR_NONE;
}

/** @internal @This builds the flow definition packet using attributes
 * from the buffersink and the first frame.
 *
 * @param upipe description structure of the pipe
 * @param frame first frame
 * @return the output flow definition or NULL in case of error
 */
static struct uref *upipe_avfilt_build_flow_def(struct upipe *upipe,
                                                const AVFrame *frame)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);
    AVFilterContext *ctx = upipe_avfilt->buffersink_ctx;

    struct uref *flow_def = upipe_avfilt_alloc_flow_def_attr(upipe);
    if (unlikely(flow_def == NULL))
        return NULL;

    int ret = build_flow_def(flow_def, ctx, frame);
    if (unlikely(!ubase_check(ret))) {
        uref_free(flow_def);
        upipe_err_va(upipe, "unknown buffersink type");
        return NULL;
    }
    return flow_def;
}

/** @internal @This outputs a frame.
 *
 * @param upipe description structure of the pipe
 * @param frame AVFrame to output
 * @param upump_p reference to the pump that generated the buffer
 */
static void upipe_avfilt_output_frame(struct upipe *upipe,
                                      AVFrame *frame,
                                      struct upump **upump_p)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);

    AVRational time_base = av_buffersink_get_time_base(
        upipe_avfilt->buffersink_ctx);

    struct uref *flow_def_attr = upipe_avfilt_build_flow_def(upipe, frame);
    if (unlikely(flow_def_attr == NULL)) {
        upipe_throw_error(upipe, UBASE_ERR_ALLOC);
        return;
    }

    if (!upipe_avfilt_check_flow_def_attr(upipe, flow_def_attr)) {
        struct uref *flow_def =
            upipe_avfilt_store_flow_def_attr(upipe, flow_def_attr);
        if (flow_def == NULL) {
            upipe_throw_error(upipe, UBASE_ERR_ALLOC);
            return;
        }

        uint64_t latency = 0;
        if (frame->pts != AV_NOPTS_VALUE) {
            uint64_t pts = av_rescale_q(frame->pts, time_base,
                                        av_make_q(1, UCLOCK_FREQ));
            uint64_t input_pts;
            if (ubase_check(uref_clock_get_pts_prog(
                        upipe_avfilt->uref, &input_pts)) &&
                input_pts >= pts) {
                latency = input_pts - pts;
                upipe_notice_va(upipe, "latency: %" PRIu64 " ms",
                                1000 * latency / UCLOCK_FREQ);
            }
        }
        uint64_t input_latency = 0;
        uref_clock_get_latency(upipe_avfilt->flow_def_input, &input_latency);
        uref_clock_set_latency(flow_def, input_latency + latency);

        upipe_avfilt_store_flow_def(upipe, flow_def);
    } else {
        uref_free(flow_def_attr);
    }

    if (unlikely(!upipe_avfilt->ubuf_mgr)) {
        upipe_warn(upipe, "no ubuf manager for now");
        return;
    }

    enum AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;

    struct ubuf *ubuf = NULL;
    if (ubase_check(uref_flow_match_def(
                upipe_avfilt->flow_def, UREF_PIC_FLOW_DEF))) {
        ubuf = ubuf_pic_av_alloc(upipe_avfilt->ubuf_mgr, frame);
        media_type = AVMEDIA_TYPE_VIDEO;

    } else if (ubase_check(uref_flow_match_def(
                upipe_avfilt->flow_def, UREF_SOUND_FLOW_DEF))) {
        ubuf = ubuf_sound_av_alloc(upipe_avfilt->ubuf_mgr, frame);
        media_type = AVMEDIA_TYPE_AUDIO;

    } else {
        upipe_warn(upipe, "unsupported flow format");
        return;
    }

    if (ubuf == NULL) {
        upipe_throw_error(upipe, UBASE_ERR_ALLOC);
        return;
    }

    struct uref *uref = uref_dup(upipe_avfilt->uref);
    if (uref == NULL) {
        ubuf_free(ubuf);
        upipe_throw_error(upipe, UBASE_ERR_ALLOC);
        return;
    }
    uref_attach_ubuf(uref, ubuf);

    /* set pts prog */
    uint64_t pts_prog = UINT64_MAX;
    uint64_t pts_sys = UINT64_MAX;
    if (frame->pts != AV_NOPTS_VALUE) {
        uint64_t pts = av_rescale_q(frame->pts, time_base,
                                    av_make_q(1, UCLOCK_FREQ));

        if (ubase_check(uref_clock_get_pts_prog(uref, &pts_prog)) &&
            ubase_check(uref_clock_get_pts_sys(uref, &pts_sys))) {
            int64_t pts_offset = pts_sys - pts_prog;
            uref_clock_set_pts_sys(uref, pts_sys = pts + pts_offset);
        }

        uref_clock_set_pts_prog(uref, pts_prog = pts);
    }

    uint64_t duration = 0;
    switch (media_type) {
        case AVMEDIA_TYPE_VIDEO:
            duration = av_rescale_q(frame->pkt_duration, time_base,
                                    av_make_q(1, UCLOCK_FREQ));

            if (!frame->interlaced_frame)
                UBASE_ERROR(upipe, uref_pic_set_progressive(uref))
            else if (frame->top_field_first)
                UBASE_ERROR(upipe, uref_pic_set_tff(uref))

            if (frame->key_frame)
                UBASE_ERROR(upipe, uref_pic_set_key(uref))

            break;
        case AVMEDIA_TYPE_AUDIO:
            duration = frame->nb_samples * UCLOCK_FREQ / frame->sample_rate;
            break;
        default:
            break;
    }
    UBASE_ERROR(upipe, uref_clock_set_duration(uref, duration));

    upipe_verbose_va(upipe, "output frame %ix%i pts_prog=%f "
                     "pts_sys=%f duration=%f",
                     frame->width, frame->height,
                     (double) pts_prog / UCLOCK_FREQ,
                     (double) pts_sys / UCLOCK_FREQ,
                     (double) duration / UCLOCK_FREQ);

    upipe_avfilt_output(upipe, uref, upump_p);
}

/** @internal @This handles the input buffer.
 *
 * @param upipe description structure of the pipe
 * @param uref input buffer to handle
 * @param upump_p reference to the pump that generated the buffer
 */
static void upipe_avfilt_input(struct upipe *upipe,
                               struct uref *uref,
                               struct upump **upump_p)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);
    int ret, err;

    AVFrame *frame = av_frame_alloc();
    if (frame == NULL) {
        upipe_err_va(upipe, "cannot allocate av frame");
        uref_free(uref);
        upipe_throw_error(upipe, UBASE_ERR_ALLOC);
        return;
    }

    uref_free(upipe_avfilt->uref);
    upipe_avfilt->uref = uref;

    if (!ubase_check(ubuf_av_get_avframe(uref->ubuf, frame))) {
        struct uref *uref_tmp = uref_dup(uref);
        ret = upipe_avfilt_avframe_from_uref(upipe, uref_tmp,
                                             upipe_avfilt->flow_def_input,
                                             frame);
        if (!ubase_check(ret)) {
            upipe_throw_error(upipe, ret);
            uref_free(uref_tmp);
            goto end;
        }
    }
    else {
        uint64_t pts = UINT64_MAX;
        if (ubase_check(uref_clock_get_pts_prog(uref, &pts)))
            frame->pts = pts;

        uint64_t duration = UINT64_MAX;
        if (ubase_check(uref_clock_get_duration(uref, &duration)))
            frame->pkt_duration = duration;
    }

    if (!upipe_avfilt->configured) {
        ret = upipe_avfilt_init_buffer_from_first_frame(upipe, frame);
        if (!ubase_check(ret)) {
            upipe_throw_error(upipe, ret);
            goto end;
        }
        err = avfilter_graph_config(upipe_avfilt->filter_graph, NULL);
        if (err < 0) {
            upipe_err_va(upipe, "cannot configure filter graph: %s",
                         av_err2str(err));
            avfilter_graph_free(&upipe_avfilt->filter_graph);
            upipe_throw_error(upipe, UBASE_ERR_EXTERNAL);
            goto end;
        }
        upipe_avfilt_set_configured(upipe, true);
    }

    /* push incoming frame to the filtergraph */
    err = av_buffersrc_write_frame(upipe_avfilt->buffer_ctx, frame);
    av_frame_unref(frame);
    if (unlikely(err < 0)) {
        upipe_err_va(upipe, "cannot write frame to filter graph: %s",
                     av_err2str(err));
        upipe_throw_error(upipe, UBASE_ERR_EXTERNAL);
        goto end;
    }

    /* pull filtered frames from the filtergraph */
    while (1) {
        err = av_buffersink_get_frame(upipe_avfilt->buffersink_ctx, frame);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
            break;
        if (err < 0) {
            upipe_err_va(upipe, "cannot get frame from filter graph: %s",
                         av_err2str(err));
            upipe_throw_error(upipe, UBASE_ERR_EXTERNAL);
            break;
        }
        upipe_avfilt_output_frame(upipe, frame, upump_p);
        av_frame_unref(frame);
    }

end:
    av_frame_free(&frame);
}

/** @internal @This sets the content of an avfilter option.
 *
 * @param upipe description structure of the pipe
 * @param option name of the option
 * @param content content of the option, or NULL to delete it
 * @return an error code
 */
static int upipe_avfilt_set_option(struct upipe *upipe,
                                   const char *option, const char *content)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);
    int err;

    if (option == NULL)
        return UBASE_ERR_INVALID;

    if (upipe_avfilt->filter_graph != NULL) {
        err = av_opt_set(upipe_avfilt->filter_graph, option, content,
                         AV_OPT_SEARCH_CHILDREN);
        if (err < 0) {
            upipe_err_va(upipe, "can't set option %s:%s (%s)", option, content,
                         av_err2str(err));
            return UBASE_ERR_EXTERNAL;
        }
    }

    err = av_dict_set(&upipe_avfilt->options, option, content, 0);
    if (err < 0) {
        upipe_err_va(upipe, "av_dict_set: %s", av_err2str(err));
        return UBASE_ERR_EXTERNAL;
    }

    return UBASE_ERR_NONE;
}

/** @internal @This processes control commands on an avfilter pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_avfilt_control(struct upipe *upipe,
                                int command, va_list args)
{
    UBASE_HANDLED_RETURN(upipe_avfilt_control_subs(upipe, command, args))
    UBASE_HANDLED_RETURN(upipe_avfilt_control_output(upipe, command, args))

    switch (command) {
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_avfilt_set_flow_def(upipe, flow_def);
        }
        case UPIPE_SET_OPTION: {
            const char *option = va_arg(args, const char *);
            const char *content = va_arg(args, const char *);
            return upipe_avfilt_set_option(upipe, option, content);
        }
        case UPIPE_AVFILT_SET_FILTERS_DESC: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_AVFILT_SIGNATURE)
            const char *filters_desc = va_arg(args, const char *);
            return _upipe_avfilt_set_filters_desc(upipe, filters_desc);
        }
        case UPIPE_AVFILT_SET_HW_CONFIG: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_AVFILT_SIGNATURE)
            const char *hw_type = va_arg(args, const char *);
            const char *hw_device = va_arg(args, const char *);
            return _upipe_avfilt_set_hw_config(upipe, hw_type, hw_device);
        }
        case UPIPE_AVFILT_SEND_COMMAND: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_AVFILT_SIGNATURE)
            const char *target = va_arg(args, const char *);
            const char *command = va_arg(args, const char *);
            const char *arg = va_arg(args, const char *);
            return _upipe_avfilt_send_command(upipe, target, command, arg);
        }
        default:
            return UBASE_ERR_UNHANDLED;
    }
}

/** @internal @This initializes the sub pipes manager.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_avfilt_init_sub_mgr(struct upipe *upipe)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);
    memset(&upipe_avfilt->sub_mgr, 0, sizeof (upipe_avfilt->sub_mgr));
    upipe_avfilt->sub_mgr.signature = UPIPE_AVFILT_SUB_SIGNATURE;
    upipe_avfilt->sub_mgr.refcount = upipe_avfilt_to_urefcount(upipe_avfilt);
    upipe_avfilt->sub_mgr.upipe_alloc = upipe_avfilt_sub_alloc;
    upipe_avfilt->sub_mgr.upipe_input = upipe_avfilt_sub_input;
    upipe_avfilt->sub_mgr.upipe_control = upipe_avfilt_sub_control;
}

/** @internal @This allocates an avfilter pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_avfilt_alloc(struct upipe_mgr *mgr,
                                        struct uprobe *uprobe,
                                        uint32_t signature, va_list args)
{
    struct upipe *upipe = upipe_avfilt_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(upipe == NULL))
        return NULL;

    upipe_avfilt_init_urefcount(upipe);
    upipe_avfilt_init_output(upipe);
    upipe_avfilt_init_flow_def(upipe);
    upipe_avfilt_init_sub_subs(upipe);
    upipe_avfilt_init_sub_mgr(upipe);
    upipe_avfilt_init_sync(upipe);

    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);
    upipe_avfilt->filters_desc = NULL;
    upipe_avfilt->filter_graph = NULL;
    upipe_avfilt->hw_device_ctx = NULL;
    upipe_avfilt->ubuf_mgr = ubuf_av_mgr_alloc();
    upipe_avfilt->buffer_ctx = NULL;
    upipe_avfilt->buffersink_ctx = NULL;
    upipe_avfilt->uref = NULL;
    upipe_avfilt->options = NULL;

    upipe_throw_ready(upipe);

    if (unlikely(!upipe_avfilt->ubuf_mgr)) {
        upipe_release(upipe);
        return NULL;
    }

    return upipe;
}

/** @internal @This frees all resources allocated.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_avfilt_free(struct upipe *upipe)
{
    struct upipe_avfilt *upipe_avfilt = upipe_avfilt_from_upipe(upipe);

    if (upipe_avfilt->buffer_ctx) {
        AVFrame *frame = av_frame_alloc();
        assert(frame);

        /* flush filtergraph by pushing a NULL frame */
        int err = av_buffersrc_write_frame(upipe_avfilt->buffer_ctx, NULL);
        if (unlikely(err < 0)) {
            upipe_err_va(upipe, "cannot write frame to filter graph: %s",
                         av_err2str(err));
        }

        /* flush filtered frames from the filtergraph */
        while (1) {
            err = av_buffersink_get_frame(upipe_avfilt->buffersink_ctx, frame);
            if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
                break;
            if (err < 0) {
                upipe_err_va(upipe, "cannot get frame from filter graph: %s",
                             av_err2str(err));
                upipe_throw_error(upipe, UBASE_ERR_EXTERNAL);
                break;
            }
            upipe_avfilt_output_frame(upipe, frame, NULL);
            av_frame_unref(frame);
        }

        av_frame_free(&frame);
    }

    upipe_throw_dead(upipe);

    upipe_avfilt_clean_filters(upipe);
    free(upipe_avfilt->filters_desc);
    av_buffer_unref(&upipe_avfilt->hw_device_ctx);
    av_dict_free(&upipe_avfilt->options);
    uref_free(upipe_avfilt->uref);
    ubuf_mgr_release(upipe_avfilt->ubuf_mgr);
    upipe_avfilt_clean_sync(upipe);
    upipe_avfilt_clean_sub_subs(upipe);
    upipe_avfilt_clean_urefcount(upipe);
    upipe_avfilt_clean_output(upipe);
    upipe_avfilt_clean_flow_def(upipe);
    upipe_avfilt_free_void(upipe);
}

/** @This returns the pixel format name for the given flow definition.
 *
 * @param flow_def flow definition packet
 * @param name pixel format name
 * @return an error code
 */
static int _upipe_avfilt_mgr_get_pixfmt_name(struct uref *flow_def,
                                             const char **name_p,
                                             bool software)
{
    const char *chroma_map[UPIPE_AV_MAX_PLANES];
    enum AVPixelFormat pix_fmt = software ?
        upipe_av_sw_pixfmt_from_flow_def(flow_def, NULL, chroma_map) :
        upipe_av_pixfmt_from_flow_def(flow_def, NULL, chroma_map);

    const char *name = av_get_pix_fmt_name(pix_fmt);
    if (name == NULL)
        return UBASE_ERR_EXTERNAL;

    if (name_p != NULL)
        *name_p = name;

    return UBASE_ERR_NONE;
}

/** @This processes control commands on a avfilt manager.
 *
 * @param mgr pointer to manager
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_avfilt_mgr_control(struct upipe_mgr *mgr,
                                    int command, va_list args)
{
    switch (command) {
        case UPIPE_AVFILT_MGR_GET_PIXFMT_NAME:
            UBASE_SIGNATURE_CHECK(args, UPIPE_AVFILT_SIGNATURE)
            struct uref *flow_def = va_arg(args, struct uref *);
            const char **name_p = va_arg(args, const char **);
            bool software = va_arg(args, int);
            return _upipe_avfilt_mgr_get_pixfmt_name(flow_def, name_p,
                                                     software);

        case UPIPE_AVFILT_MGR_GET_COLOR_PRIMARIES_NAME: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_AVFILT_SIGNATURE)
            int color_primaries = va_arg(args, int);
            const char **name_p = va_arg(args, const char **);
            const char *name = av_color_primaries_name(color_primaries);
            if (!name)
                return UBASE_ERR_INVALID;
            if (name_p)
                *name_p = name;
            return UBASE_ERR_NONE;
        }

        case UPIPE_AVFILT_MGR_GET_COLOR_TRANSFER_NAME: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_AVFILT_SIGNATURE)
            int color_transfer = va_arg(args, int);
            const char **name_p = va_arg(args, const char **);
            const char *name = av_color_transfer_name(color_transfer);
            if (!name)
                return UBASE_ERR_INVALID;
            if (name_p)
                *name_p = name;
            return UBASE_ERR_NONE;
        }

        case UPIPE_AVFILT_MGR_GET_COLOR_SPACE_NAME: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_AVFILT_SIGNATURE)
            int color_space = va_arg(args, int);
            const char **name_p = va_arg(args, const char **);
            const char *name = av_color_space_name(color_space);
            if (!name)
                return UBASE_ERR_INVALID;
            if (name_p)
                *name_p = name;
            return UBASE_ERR_NONE;
        }

        default:
            return UBASE_ERR_UNHANDLED;
    }
}

/** module manager static descriptor */
static struct upipe_mgr upipe_avfilt_mgr = {
    .refcount = NULL,
    .signature = UPIPE_AVFILT_SIGNATURE,

    .upipe_alloc = upipe_avfilt_alloc,
    .upipe_input = upipe_avfilt_input,
    .upipe_control = upipe_avfilt_control,

    .upipe_mgr_control = upipe_avfilt_mgr_control
};

/** @This returns the management structure for avfilter pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_avfilt_mgr_alloc(void)
{
    return &upipe_avfilt_mgr;
}

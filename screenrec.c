#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libportal/portal.h>
#include <pipewire/pipewire.h>
#include <spa/debug/pod.h>
#include <spa/debug/types.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format.h>
#include <spa/param/video/raw-utils.h>
#include <spa/param/video/raw.h>
#include <spa/utils/result.h>

#define COUNT_OF(x) (sizeof(x) / sizeof(x[0]))

struct context {
    GMainLoop *gloop;
    XdpSession *screencast_session;
    int pipewire_fd;
    guint node_id;

    struct pw_context *context;
    struct pw_core *core;
    struct pw_main_loop *main_loop;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    struct spa_video_info_raw video_info;

    bool write_output;
};

static struct context *g_ctx = NULL;

static void fputs_upper(FILE *out, const char *s) {
    for (; *s; ++s)
        fputc(toupper((unsigned char) *s), out);
}

static void on_pw_stream_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    struct context *ctx = data;

    fprintf(stderr, "State changed: ");
    fputs_upper(stderr, pw_stream_state_as_string(old));
    fprintf(stderr, " -> ");
    fputs_upper(stderr, pw_stream_state_as_string(state));
    fprintf(stderr, "\n");

    if (error)
        fprintf(stderr, "PipeWire error: %s\n", error);

    switch (state) {
        case PW_STREAM_STATE_ERROR:
        case PW_STREAM_STATE_UNCONNECTED:
            pw_main_loop_quit(ctx->main_loop);
            break;
        default:
            break;
    }
}

static void on_pw_param_changed(
        void *data,
        uint32_t id,
        const struct spa_pod *param) {
    struct context *ctx = data;

    if (!param || id != SPA_PARAM_Format)
        return;

    struct spa_video_info info;
    spa_zero(info);

    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
        return;

    if (info.media_type != SPA_MEDIA_TYPE_video ||
        info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    struct spa_video_info_raw raw;
    spa_format_video_raw_parse(param, &raw);

    ctx->video_info = raw;

    fprintf(stderr, "Video format:\n");
    fprintf(stderr, "  Size: %ux%u\n", raw.size.width, raw.size.height);
    fprintf(stderr, "  Framerate: %u/%u\n",
            raw.framerate.num, raw.framerate.denom);
    fprintf(stderr, "  Pixel format: %s\n",
            spa_debug_type_find_name(spa_type_video_format, raw.format));
}

static void on_pw_stream_process(void *data) {
    struct context *ctx = data;

    struct pw_buffer *buf;

    if ((buf = pw_stream_dequeue_buffer(ctx->stream)) == NULL) {
        pw_log_warn("Out of buffers: %m");
        return;
    }

    const struct spa_buffer *spa_buf = buf->buffer;

    const struct spa_chunk *chunk = spa_buf->datas[0].chunk;
    const uint8_t *video_data = (const uint8_t *) spa_buf->datas[0].data;

    if (ctx->write_output && video_data != NULL) {
        const uint8_t *p = video_data + chunk->offset;
        size_t remaining = chunk->size;

        while (remaining > 0) {
            ssize_t written = write(STDOUT_FILENO, p, remaining);
            if (written < 0) {
                fprintf(stderr, "Failed to write frame: %s\n", strerror(errno));
                pw_main_loop_quit(ctx->main_loop);
                break;
            }
            p += written;
            remaining -= (size_t) written;
        }
    }

    pw_stream_queue_buffer(ctx->stream, buf);
}

static const struct pw_stream_events stream_events = {
        PW_VERSION_STREAM_EVENTS,
        .state_changed = on_pw_stream_state_changed,
        .param_changed = on_pw_param_changed,
        .process = on_pw_stream_process,
};

static struct spa_pod *build_format_params(struct spa_pod_builder *b) {
    struct spa_pod_frame format_frame;

    struct spa_rectangle max_resolution = SPA_RECTANGLE(8192, 4320);
    struct spa_rectangle min_resolution = SPA_RECTANGLE(1, 1);
    struct spa_rectangle default_resolution = SPA_RECTANGLE(300, 300);

    struct spa_fraction max_framerate = SPA_FRACTION(1000, 1);
    struct spa_fraction min_framerate = SPA_FRACTION(0, 1);
    struct spa_fraction default_framerate = SPA_FRACTION(25, 1);

    spa_pod_builder_push_object(b, &format_frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);

    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);

    struct spa_pod_frame format_choice;

    spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_format, 0);
    spa_pod_builder_push_choice(b, &format_choice, SPA_CHOICE_Enum, 0);

    spa_pod_builder_id(b, SPA_VIDEO_FORMAT_BGRA);
    spa_pod_builder_id(b, SPA_VIDEO_FORMAT_BGRx);
    spa_pod_builder_id(b, SPA_VIDEO_FORMAT_RGBA);
    spa_pod_builder_id(b, SPA_VIDEO_FORMAT_RGBx);

    spa_pod_builder_pop(b, &format_choice);

    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
                        SPA_POD_CHOICE_RANGE_Rectangle(&default_resolution, &min_resolution, &max_resolution),
                        SPA_FORMAT_VIDEO_framerate,
                        SPA_POD_CHOICE_RANGE_Fraction(&default_framerate, &min_framerate, &max_framerate), 0);

    return spa_pod_builder_pop(b, &format_frame);
}

static void do_quit(int sig) {
    fprintf(stderr, "Received %s, stopping...\n", strsignal(sig));

    pw_main_loop_quit(g_ctx->main_loop);
}

static bool setup_pw_stream(struct context *ctx) {
    if (ctx->node_id == 0) {
        fprintf(stderr, "No screencast stream available\n");
        return false;
    }

    ctx->main_loop = pw_main_loop_new(NULL);
    if (ctx->main_loop == NULL) {
        fprintf(stderr, "Failed to create pipewire main loop\n");
        return false;
    }

    g_ctx = ctx;

    signal(SIGTERM, do_quit);
    signal(SIGINT, do_quit);

    ctx->context = pw_context_new(pw_main_loop_get_loop(ctx->main_loop), NULL, 0);
    if (ctx->context == NULL) {
        fprintf(stderr, "Failed to create pipewire context\n");
        return false;
    }

    ctx->core = pw_context_connect_fd(ctx->context, ctx->pipewire_fd, NULL, 0);
    if (ctx->core == NULL) {
        fprintf(stderr, "Failed to connect to pipewire: %s\n", strerror(errno));
        return false;
    }

    struct pw_properties *props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen",
            NULL);

    ctx->stream = pw_stream_new(ctx->core, "screenrec", props);
    if (ctx->stream == NULL) {
        fprintf(stderr, "Failed to create pipewire stream\n");
        return false;
    }

    pw_stream_add_listener(ctx->stream, &ctx->stream_listener, &stream_events, ctx);

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *param = build_format_params(&b);

    const struct spa_pod *params[1] = {param};

    int ret;
    if ((ret = pw_stream_connect(
                 ctx->stream,
                 PW_DIRECTION_INPUT,
                 ctx->node_id,
                 PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_AUTOCONNECT,
                 params,
                 COUNT_OF(params)))) {
        fprintf(stderr, "Failed to connect pipewire stream: %s\n", spa_strerror(ret));
        return false;
    }

    return true;
}

static guint get_first_node_id(GVariant *streams) {
    if (!streams)
        return 0;

    g_autoptr(GVariant) first = g_variant_get_child_value(streams, 0);
    if (!first)
        return 0;

    guint node_id;
    g_variant_get(first, "(u@a{sv})", &node_id, NULL);

    return node_id;
}

static void on_screencast_session_started(GObject *src, GAsyncResult *res, gpointer data) {
    struct context *ctx = data;

    ctx->screencast_session = XDP_SESSION(src);

    g_autoptr(GError) error = NULL;
    xdp_session_start_finish(ctx->screencast_session, res, &error);

    if (error != NULL) {
        fprintf(stderr, "Failed to start screencast session: %s\n", error->message);
        g_main_loop_quit(ctx->gloop);
        return;
    }

    g_autoptr(GVariant) streams = xdp_session_get_streams(ctx->screencast_session);
    ctx->node_id = get_first_node_id(streams);

    ctx->pipewire_fd = xdp_session_open_pipewire_remote(ctx->screencast_session);

    if (!setup_pw_stream(ctx)) {
        if (ctx->pipewire_fd >= 0)
            close(ctx->pipewire_fd);
        ctx->pipewire_fd = -1;
    }

    g_main_loop_quit(ctx->gloop);
}

static void on_screencast_session_created(GObject *src, GAsyncResult *res, gpointer data) {
    struct context *ctx = data;

    g_autoptr(GError) error = NULL;
    g_autoptr(XdpSession) session = xdp_portal_create_screencast_session_finish(XDP_PORTAL(src), res, &error);

    if (error != NULL) {
        fprintf(stderr, "Failed to create screencast session: %s\n", error->message);
        g_main_loop_quit(ctx->gloop);
        return;
    }

    fprintf(stderr, "Screencast session created successfully\n");

    xdp_session_start(session, NULL, NULL, on_screencast_session_started, data);
}

int main(int argc, char *argv[]) {
    pw_init(&argc, &argv);

    fprintf(stderr, "Compiled with libpipewire %s\n"
                    "Linked with libpipewire %s\n",
            pw_get_headers_version(),
            pw_get_library_version());

    struct context ctx = {0};
    ctx.pipewire_fd = -1;

    if (isatty(STDOUT_FILENO))
        fprintf(stderr, "Warning: stdout is a tty, frames will not be outputted to it\n");
    else
        ctx.write_output = true;

    g_autoptr(XdpPortal) portal = xdp_portal_new();
    g_autoptr(GMainLoop) gloop = g_main_loop_new(NULL, FALSE);

    ctx.gloop = gloop;

    xdp_portal_create_screencast_session(
            portal,
            XDP_OUTPUT_MONITOR | XDP_OUTPUT_WINDOW,
            XDP_SCREENCAST_FLAG_NONE,
            XDP_CURSOR_MODE_EMBEDDED,
            XDP_PERSIST_MODE_NONE,
            NULL,
            NULL,
            on_screencast_session_created,
            &ctx);

    g_main_loop_run(ctx.gloop);

    if (ctx.pipewire_fd < 0)
        return 1;

    pw_main_loop_run(ctx.main_loop);

    if (ctx.stream) {
        pw_stream_disconnect(ctx.stream);
        pw_stream_destroy(ctx.stream);
    }
    if (ctx.core)
        pw_core_disconnect(ctx.core);
    if (ctx.context)
        pw_context_destroy(ctx.context);
    if (ctx.main_loop)
        pw_main_loop_destroy(ctx.main_loop);
    if (ctx.pipewire_fd >= 0)
        close(ctx.pipewire_fd);

    pw_deinit();
    return 0;
}

#include <glib.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include "../renderers/audio_renderer.h"
#include "../renderers/sample_tap.h"
#include "../renderers/video_renderer.h"

static void borrowed_sample_callback(GstSample *sample, void *context);

static logger_t *test_logger;
static guint shape_callback_calls;

static void discard_log_message(void *context, int level, const char *message) {
    (void)context;
    (void)level;
    (void)message;
}

typedef struct video_capture_s {
    GMutex mutex;
    GCond changed;
    guint calls;
    gboolean all_rgba;
    gboolean all_pts_valid;
    GstClockTime pts[3];
} video_capture_t;

static gboolean have_gst_factory(const gchar *name) {
    GstElementFactory *factory = gst_element_factory_find(name);

    if (!factory) {
        return FALSE;
    }
    gst_object_unref(factory);
    return TRUE;
}

static GPtrArray *generate_access_units(const gchar *description) {
    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(description, &error);
    GstElement *sink;
    GPtrArray *buffers;

    g_assert_no_error(error);
    g_assert_nonnull(pipeline);
    sink = gst_bin_get_by_name(GST_BIN(pipeline), "encoded_sink");
    g_assert_nonnull(sink);
    buffers = g_ptr_array_new_with_free_func((GDestroyNotify)gst_buffer_unref);
    g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=,
                    GST_STATE_CHANGE_FAILURE);
    for (guint i = 0; i < 3; i++) {
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink),
                                                         5 * GST_SECOND);
        GstBuffer *buffer;

        g_assert_nonnull(sample);
        buffer = gst_sample_get_buffer(sample);
        g_assert_nonnull(buffer);
        g_ptr_array_add(buffers, gst_buffer_ref(buffer));
        gst_sample_unref(sample);
    }
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *terminal = gst_bus_timed_pop_filtered(bus,
                                                       5 * GST_SECOND,
                                                       GST_MESSAGE_EOS |
                                                       GST_MESSAGE_ERROR);
    g_assert_nonnull(terminal);
    if (GST_MESSAGE_TYPE(terminal) == GST_MESSAGE_ERROR) {
        GError *terminal_error = NULL;
        gchar *debug = NULL;
        gst_message_parse_error(terminal, &terminal_error, &debug);
        g_test_message("encoded AU pipeline ERROR: %s; debug=%s",
                       terminal_error ? terminal_error->message : "(none)",
                       debug ? debug : "(none)");
        g_clear_error(&terminal_error);
        g_free(debug);
    }
    g_assert_cmpint(GST_MESSAGE_TYPE(terminal), ==, GST_MESSAGE_EOS);
    gst_message_unref(terminal);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return buffers;
}

static GPtrArray *generate_h264_access_units(void) {
    return generate_access_units(
        "videotestsrc num-buffers=3 ! "
        "video/x-raw,format=I420,width=64,height=64,framerate=30/1 ! "
        "videoconvert ! openh264enc gop-size=1 ! h264parse config-interval=-1 ! "
        "video/x-h264,stream-format=byte-stream,alignment=au ! "
        "appsink name=encoded_sink sync=false");
}

static GPtrArray *generate_h265_access_units(void) {
    return generate_access_units(
        "videotestsrc num-buffers=3 ! "
        "video/x-raw,format=I420,width=64,height=64,framerate=30/1 ! "
        "videoconvert ! x265enc tune=zerolatency speed-preset=ultrafast key-int-max=1 ! "
        "h265parse config-interval=-1 ! "
        "video/x-h265,stream-format=byte-stream,alignment=au ! "
        "appsink name=encoded_sink sync=false");
}

static void assert_access_units_are_self_contained(GPtrArray *buffers,
                                                   gboolean h265) {
    g_assert_cmpuint(buffers->len, ==, 3);
    for (guint i = 0; i < buffers->len; i++) {
        GstBuffer *buffer = g_ptr_array_index(buffers, i);
        GstMapInfo map;
        gboolean parameter_set_a = FALSE;
        gboolean parameter_set_b = FALSE;
        gboolean parameter_set_c = !h265;
        gboolean random_access = FALSE;

        g_assert_true(gst_buffer_map(buffer, &map, GST_MAP_READ));
        for (gsize offset = 0; offset + 5 < map.size; offset++) {
            gsize nal_offset = 0;
            if (map.data[offset] == 0 && map.data[offset + 1] == 0 &&
                map.data[offset + 2] == 1) {
                nal_offset = offset + 3;
            } else if (map.data[offset] == 0 && map.data[offset + 1] == 0 &&
                       map.data[offset + 2] == 0 && map.data[offset + 3] == 1) {
                nal_offset = offset + 4;
            }
            if (!nal_offset) {
                continue;
            }
            if (h265) {
                guint nal_type = (map.data[nal_offset] >> 1) & 0x3f;
                parameter_set_a |= nal_type == 32;
                parameter_set_b |= nal_type == 33;
                parameter_set_c |= nal_type == 34;
                random_access |= nal_type >= 19 && nal_type <= 21;
            } else {
                guint nal_type = map.data[nal_offset] & 0x1f;
                parameter_set_a |= nal_type == 7;
                parameter_set_b |= nal_type == 8;
                random_access |= nal_type == 5;
            }
        }
        gst_buffer_unmap(buffer, &map);
        g_assert_true(parameter_set_a);
        g_assert_true(parameter_set_b);
        g_assert_true(parameter_set_c);
        g_assert_true(random_access);
    }
}

static void capture_video_sample(GstSample *sample, void *context) {
    video_capture_t *capture = context;
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    const GstStructure *structure = caps ? gst_caps_get_structure(caps, 0) : NULL;
    const gchar *format = structure ? gst_structure_get_string(structure, "format") : NULL;

    g_mutex_lock(&capture->mutex);
    if (!format || g_strcmp0(format, "RGBA") != 0) {
        capture->all_rgba = FALSE;
    }
    if (!buffer || !GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))) {
        capture->all_pts_valid = FALSE;
    }
    if (capture->calls < G_N_ELEMENTS(capture->pts)) {
        capture->pts[capture->calls] = buffer ? GST_BUFFER_PTS(buffer) : GST_CLOCK_TIME_NONE;
    }
    capture->calls++;
    g_cond_broadcast(&capture->changed);
    g_mutex_unlock(&capture->mutex);
}

static gboolean wait_for_video_samples(video_capture_t *capture, guint expected) {
    gint64 deadline = g_get_monotonic_time() + (10 * G_TIME_SPAN_SECOND);

    g_mutex_lock(&capture->mutex);
    while (capture->calls < expected &&
           g_cond_wait_until(&capture->changed, &capture->mutex, deadline)) {
    }
    gboolean complete = capture->calls >= expected;
    g_mutex_unlock(&capture->mutex);
    return complete;
}

static void dump_video_pipeline_diagnostics(const gchar *codec, guint callback_calls) {
    GstElement *pipeline = GST_ELEMENT(video_renderer_get_pipeline());
    GstState current = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    GstElement *queue;
    GstElement *sink;
    GstBus *bus;
    gchar *queue_name;
    gchar *sink_name;
    guint queue_level = 0;

    g_test_message("%s callback count at deadline: %u", codec, callback_calls);
    if (!pipeline) {
        g_test_message("%s active pipeline is NULL", codec);
        return;
    }
    GstStateChangeReturn state_result = gst_element_get_state(pipeline,
                                                               &current,
                                                               &pending,
                                                               0);
    g_test_message("%s pipeline state=%s pending=%s result=%s",
                   codec,
                   gst_element_state_get_name(current),
                   gst_element_state_get_name(pending),
                   gst_element_state_change_return_get_name(state_result));

    queue_name = g_strdup_printf("recording_video_queue_%s", codec);
    sink_name = g_strdup_printf("recording_video_sink_%s", codec);
    queue = gst_bin_get_by_name(GST_BIN(pipeline), queue_name);
    sink = gst_bin_get_by_name(GST_BIN(pipeline), sink_name);
    if (queue) {
        g_object_get(queue, "current-level-buffers", &queue_level, NULL);
        g_test_message("%s recording queue current-level-buffers=%u", codec, queue_level);
        gst_object_unref(queue);
    }
    if (sink) {
        GstStructure *stats = NULL;
        g_object_get(sink, "stats", &stats, NULL);
        if (stats) {
            gchar *stats_text = gst_structure_to_string(stats);
            g_test_message("%s recording appsink stats=%s", codec, stats_text);
            g_free(stats_text);
            gst_structure_free(stats);
        }
        gst_object_unref(sink);
    }
    g_free(sink_name);
    g_free(queue_name);

    bus = gst_element_get_bus(pipeline);
    for (;;) {
        GstMessage *message = gst_bus_pop(bus);
        if (!message) {
            break;
        }
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *error = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(message, &error, &debug);
            g_test_message("%s bus ERROR from %s: %s; debug=%s",
                           codec,
                           message->src ? GST_OBJECT_NAME(message->src) : "(none)",
                           error ? error->message : "(none)",
                           debug ? debug : "(none)");
            g_clear_error(&error);
            g_free(debug);
        } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            g_test_message("%s bus EOS from %s",
                           codec,
                           message->src ? GST_OBJECT_NAME(message->src) : "(none)");
        }
        gst_message_unref(message);
    }
    gst_object_unref(bus);
}

static gboolean push_access_units(GPtrArray *buffers, video_capture_t *capture) {
    for (guint i = 0; i < buffers->len; i++) {
        GstBuffer *encoded = g_ptr_array_index(buffers, i);
        GstMapInfo map;
        int data_len;
        int nal_count = 1;
        uint64_t remote_pts = 10 * GST_SECOND +
                              gst_util_uint64_scale(i, GST_SECOND, 30);

        g_assert_true(gst_buffer_map(encoded, &map, GST_MAP_READ));
        data_len = (int)map.size;
        g_assert_cmpuint(video_renderer_render_buffer(map.data,
                                                      &data_len,
                                                      &nal_count,
                                                      &remote_pts),
                         ==,
                         0);
        gst_buffer_unmap(encoded, &map);
        if (!wait_for_video_samples(capture, i + 1)) {
            return FALSE;
        }
    }
    return TRUE;
}

static void test_video_tap_h264_samples_are_rgba_with_monotonic_pts(void) {
    videoflip_t flip[2] = {NONE, NONE};
    video_capture_t capture = {0};
    GPtrArray *buffers;

    if (!have_gst_factory("videotestsrc") ||
        !have_gst_factory("videoconvert") ||
        !have_gst_factory("openh264enc") ||
        !have_gst_factory("h264parse") ||
        !have_gst_factory("avdec_h264") ||
        !have_gst_factory("appsink")) {
        g_test_skip("H.264 encode/decode plugins are unavailable");
        return;
    }

    buffers = generate_h264_access_units();
    assert_access_units_are_self_contained(buffers, FALSE);
    g_mutex_init(&capture.mutex);
    g_cond_init(&capture.changed);
    capture.all_rgba = TRUE;
    capture.all_pts_valid = TRUE;
    video_renderer_set_sample_callback(capture_video_sample, &capture);
    g_assert_cmpint(video_renderer_init(test_logger,
                                        "RendererSampleTapsTest",
                                        flip,
                                        "h264parse",
                                        "",
                                        "avdec_h264",
                                        "videoconvert",
                                        "appsink",
                                        "",
                                        FALSE,
                                        FALSE,
                                        FALSE,
                                        FALSE,
                                        3,
                                        NULL),
                    ==,
                    0);
    g_assert_cmpint(video_renderer_choose_codec(FALSE, FALSE), ==, 0);

    gboolean received = push_access_units(buffers, &capture);
    if (!received) {
        g_mutex_lock(&capture.mutex);
        guint diagnostic_calls = capture.calls;
        g_mutex_unlock(&capture.mutex);
        dump_video_pipeline_diagnostics("h264", diagnostic_calls);
    }
    video_renderer_set_sample_callback(NULL, NULL);
    video_renderer_destroy();

    g_mutex_lock(&capture.mutex);
    guint calls = capture.calls;
    gboolean all_rgba = capture.all_rgba;
    gboolean all_pts_valid = capture.all_pts_valid;
    GstClockTime first_pts = capture.pts[0];
    GstClockTime second_pts = capture.pts[1];
    GstClockTime third_pts = capture.pts[2];
    g_mutex_unlock(&capture.mutex);

    g_ptr_array_unref(buffers);
    g_cond_clear(&capture.changed);
    g_mutex_clear(&capture.mutex);
    g_assert_true(received);
    g_assert_cmpuint(calls, ==, 3);
    g_assert_true(all_rgba);
    g_assert_true(all_pts_valid);
    g_assert_cmpuint(first_pts, <, second_pts);
    g_assert_cmpuint(second_pts, <, third_pts);
}

static void run_selected_video_renderer_case(GPtrArray *buffers,
                                             gboolean choose_h265,
                                             const gchar *decoder) {
    videoflip_t flip[2] = {NONE, NONE};
    video_capture_t capture = {0};

    g_mutex_init(&capture.mutex);
    g_cond_init(&capture.changed);
    capture.all_rgba = TRUE;
    capture.all_pts_valid = TRUE;
    video_renderer_set_sample_callback(capture_video_sample, &capture);
    g_assert_cmpint(video_renderer_init(test_logger,
                                        "RendererSampleTapsTest",
                                        flip,
                                        "h264parse",
                                        "",
                                        decoder,
                                        "videoconvert",
                                        "appsink",
                                        "",
                                        FALSE,
                                        FALSE,
                                        TRUE,
                                        FALSE,
                                        3,
                                        NULL),
                    ==,
                    0);
    g_assert_cmpint(video_renderer_choose_codec(FALSE, choose_h265), ==, 0);
    gboolean received = push_access_units(buffers, &capture);
    if (!received) {
        g_mutex_lock(&capture.mutex);
        guint diagnostic_calls = capture.calls;
        g_mutex_unlock(&capture.mutex);
        dump_video_pipeline_diagnostics(choose_h265 ? "h265" : "h264",
                                        diagnostic_calls);
    }
    video_renderer_set_sample_callback(NULL, NULL);
    video_renderer_destroy();

    g_mutex_lock(&capture.mutex);
    guint calls = capture.calls;
    gboolean all_rgba = capture.all_rgba;
    gboolean all_pts_valid = capture.all_pts_valid;
    GstClockTime first_pts = capture.pts[0];
    GstClockTime second_pts = capture.pts[1];
    GstClockTime third_pts = capture.pts[2];
    g_mutex_unlock(&capture.mutex);
    g_cond_clear(&capture.changed);
    g_mutex_clear(&capture.mutex);

    g_test_message("selected %s renderer delivered %u samples",
                   choose_h265 ? "H.265" : "H.264",
                   calls);
    g_assert_true(received);
    g_assert_cmpuint(calls, ==, 3);
    g_assert_true(all_rgba);
    g_assert_true(all_pts_valid);
    g_assert_cmpuint(first_pts, <, second_pts);
    g_assert_cmpuint(second_pts, <, third_pts);
}

static void test_only_selected_video_renderer_emits_samples(void) {
    GPtrArray *h264_buffers;
    GPtrArray *h265_buffers;

    if (!have_gst_factory("videotestsrc") ||
        !have_gst_factory("videoconvert") ||
        !have_gst_factory("openh264enc") ||
        !have_gst_factory("avdec_h264") ||
        !have_gst_factory("h264parse") ||
        !have_gst_factory("appsink")) {
        g_test_skip("H.264 encode/decode plugins are unavailable");
        return;
    }

    h264_buffers = generate_h264_access_units();
    assert_access_units_are_self_contained(h264_buffers, FALSE);
    run_selected_video_renderer_case(h264_buffers, FALSE, "avdec_h264");
    g_ptr_array_unref(h264_buffers);

    if (!have_gst_factory("x265enc") ||
        !have_gst_factory("h265parse") ||
        !have_gst_factory("avdec_h265")) {
        g_test_skip("H.265 encode/decode plugins are unavailable");
        return;
    }

    h265_buffers = generate_h265_access_units();
    assert_access_units_are_self_contained(h265_buffers, TRUE);
    run_selected_video_renderer_case(h265_buffers, TRUE, "avdec_h264");
    g_ptr_array_unref(h265_buffers);
}

static GstElement *init_and_choose_video_renderer(gboolean tap_enabled,
                                                  gboolean h265_support,
                                                  gboolean choose_h265) {
    videoflip_t flip[2] = {NONE, NONE};
    GstElement *pipeline;

    video_renderer_set_sample_callback(tap_enabled ? borrowed_sample_callback : NULL,
                                       tap_enabled ? (void *)&shape_callback_calls : NULL);
    g_assert_cmpint(video_renderer_init(test_logger,
                                        "RendererSampleTapsTest",
                                        flip,
                                        "h264parse",
                                        "",
                                        "decodebin",
                                        "videoconvert",
                                        "appsink",
                                        "",
                                        FALSE,
                                        FALSE,
                                        h265_support,
                                        FALSE,
                                        3,
                                        NULL),
                    ==,
                    0);
    g_assert_cmpint(video_renderer_choose_codec(FALSE, choose_h265), ==, 0);
    pipeline = GST_ELEMENT(video_renderer_get_pipeline());
    g_assert_nonnull(pipeline);
    return pipeline;
}

static void assert_pipeline_has_no_element(GstElement *pipeline, const gchar *name) {
    GstElement *element = gst_bin_get_by_name(GST_BIN(pipeline), name);

    g_assert_null(element);
}

static void assert_recording_video_branch(GstElement *pipeline, const gchar *codec) {
    gchar *tee_name = g_strdup_printf("video_raw_tee_%s", codec);
    gchar *queue_name = g_strdup_printf("recording_video_queue_%s", codec);
    gchar *sink_name = g_strdup_printf("recording_video_sink_%s", codec);
    gchar *display_name = g_strdup_printf("appsink_%s", codec);
    GstElement *tee = gst_bin_get_by_name(GST_BIN(pipeline), tee_name);
    GstElement *queue = gst_bin_get_by_name(GST_BIN(pipeline), queue_name);
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), sink_name);
    GstElement *display = gst_bin_get_by_name(GST_BIN(pipeline), display_name);
    gint leaky = 0;
    guint max_size_buffers = 0;
    guint max_buffers = 0;
    gboolean drop = FALSE;
    gboolean sync = TRUE;

    g_assert_nonnull(tee);
    g_assert_nonnull(queue);
    g_assert_nonnull(sink);
    g_assert_nonnull(display);
    g_object_get(queue,
                 "leaky", &leaky,
                 "max-size-buffers", &max_size_buffers,
                 NULL);
    g_assert_cmpint(leaky, ==, 2);
    g_assert_cmpuint(max_size_buffers, ==, 2);
    g_object_get(sink,
                 "max-buffers", &max_buffers,
                 "drop", &drop,
                 "sync", &sync,
                 NULL);
    g_assert_cmpuint(max_buffers, ==, 2);
    g_assert_true(drop);
    g_assert_false(sync);

    gst_object_unref(display);
    gst_object_unref(sink);
    gst_object_unref(queue);
    gst_object_unref(tee);
    g_free(display_name);
    g_free(sink_name);
    g_free(queue_name);
    g_free(tee_name);
}

static void test_video_tap_default_off_keeps_original_pipeline(void) {
    GstElement *pipeline = init_and_choose_video_renderer(FALSE, FALSE, FALSE);
    GstElement *display = gst_bin_get_by_name(GST_BIN(pipeline), "appsink_h264");

    g_assert_nonnull(display);
    assert_pipeline_has_no_element(pipeline, "video_raw_tee_h264");
    assert_pipeline_has_no_element(pipeline, "recording_video_sink_h264");

    gst_object_unref(display);
    video_renderer_set_sample_callback(NULL, NULL);
    video_renderer_destroy();
}

static void test_video_tap_registered_adds_bounded_branch_for_h264_and_h265(void) {
    GstElement *pipeline = init_and_choose_video_renderer(TRUE, TRUE, FALSE);

    assert_recording_video_branch(pipeline, "h264");
    video_renderer_set_sample_callback(NULL, NULL);
    video_renderer_destroy();

    pipeline = init_and_choose_video_renderer(TRUE, TRUE, TRUE);
    assert_recording_video_branch(pipeline, "h265");
    video_renderer_set_sample_callback(NULL, NULL);
    video_renderer_destroy();
}

static void test_video_tap_registered_preserves_non_appsink_display(void) {
    videoflip_t flip[2] = {NONE, NONE};
    GstElement *pipeline;
    GstElement *display;
    GstElement *unexpected_appsink;
    GstElement *recording_sink;
    gboolean display_sync = FALSE;

    video_renderer_set_sample_callback(borrowed_sample_callback,
                                       &shape_callback_calls);
    g_assert_cmpint(video_renderer_init(test_logger,
                                        "RendererSampleTapsTest",
                                        flip,
                                        "h264parse",
                                        "",
                                        "decodebin",
                                        "videoconvert",
                                        "fakesink",
                                        "",
                                        FALSE,
                                        TRUE,
                                        FALSE,
                                        FALSE,
                                        3,
                                        NULL),
                    ==,
                    0);
    g_assert_cmpint(video_renderer_choose_codec(FALSE, FALSE), ==, 0);
    pipeline = GST_ELEMENT(video_renderer_get_pipeline());
    g_assert_nonnull(pipeline);

    display = gst_bin_get_by_name(GST_BIN(pipeline), "fakesink_h264");
    unexpected_appsink = gst_bin_get_by_name(GST_BIN(pipeline), "appsink_h264");
    recording_sink = gst_bin_get_by_name(GST_BIN(pipeline),
                                          "recording_video_sink_h264");
    g_assert_nonnull(display);
    g_assert_null(unexpected_appsink);
    g_assert_nonnull(recording_sink);
    g_object_get(display, "sync", &display_sync, NULL);
    g_assert_true(display_sync);

    gst_object_unref(recording_sink);
    gst_object_unref(display);
    video_renderer_set_sample_callback(NULL, NULL);
    video_renderer_destroy();
}

static void borrowed_sample_callback(GstSample *sample, void *context) {
    guint *calls = context;
    g_assert_nonnull(sample);
    (*calls)++;
}

static void test_registration_api_compiles(void) {
    video_renderer_set_sample_callback(borrowed_sample_callback, NULL);
    video_renderer_set_sample_callback(NULL, NULL);
    audio_renderer_set_sample_callback(borrowed_sample_callback, NULL);
    audio_renderer_set_sample_callback(NULL, NULL);
}

static void test_sample_tap_is_disabled_after_init(void) {
    sample_tap_t tap;

    sample_tap_init(&tap);
    g_assert_false(sample_tap_is_enabled(&tap));
    sample_tap_clear(&tap);
}

static void test_sample_tap_emits_to_registered_callback(void) {
    sample_tap_t tap;
    GstSample *sample = gst_sample_new(NULL, NULL, NULL, NULL);
    guint calls = 0;

    sample_tap_init(&tap);
    sample_tap_set(&tap, borrowed_sample_callback, &calls);
    g_assert_true(sample_tap_is_enabled(&tap));

    sample_tap_emit(&tap, sample);

    g_assert_cmpuint(calls, ==, 1);
    sample_tap_clear(&tap);
    gst_sample_unref(sample);
}

static void test_sample_tap_disable_prevents_callbacks(void) {
    sample_tap_t tap;
    GstSample *sample = gst_sample_new(NULL, NULL, NULL, NULL);
    guint calls = 0;

    sample_tap_init(&tap);
    sample_tap_set(&tap, borrowed_sample_callback, &calls);
    sample_tap_set(&tap, NULL, NULL);
    g_assert_false(sample_tap_is_enabled(&tap));

    sample_tap_emit(&tap, sample);

    g_assert_cmpuint(calls, ==, 0);
    sample_tap_clear(&tap);
    gst_sample_unref(sample);
}

typedef struct blocking_callback_context_s {
    sample_tap_t *tap;
    GMutex mutex;
    GCond changed;
    gboolean entered;
    gboolean release;
    gboolean exited;
    gboolean saw_enabled;
    gboolean wait_timed_out;
    guint calls;
} blocking_callback_context_t;

typedef struct emit_thread_context_s {
    sample_tap_t *tap;
    GstSample *sample;
} emit_thread_context_t;

typedef struct disable_thread_context_s {
    sample_tap_t *tap;
    GMutex mutex;
    GCond changed;
    gboolean started;
    gboolean returned;
} disable_thread_context_t;

static gboolean wait_for_flag(GCond *cond, GMutex *mutex, gboolean *flag) {
    gint64 deadline = g_get_monotonic_time() + (5 * G_TIME_SPAN_SECOND);

    while (!*flag && g_cond_wait_until(cond, mutex, deadline)) {
    }
    return *flag;
}

static void blocking_sample_callback(GstSample *sample, void *context) {
    blocking_callback_context_t *state = context;
    gboolean enabled;
    gint64 deadline;

    g_assert_nonnull(sample);
    enabled = sample_tap_is_enabled(state->tap);

    g_mutex_lock(&state->mutex);
    state->calls++;
    state->saw_enabled = enabled;
    state->entered = TRUE;
    g_cond_broadcast(&state->changed);

    deadline = g_get_monotonic_time() + (5 * G_TIME_SPAN_SECOND);
    while (!state->release && g_cond_wait_until(&state->changed, &state->mutex, deadline)) {
    }
    state->wait_timed_out = !state->release;
    state->exited = TRUE;
    g_cond_broadcast(&state->changed);
    g_mutex_unlock(&state->mutex);
}

static gpointer emit_sample_thread(gpointer data) {
    emit_thread_context_t *state = data;

    sample_tap_emit(state->tap, state->sample);
    return NULL;
}

static gpointer disable_sample_tap_thread(gpointer data) {
    disable_thread_context_t *state = data;

    g_mutex_lock(&state->mutex);
    state->started = TRUE;
    g_cond_broadcast(&state->changed);
    g_mutex_unlock(&state->mutex);

    sample_tap_set(state->tap, NULL, NULL);

    g_mutex_lock(&state->mutex);
    state->returned = TRUE;
    g_cond_broadcast(&state->changed);
    g_mutex_unlock(&state->mutex);
    return NULL;
}

static void test_sample_tap_disable_drains_in_flight_callback(void) {
    sample_tap_t tap;
    GstSample *sample = gst_sample_new(NULL, NULL, NULL, NULL);
    blocking_callback_context_t callback_state = {0};
    emit_thread_context_t emit_state = {&tap, sample};
    disable_thread_context_t disable_state = {0};
    GThread *emit_thread;
    GThread *disable_thread = NULL;
    gboolean callback_entered;
    gboolean disable_started = FALSE;
    gboolean disabled_while_callback_blocked = FALSE;
    gboolean disable_returned_before_release = TRUE;
    gboolean disable_returned_after_release = FALSE;
    gboolean callback_exited;
    gboolean callback_wait_timed_out;
    gboolean callback_saw_enabled;
    guint calls;
    gint64 deadline;

    callback_state.tap = &tap;
    disable_state.tap = &tap;
    g_mutex_init(&callback_state.mutex);
    g_cond_init(&callback_state.changed);
    g_mutex_init(&disable_state.mutex);
    g_cond_init(&disable_state.changed);

    sample_tap_init(&tap);
    sample_tap_set(&tap, blocking_sample_callback, &callback_state);
    emit_thread = g_thread_new("sample-tap-emit", emit_sample_thread, &emit_state);

    g_mutex_lock(&callback_state.mutex);
    callback_entered = wait_for_flag(&callback_state.changed,
                                     &callback_state.mutex,
                                     &callback_state.entered);
    g_mutex_unlock(&callback_state.mutex);

    if (callback_entered) {
        disable_thread = g_thread_new("sample-tap-disable",
                                      disable_sample_tap_thread,
                                      &disable_state);

        g_mutex_lock(&disable_state.mutex);
        disable_started = wait_for_flag(&disable_state.changed,
                                        &disable_state.mutex,
                                        &disable_state.started);
        g_mutex_unlock(&disable_state.mutex);

        deadline = g_get_monotonic_time() + (5 * G_TIME_SPAN_SECOND);
        while (sample_tap_is_enabled(&tap) && g_get_monotonic_time() < deadline) {
            g_usleep(1000);
        }
        disabled_while_callback_blocked = !sample_tap_is_enabled(&tap);

        g_mutex_lock(&disable_state.mutex);
        deadline = g_get_monotonic_time() + (250 * G_TIME_SPAN_MILLISECOND);
        while (!disable_state.returned &&
               g_cond_wait_until(&disable_state.changed,
                                 &disable_state.mutex,
                                 deadline)) {
        }
        disable_returned_before_release = disable_state.returned;
        g_mutex_unlock(&disable_state.mutex);
    }

    g_mutex_lock(&callback_state.mutex);
    callback_state.release = TRUE;
    g_cond_broadcast(&callback_state.changed);
    g_mutex_unlock(&callback_state.mutex);

    if (disable_thread) {
        g_mutex_lock(&disable_state.mutex);
        disable_returned_after_release = wait_for_flag(&disable_state.changed,
                                                       &disable_state.mutex,
                                                       &disable_state.returned);
        g_mutex_unlock(&disable_state.mutex);
    }

    g_thread_join(emit_thread);
    if (disable_thread) {
        g_thread_join(disable_thread);
    }

    sample_tap_emit(&tap, sample);

    g_mutex_lock(&callback_state.mutex);
    callback_exited = callback_state.exited;
    callback_wait_timed_out = callback_state.wait_timed_out;
    callback_saw_enabled = callback_state.saw_enabled;
    calls = callback_state.calls;
    g_mutex_unlock(&callback_state.mutex);

    sample_tap_clear(&tap);
    g_cond_clear(&disable_state.changed);
    g_mutex_clear(&disable_state.mutex);
    g_cond_clear(&callback_state.changed);
    g_mutex_clear(&callback_state.mutex);
    gst_sample_unref(sample);

    g_assert_true(callback_entered);
    g_assert_true(disable_started);
    g_assert_true(disabled_while_callback_blocked);
    g_assert_false(disable_returned_before_release);
    g_assert_true(disable_returned_after_release);
    g_assert_true(callback_exited);
    g_assert_false(callback_wait_timed_out);
    g_assert_true(callback_saw_enabled);
    g_assert_cmpuint(calls, ==, 1);
}

static void test_sample_tap_can_register_after_disable(void) {
    sample_tap_t tap;
    GstSample *sample = gst_sample_new(NULL, NULL, NULL, NULL);
    guint first_calls = 0;
    guint second_calls = 0;

    sample_tap_init(&tap);
    sample_tap_set(&tap, borrowed_sample_callback, &first_calls);
    sample_tap_emit(&tap, sample);
    sample_tap_set(&tap, NULL, NULL);

    sample_tap_set(&tap, borrowed_sample_callback, &second_calls);
    sample_tap_emit(&tap, sample);
    sample_tap_set(&tap, NULL, NULL);
    sample_tap_emit(&tap, sample);

    g_assert_cmpuint(first_calls, ==, 1);
    g_assert_cmpuint(second_calls, ==, 1);
    sample_tap_clear(&tap);
    gst_sample_unref(sample);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    gst_init(&argc, &argv);
    test_logger = logger_init();
    logger_set_level(test_logger, LOGGER_ERR);
    logger_set_callback(test_logger, discard_log_message, NULL);
    g_test_add_func("/renderer/video-tap/default-off-keeps-original-pipeline",
                    test_video_tap_default_off_keeps_original_pipeline);
    g_test_add_func("/renderer/video-tap/registered-adds-bounded-branches",
                    test_video_tap_registered_adds_bounded_branch_for_h264_and_h265);
    g_test_add_func("/renderer/video-tap/registered-preserves-non-appsink-display",
                    test_video_tap_registered_preserves_non_appsink_display);
    g_test_add_func("/renderer/video-tap/h264-rgba-monotonic-pts",
                    test_video_tap_h264_samples_are_rgba_with_monotonic_pts);
    g_test_add_func("/renderer/video-tap/only-selected-renderer-emits",
                    test_only_selected_video_renderer_emits_samples);
    g_test_add_func("/renderer/sample-taps/registration-api-compiles",
                    test_registration_api_compiles);
    g_test_add_func("/renderer/sample-taps/disabled-after-init",
                    test_sample_tap_is_disabled_after_init);
    g_test_add_func("/renderer/sample-taps/emits-to-registered-callback",
                    test_sample_tap_emits_to_registered_callback);
    g_test_add_func("/renderer/sample-taps/disable-prevents-callbacks",
                    test_sample_tap_disable_prevents_callbacks);
    g_test_add_func("/renderer/sample-taps/disable-drains-in-flight-callback",
                    test_sample_tap_disable_drains_in_flight_callback);
    g_test_add_func("/renderer/sample-taps/register-after-disable",
                    test_sample_tap_can_register_after_disable);
    int result = g_test_run();
    video_renderer_set_sample_callback(NULL, NULL);
    video_renderer_destroy();
    logger_destroy(test_logger);
    return result;
}

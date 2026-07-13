#include <glib.h>
#include <gst/gst.h>

#include "../renderers/audio_renderer.h"
#include "../renderers/sample_tap.h"
#include "../renderers/video_renderer.h"

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
    return g_test_run();
}

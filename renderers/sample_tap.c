#include "sample_tap.h"

void sample_tap_init(sample_tap_t *tap) {
    g_mutex_init(&tap->mutex);
    g_cond_init(&tap->idle);
    tap->callback = NULL;
    tap->context = NULL;
    tap->in_flight = 0;
}

void sample_tap_set(sample_tap_t *tap, renderer_sample_callback_t callback, void *context) {
    g_mutex_lock(&tap->mutex);
    tap->callback = callback;
    tap->context = callback ? context : NULL;
    if (!callback) {
        while (tap->in_flight > 0) {
            g_cond_wait(&tap->idle, &tap->mutex);
        }
    }
    g_mutex_unlock(&tap->mutex);
}

gboolean sample_tap_is_enabled(sample_tap_t *tap) {
    gboolean enabled;

    g_mutex_lock(&tap->mutex);
    enabled = tap->callback != NULL;
    g_mutex_unlock(&tap->mutex);
    return enabled;
}

void sample_tap_emit(sample_tap_t *tap, GstSample *sample) {
    renderer_sample_callback_t callback;
    void *context;

    g_mutex_lock(&tap->mutex);
    callback = tap->callback;
    context = tap->context;
    if (callback) {
        tap->in_flight++;
    }
    g_mutex_unlock(&tap->mutex);

    if (!callback) {
        return;
    }

    callback(sample, context);

    g_mutex_lock(&tap->mutex);
    tap->in_flight--;
    if (tap->in_flight == 0) {
        g_cond_broadcast(&tap->idle);
    }
    g_mutex_unlock(&tap->mutex);
}

void sample_tap_clear(sample_tap_t *tap) {
    sample_tap_set(tap, NULL, NULL);
    g_cond_clear(&tap->idle);
    g_mutex_clear(&tap->mutex);
}

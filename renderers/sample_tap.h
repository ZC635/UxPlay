#ifndef SAMPLE_TAP_H
#define SAMPLE_TAP_H

#include <gst/gst.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RENDERER_SAMPLE_CALLBACK_T_DEFINED
#define RENDERER_SAMPLE_CALLBACK_T_DEFINED
typedef void (*renderer_sample_callback_t)(GstSample *sample, void *context);
#endif

/*
 * The callback receives a borrowed GstSample. A consumer that retains the
 * sample after the callback returns must call gst_sample_ref(). Call
 * sample_tap_set() only from outside the sample callback. To replace an active
 * callback or context, disable it with (NULL, NULL), wait for that call to
 * return, then register the replacement; the old context may be freed after
 * disable returns. Register before renderer initialization and disable before
 * renderer destruction. Call sample_tap_clear() only after every producer that
 * can call sample_tap_emit() has permanently stopped; it is not a concurrent
 * destructor.
 */
typedef struct sample_tap_s {
    GMutex mutex;
    GCond idle;
    renderer_sample_callback_t callback;
    void *context;
    guint in_flight;
} sample_tap_t;

void sample_tap_init(sample_tap_t *tap);
void sample_tap_set(sample_tap_t *tap, renderer_sample_callback_t callback, void *context);
gboolean sample_tap_is_enabled(sample_tap_t *tap);
void sample_tap_emit(sample_tap_t *tap, GstSample *sample);
void sample_tap_clear(sample_tap_t *tap);

#ifdef __cplusplus
}
#endif

#endif /* SAMPLE_TAP_H */

/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-23 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <math.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include "audio_renderer.h"
#include "sample_tap.h"
#define SECOND_IN_NSECS 1000000000UL

#define NFORMATS_MAX 4

static GstClockTime gst_audio_pipeline_base_time = GST_CLOCK_TIME_NONE;
static logger_t *logger = NULL;
const char * format[NFORMATS_MAX];
static const unsigned char compression_types[NFORMATS_MAX] = {8, 2, 4, 1};

static const gchar *avdec_aac = "avdec_aac";
static const gchar *avdec_alac = "avdec_alac";
static gboolean aac = FALSE;
static gboolean alac = FALSE;
static gboolean render_audio = FALSE;
static gboolean async = FALSE;
static gboolean vsync = FALSE;
static gboolean sync = FALSE;
static gboolean audio_rtp = FALSE;
static int n_formats = 2;
static sample_tap_t audio_sample_tap;
static gsize audio_sample_tap_initialized = 0;

static sample_tap_t *audio_renderer_get_sample_tap(void) {
    if (g_once_init_enter(&audio_sample_tap_initialized)) {
        sample_tap_init(&audio_sample_tap);
        g_once_init_leave(&audio_sample_tap_initialized, 1);
    }
    return &audio_sample_tap;
}

void audio_renderer_set_sample_callback(renderer_sample_callback_t callback, void *context) {
    sample_tap_set(audio_renderer_get_sample_tap(), callback, context);
}

typedef struct audio_renderer_s {
    GstElement *appsrc; 
    GstElement *pipeline;
    GstElement *volume;
    GstElement *recording_sink;
    gulong recording_sample_handler;
    GstBus *bus;
    unsigned char ct;
} audio_renderer_t ;
static audio_renderer_t *renderer_type[NFORMATS_MAX];
static audio_renderer_t *renderer = NULL;

static GstFlowReturn audio_renderer_recording_new_sample(GstAppSink *sink,
                                                          gpointer user_data) {
    audio_renderer_t *audio_renderer = user_data;
    GstSample *sample;

    g_assert(audio_renderer != NULL);
    g_assert(audio_renderer->recording_sink == GST_ELEMENT(sink));
    sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        return GST_FLOW_EOS;
    }
    sample_tap_emit(audio_renderer_get_sample_tap(), sample);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* GStreamer Caps strings for Airplay-defined audio compression types (ct) */

/* ct = 1; linear PCM (uncompressed): 44100/16/2, S16LE */
static const char lpcm_caps[]="audio/x-raw,rate=(int)44100,channels=(int)2,format=S16LE,layout=interleaved";

/* ct = 2; codec_data is ALAC magic cookie:  44100/16/2 spf = 352 */    
static const char alac_caps[] = "audio/x-alac,mpegversion=(int)4,channnels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)"
                           "00000024""616c6163""00000000""00000160""0010280a""0e0200ff""00000000""00000000""0000ac44";

/* ct = 4; codec_data from MPEG v4 ISO 14996-3 Section 1.6.2.1:  AAC-LC 44100/2 spf = 1024 */
static const char aac_lc_caps[] ="audio/mpeg,mpegversion=(int)4,channnels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)1210";

/* ct = 8; codec_data from MPEG v4 ISO 14996-3 Section 1.6.2.1: AAC_ELD 44100/2  spf = 480 */
static const char aac_eld_caps[] ="audio/mpeg,mpegversion=(int)4,channnels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)f8e85000";

static gboolean check_plugins (void)
{
    GstRegistry *registry = NULL;
    const gchar *needed[] = { "app", "libav", "playback", "autodetect", "videoparsersbad",  NULL};
    const gchar *gst[] = {"plugins-base", "libav", "plugins-base", "plugins-good", "plugins-bad", NULL};
    registry = gst_registry_get ();
    gboolean ret = TRUE;
    for (int i = 0; i < g_strv_length ((gchar **) needed); i++) {
        GstPlugin *plugin = NULL;
        plugin = gst_registry_find_plugin (registry, needed[i]);
        if (!plugin) {
            g_print ("Required gstreamer plugin '%s' not found\n"
                     "Missing plugin is contained in  '[GStreamer 1.x]-%s'\n",needed[i], gst[i]);
            ret = FALSE;
            continue;
        }
        gst_object_unref (plugin);
        plugin = NULL;
    }
    if (ret == FALSE) {
        g_print ("\nif the plugin is installed, but not found, your gstreamer registry may have been corrupted.\n"
                 "to rebuild it when gstreamer next starts, clear your gstreamer cache with:\n"
                 "\"rm -rf ~/.cache/gstreamer-1.0\"\n\n");
    }
    return ret;
}

static gboolean check_plugin_feature (const gchar *needed_feature)
{
    GstPluginFeature *plugin_feature = NULL;
    GstRegistry *registry = gst_registry_get ();
    gboolean ret = TRUE;

    plugin_feature = gst_registry_find_feature (registry, needed_feature, GST_TYPE_ELEMENT_FACTORY);
    if (!plugin_feature) {
        g_print ("Required gstreamer libav plugin feature '%s' not found:\n\n"
	         "This may be missing because the FFmpeg package used by GStreamer-1.x-libav is incomplete.\n"
	         "(Some distributions provide an incomplete FFmpeg due to License or Patent issues:\n"
	         "in such cases a complete version for that distribution is usually made available elsewhere)\n",
	         needed_feature);
        ret = FALSE;
    } else {
        gst_object_unref (plugin_feature);
        plugin_feature = NULL;
    }
    if (ret == FALSE) {
        g_print ("\nif the plugin feature is installed, but not found, your gstreamer registry may have been corrupted.\n"
                 "to rebuild it when gstreamer next starts, clear your gstreamer cache with:\n"
                 "\"rm -rf ~/.cache/gstreamer-1.0\"\n\n");
    }
    return ret;
}

bool gstreamer_init(){
    gst_init(NULL,NULL);    
    return (bool) check_plugins ();
}

int audio_renderer_init(logger_t *render_logger, const char* audiosink, const bool* audio_sync, const bool* video_sync, const char *artp_pipeline) {
    GError *error = NULL;
    GstCaps *caps = NULL;
    GstClock *clock = gst_system_clock_obtain();
    g_object_set(clock, "clock-type", GST_CLOCK_TYPE_REALTIME, NULL);

    audio_rtp = (bool) strlen(artp_pipeline);
    if (audio_rtp) {
        g_print("*** Audio RTP mode enabled: sending to %s\n", artp_pipeline);
    }

    logger = render_logger;
    gboolean tap_enabled = sample_tap_is_enabled(audio_renderer_get_sample_tap());
    n_formats = tap_enabled ? NFORMATS_MAX : 2;
    
    aac = check_plugin_feature (avdec_aac);
    alac = check_plugin_feature (avdec_alac);

    for (int i = 0; i < n_formats ; i++) {
        renderer_type[i] = (audio_renderer_t *)  calloc(1,sizeof(audio_renderer_t));
        g_assert(renderer_type[i]);
        GString *launch = g_string_new("appsrc name=audio_source ! ");
        g_string_append(launch, "queue ! ");
        switch (i) {
        case 0:    /* AAC-ELD */
        case 2:    /* AAC-LC */
            if (aac) g_string_append(launch, "avdec_aac ! ");
            break;
        case 1:    /* ALAC */
            if (alac) g_string_append(launch, "avdec_alac ! ");
            break;
        case 3:   /*PCM*/
            break;
        default:
            break;
        }
        if (tap_enabled) {
            g_string_append_printf(launch,
                                   "audioconvert ! tee name=audio_raw_tee_%u "
                                   "audio_raw_tee_%u. ! queue ! audioresample ! "
                                   "volume name=volume ! ",
                                   compression_types[i],
                                   compression_types[i]);
        } else {
            g_string_append (launch, "audioconvert ! ");
            g_string_append (launch, "audioresample ! ");    /* wasapisink must resample from 44.1 kHz to 48 kHz */
            g_string_append (launch, "volume name=volume ! ");
        }

        if (!audio_rtp) {
            /* Normal path: local audio output */
            g_string_append (launch, "level ! ");
            g_string_append (launch, audiosink);
            switch(i) {
            case 1:  /*ALAC*/
                if (*audio_sync) {
                    g_string_append (launch, " sync=true");
                    async = TRUE;
                } else {
                    g_string_append (launch, " sync=false");
                    async = FALSE;
                }
                break;
            default:
                if (*video_sync) {
                    g_string_append (launch, " sync=true");
                    vsync = TRUE;
                } else {
                    g_string_append (launch, " sync=false");
                    vsync = FALSE;
                }
                break;
            }
        } else {
            /* RTP path: send decoded PCM over RTP */
            /* rtpL16pay requires S16BE (big-endian) format */
            g_string_append (launch, "audioconvert ! audio/x-raw,format=S16BE,rate=44100,channels=2 ! ");
            g_string_append (launch, "rtpL16pay ");
            g_string_append (launch, artp_pipeline);
        }
        if (tap_enabled) {
            guint compression_type = compression_types[i];
            g_string_append_printf(
                launch,
                " audio_raw_tee_%u. ! queue name=recording_audio_queue_%u "
                "leaky=downstream max-size-buffers=8 ! audioconvert ! audioresample ! "
                "audio/x-raw,format=S16LE,rate=44100,channels=2,layout=interleaved ! "
                "appsink name=recording_audio_sink_%u emit-signals=true "
                "max-buffers=8 drop=true sync=false",
                compression_type,
                compression_type,
                compression_type);
        }
        renderer_type[i]->pipeline  = gst_parse_launch(launch->str, &error);
        if (error) {
            logger_log(logger, LOGGER_ERR, "gst_parse_launch error (audio %d):\n %s\n", i+1, error->message);
            g_clear_error (&error);
        }
        if (!renderer_type[i]->pipeline) {
            return -1;
        }
        gst_pipeline_use_clock(GST_PIPELINE_CAST(renderer_type[i]->pipeline), clock);
        renderer_type[i]->bus = gst_element_get_bus(renderer_type[i]->pipeline);
        renderer_type[i]->appsrc = gst_bin_get_by_name (GST_BIN (renderer_type[i]->pipeline), "audio_source");
        renderer_type[i]->volume = gst_bin_get_by_name (GST_BIN (renderer_type[i]->pipeline), "volume");
        if (!renderer_type[i]->volume) {
            return -1;
        }
        renderer_type[i]->ct = compression_types[i];
        switch (i) {
        case 0:
            caps =  gst_caps_from_string(aac_eld_caps);
            format[i] = "AAC-ELD 44100/2";
            break;
        case 1:
            caps =  gst_caps_from_string(alac_caps);
            format[i] = "ALAC 44100/16/2";
            break;
        case 2:
            caps =  gst_caps_from_string(aac_lc_caps);
            format[i] = "AAC-LC 44100/2";
            break;
        case 3:
            caps =  gst_caps_from_string(lpcm_caps);
            format[i] = "PCM 44100/16/2 S16LE";
            break;
        default:
            break;
        }
        logger_log(logger, LOGGER_DEBUG, "Audio format %d: %s",i+1,format[i]);
        logger_log(logger, LOGGER_DEBUG, "GStreamer audio pipeline %d: \"%s\"", i+1, launch->str);
        g_string_free(launch, TRUE);
        g_object_set(renderer_type[i]->appsrc, "caps", caps, "stream-type", 0, "is-live", TRUE, "format", GST_FORMAT_TIME, NULL);
        if (tap_enabled) {
            gchar *recording_sink_name = g_strdup_printf("recording_audio_sink_%u",
                                                          renderer_type[i]->ct);
            renderer_type[i]->recording_sink = gst_bin_get_by_name(
                GST_BIN(renderer_type[i]->pipeline), recording_sink_name);
            g_free(recording_sink_name);
            g_assert(renderer_type[i]->recording_sink != NULL);
            renderer_type[i]->recording_sample_handler = g_signal_connect(
                renderer_type[i]->recording_sink,
                "new-sample",
                G_CALLBACK(audio_renderer_recording_new_sample),
                renderer_type[i]);
        }
        gst_caps_unref(caps);
    }
    g_object_unref(clock);
    return 0;
}

void audio_renderer_stop() {
    if (renderer) {
        gst_app_src_end_of_stream(GST_APP_SRC(renderer->appsrc));
        gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
        renderer = NULL;
    }
}

static void get_renderer_type(unsigned char *ct, int *id) {
    render_audio = FALSE;
    *id = -1;
    for (int i = 0; i < n_formats; i++) {
        if (renderer_type[i]->ct == *ct) {
	    *id = i;
            break;
        }
    }
    switch (*id) {
    case 2:
    case 0:
        if (aac) {
            render_audio = TRUE;
        } else {
            logger_log(logger, LOGGER_INFO, "*** GStreamer libav plugin feature avdec_aac is missing, cannot decode AAC audio");
        }
        sync = vsync;
        break;
    case 1:
        if (alac) {
            render_audio = TRUE;
        } else {
            logger_log(logger, LOGGER_INFO, "*** GStreamer libav plugin feature avdec_alac is missing, cannot decode ALAC audio");
        }
        sync = async;
        break;
    case 3:
        render_audio = TRUE;
	sync = FALSE;
        break;
    default:
        break;
    }
}

void  audio_renderer_start(unsigned char *ct) {
    int id = -1;
    get_renderer_type(ct, &id);
    if (id >= 0 && renderer) {
        if(*ct != renderer->ct) {
            gst_app_src_end_of_stream(GST_APP_SRC(renderer->appsrc));
            gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
            logger_log(logger, LOGGER_INFO, "changed audio connection, format %s", format[id]);
            renderer = renderer_type[id];
            gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
            gst_audio_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
        }
    } else if (id >= 0) {
        logger_log(logger, LOGGER_INFO, "start audio connection, format %s", format[id]);
        renderer = renderer_type[id];
        gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
        gst_audio_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    } else {
        logger_log(logger, LOGGER_ERR, "unknown audio compression type ct = %d", *ct);
    }
}

void audio_renderer_render_buffer(unsigned char* data, int *data_len, unsigned short *seqnum, uint64_t *ntp_time) {
    GstBuffer *buffer = NULL;

    if (!render_audio) return;    /* do nothing unless render_audio == TRUE */

    GstClockTime pts = (GstClockTime) *ntp_time ;    /* now in nsecs */
    //GstClockTimeDiff latency = GST_CLOCK_DIFF(gst_element_get_current_clock_time (renderer->appsrc), pts);
    if (sync) {
        if (pts >= gst_audio_pipeline_base_time) {
            pts -= gst_audio_pipeline_base_time;
        } else {
            logger_log(logger, LOGGER_ERR, "*** invalid ntp_time < gst_audio_pipeline_base_time\n%8.6f ntp_time\n%8.6f base_time",
                       ((double) *ntp_time) / SECOND_IN_NSECS, ((double) gst_audio_pipeline_base_time) / SECOND_IN_NSECS);
            return;
        }
    }
    if (data_len == 0 || renderer == NULL) return;

    /* all audio received seems to be either ct = 8 (AAC_ELD 44100/2 spf 460 ) AirPlay Mirror protocol *
     * or ct = 2 (ALAC 44100/16/2 spf 352) AirPlay protocol.                                           *
     * first byte data[0] of ALAC frame is 0x20,                                                       *
     * first byte of AAC_ELD is 0x8c, 0x8d or 0x8e: 0x100011(00,01,10) in modern devices               *
     *                   but is 0x80, 0x81 or 0x82: 0x100000(00,01,10) in ios9, ios10 devices          *
     * first byte of AAC_LC should be 0xff (ADTS) (but has never been  seen).                          */
    
    buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
    g_assert(buffer != NULL);
    //g_print("audio latency %8.6f\n", (double) latency / SECOND_IN_NSECS);
    GST_BUFFER_PTS(buffer) = pts;
    gst_buffer_fill(buffer, 0, data, *data_len);
    bool valid = false;
    switch (renderer->ct){
    case 8: /*AAC-ELD*/
        switch (data[0]){
        case 0x8c:
        case 0x8d:
        case 0x8e:
        case 0x80:
        case 0x81:
        case 0x82:
            valid = true;
            break;          
        default:
            valid = false;
            break;
        }
        break;
    case 2: /*ALAC*/
        valid = (data[0] == 0x20);
        break;
    case 4:  /*AAC_LC */
        valid = (data[0] == 0xff );
 	break;
    default:
        valid = true;
        break;
    }
    if (valid) {
        gst_app_src_push_buffer(GST_APP_SRC(renderer->appsrc), buffer);
    } else {
        logger_log(logger, LOGGER_ERR, "*** ERROR invalid  audio frame (compression_type %d) skipped ", renderer->ct);
        logger_log(logger, LOGGER_ERR, "***       first byte of invalid frame was  0x%2.2x ", (unsigned int) data[0]);
    }
}

void audio_renderer_set_volume(double volume) {
    if (!renderer || !renderer->volume) {
        return;
    }
    volume = (volume > 10.0) ? 10.0 : volume;
    volume = (volume < 0.0) ? 0.0 : volume;
    g_object_set(renderer->volume, "volume", volume, NULL);
}

void audio_renderer_flush() {
}

void audio_renderer_destroy() {
    audio_renderer_stop();
    for (int i = 0; i < n_formats ; i++ ) {
        if (renderer_type[i]->recording_sample_handler) {
            g_signal_handler_disconnect(renderer_type[i]->recording_sink,
                                        renderer_type[i]->recording_sample_handler);
            renderer_type[i]->recording_sample_handler = 0;
        }
        if (renderer_type[i]->recording_sink) {
            gst_object_unref(renderer_type[i]->recording_sink);
            renderer_type[i]->recording_sink = NULL;
        }
        gst_object_unref (renderer_type[i]->bus);
        renderer_type[i]->bus = NULL;
        gst_object_unref (renderer_type[i]->volume);
        renderer_type[i]->volume = NULL;
        gst_object_unref (renderer_type[i]->appsrc);
        renderer_type[i]->appsrc = NULL;
        gst_object_unref (renderer_type[i]->pipeline);
        renderer_type[i]->pipeline = NULL;
        free(renderer_type[i]);
        renderer_type[i] = NULL;
    }
}

static gboolean gstreamer_audio_pipeline_bus_callback(GstBus *bus, GstMessage *message, void *loop) {
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *debug = NULL;
        gst_message_parse_error (message, &err, &debug);
        logger_log(logger, LOGGER_INFO, "GStreamer error (audio): %s %s", GST_MESSAGE_SRC_NAME(message),err->message);
        g_error_free(err);
        g_free(debug);
        if (renderer->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
        gst_bus_set_flushing(bus, TRUE);
        gst_element_set_state (renderer->pipeline, GST_STATE_READY);
        g_main_loop_quit( (GMainLoop *) loop);
	break;
    }
    case GST_MESSAGE_EOS:
        logger_log(logger, LOGGER_INFO, "GStreamer: End-Of-Stream (audio)");
        break;
    case GST_MESSAGE_ELEMENT:
      // many "level" messages may be sent
        break;
    default:
        /* unhandled message */
        logger_log(logger, LOGGER_DEBUG,"GStreamer unhandled audio bus message: src = %s type = %s",
                   GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message));
        break;
    }
    return TRUE;
}

unsigned int audio_renderer_listen(void *loop, int id) {
    g_assert(id >= 0 && id < n_formats);
    return (unsigned int) gst_bus_add_watch(renderer_type[id]->bus,(GstBusFunc)
                                            gstreamer_audio_pipeline_bus_callback, (gpointer) loop); 
}

/*
 * Copyright (C) 2014 Michal Ratajsky <michal.ratajsky@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the licence, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <glib.h>
#include <glib-object.h>

#include <libmatemixer/matemixer-client-stream.h>
#include <libmatemixer/matemixer-stream.h>

#include <pulse/pulseaudio.h>

#include "pulse-connection.h"
#include "pulse-client-stream.h"
#include "pulse-monitor.h"
#include "pulse-sink.h"
#include "pulse-sink-input.h"
#include "pulse-stream.h"

static void pulse_sink_input_class_init (PulseSinkInputClass *klass);
static void pulse_sink_input_init       (PulseSinkInput      *input);

G_DEFINE_TYPE (PulseSinkInput, pulse_sink_input, PULSE_TYPE_CLIENT_STREAM);

static gboolean      sink_input_set_mute       (MateMixerStream       *stream,
                                                gboolean               mute);
static gboolean      sink_input_set_volume     (MateMixerStream       *stream,
                                                pa_cvolume            *volume);
static gboolean      sink_input_set_parent     (MateMixerClientStream *stream,
                                                MateMixerStream       *parent);
static gboolean      sink_input_remove         (MateMixerClientStream *stream);
static PulseMonitor *sink_input_create_monitor (MateMixerStream       *stream);

static void
pulse_sink_input_class_init (PulseSinkInputClass *klass)
{
    PulseStreamClass       *stream_class;
    PulseClientStreamClass *client_class;

    stream_class = PULSE_STREAM_CLASS (klass);

    stream_class->set_mute       = sink_input_set_mute;
    stream_class->set_volume     = sink_input_set_volume;
    stream_class->create_monitor = sink_input_create_monitor;

    client_class = PULSE_CLIENT_STREAM_CLASS (klass);

    client_class->set_parent = sink_input_set_parent;
    client_class->remove     = sink_input_remove;
}

static void
pulse_sink_input_init (PulseSinkInput *input)
{
}

PulseStream *
pulse_sink_input_new (PulseConnection          *connection,
                      const pa_sink_input_info *info,
                      PulseStream              *parent)
{
    PulseSinkInput *input;

    g_return_val_if_fail (PULSE_IS_CONNECTION (connection), NULL);
    g_return_val_if_fail (info != NULL, NULL);

    /* Consider the sink input index as unchanging parameter */
    input = g_object_new (PULSE_TYPE_SINK_INPUT,
                          "connection", connection,
                          "index", info->index,
                          NULL);

    /* Other data may change at any time, so let's make a use of our update function */
    pulse_sink_input_update (PULSE_STREAM (input), info, parent);

    return PULSE_STREAM (input);
}

gboolean
pulse_sink_input_update (PulseStream              *stream,
                         const pa_sink_input_info *info,
                         PulseStream              *parent)
{
    MateMixerStreamFlags flags = MATE_MIXER_STREAM_OUTPUT |
                                 MATE_MIXER_STREAM_CLIENT |
                                 MATE_MIXER_STREAM_HAS_MUTE |
                                 MATE_MIXER_STREAM_HAS_MONITOR;
    gchar *name;

    const gchar *prop;
    const gchar *description = NULL;

    g_return_val_if_fail (PULSE_IS_SINK_INPUT (stream), FALSE);

    /* Let all the information update before emitting notify signals */
    g_object_freeze_notify (G_OBJECT (stream));

    /* Many mixer applications query the Pulse client list and use the client
     * name here, but we use the name only as an identifier, so let's avoid
     * this unnecessary overhead and use a custom name.
     * Also make sure to make the name unique by including the Pulse index. */
    name = g_strdup_printf ("pulse-stream-client-output-%lu", (gulong) info->index);

    pulse_stream_update_name (stream, name);
    g_free (name);

    prop = pa_proplist_gets (info->proplist, PA_PROP_MEDIA_ROLE);

    if (prop != NULL && !strcmp (prop, "event")) {
        /* The event description seems to provide much better readable
         * description for event streams */
        prop = pa_proplist_gets (info->proplist, PA_PROP_EVENT_DESCRIPTION);

        if (G_LIKELY (prop != NULL))
            description = prop;

        flags |= MATE_MIXER_STREAM_EVENT;
    }
    if (description == NULL)
        description = info->name;

    pulse_stream_update_description (stream, description);
    pulse_stream_update_mute (stream, info->mute ? TRUE : FALSE);

    if (info->client != PA_INVALID_INDEX)
        flags |= MATE_MIXER_STREAM_APPLICATION;

    if (pa_channel_map_can_balance (&info->channel_map))
        flags |= MATE_MIXER_STREAM_CAN_BALANCE;
    if (pa_channel_map_can_fade (&info->channel_map))
        flags |= MATE_MIXER_STREAM_CAN_FADE;

#if PA_CHECK_VERSION(1, 0, 0)
    if (info->has_volume)
        flags |=
            MATE_MIXER_STREAM_HAS_VOLUME |
            MATE_MIXER_STREAM_HAS_DECIBEL_VOLUME;
    if (info->volume_writable)
        flags |= MATE_MIXER_STREAM_CAN_SET_VOLUME;

    /* Flags needed before volume */
    pulse_stream_update_flags (stream, flags);

    if (info->has_volume)
        pulse_stream_update_volume (stream, &info->volume, &info->channel_map);
    else
        pulse_stream_update_volume (stream, NULL, &info->channel_map);
#else
    /* Pre-1.0 PulseAudio does not include the has_volume and volume_writable
     * fields, but does include the volume info, so let's give it a try */
    flags |=
        MATE_MIXER_STREAM_HAS_VOLUME |
        MATE_MIXER_STREAM_HAS_DECIBEL_VOLUME |
        MATE_MIXER_STREAM_CAN_SET_VOLUME;

    /* Flags needed before volume */
    pulse_stream_update_flags (stream, flags);

    pulse_stream_update_volume (stream, &info->volume, &info->channel_map);
#endif

    prop = pa_proplist_gets (info->proplist, PA_PROP_APPLICATION_NAME);
    if (prop != NULL)
        pulse_client_stream_update_app_name (MATE_MIXER_CLIENT_STREAM (stream), prop);

    prop = pa_proplist_gets (info->proplist, PA_PROP_APPLICATION_ID);
    if (prop != NULL)
        pulse_client_stream_update_app_id (MATE_MIXER_CLIENT_STREAM (stream), prop);

    prop = pa_proplist_gets (info->proplist, PA_PROP_APPLICATION_VERSION);
    if (prop != NULL)
        pulse_client_stream_update_app_version (MATE_MIXER_CLIENT_STREAM (stream), prop);

    prop = pa_proplist_gets (info->proplist, PA_PROP_APPLICATION_ICON_NAME);
    if (prop != NULL)
        pulse_client_stream_update_app_icon (MATE_MIXER_CLIENT_STREAM (stream), prop);

    pulse_client_stream_update_parent (MATE_MIXER_CLIENT_STREAM (stream),
                                       MATE_MIXER_STREAM (parent));

    // XXX needs to fix monitor if parent changes

    g_object_thaw_notify (G_OBJECT (stream));
    return TRUE;
}

static gboolean
sink_input_set_mute (MateMixerStream *stream, gboolean mute)
{
    PulseStream *pulse;

    g_return_val_if_fail (PULSE_IS_SINK_INPUT (stream), FALSE);

    pulse = PULSE_STREAM (stream);

    return pulse_connection_set_sink_input_mute (pulse_stream_get_connection (pulse),
                                                 pulse_stream_get_index (pulse),
                                                 mute);
}

static gboolean
sink_input_set_volume (MateMixerStream *stream, pa_cvolume *volume)
{
    PulseStream *pulse;

    g_return_val_if_fail (PULSE_IS_SINK_INPUT (stream), FALSE);
    g_return_val_if_fail (volume != NULL, FALSE);

    pulse = PULSE_STREAM (stream);

    return pulse_connection_set_sink_input_volume (pulse_stream_get_connection (pulse),
                                                   pulse_stream_get_index (pulse),
                                                   volume);
}

static gboolean
sink_input_set_parent (MateMixerClientStream *stream, MateMixerStream *parent)
{
    PulseStream *pulse;

    g_return_val_if_fail (PULSE_IS_SINK_INPUT (stream), FALSE);

    if (G_UNLIKELY (!PULSE_IS_SINK (parent))) {
        g_warning ("Could not change stream parent to %s: not a parent output stream",
                   mate_mixer_stream_get_name (parent));
        return FALSE;
    }

    pulse = PULSE_STREAM (stream);

    return pulse_connection_move_sink_input (pulse_stream_get_connection (pulse),
                                             pulse_stream_get_index (pulse),
                                             pulse_stream_get_index (PULSE_STREAM (parent)));
}

static gboolean
sink_input_remove (MateMixerClientStream *stream)
{
    PulseStream *pulse;

    g_return_val_if_fail (PULSE_IS_SINK_INPUT (stream), FALSE);

    pulse = PULSE_STREAM (stream);

    return pulse_connection_kill_sink_input (pulse_stream_get_connection (pulse),
                                             pulse_stream_get_index (pulse));
}

static PulseMonitor *
sink_input_create_monitor (MateMixerStream *stream)
{
    MateMixerStream *parent;
    PulseStream     *pulse;
    guint32          index;

    g_return_val_if_fail (PULSE_IS_SINK_INPUT (stream), NULL);

    parent = mate_mixer_client_stream_get_parent (MATE_MIXER_CLIENT_STREAM (stream));
    if (G_UNLIKELY (parent == NULL)) {
        g_debug ("Not creating monitor for client stream %s as it is not available",
                 mate_mixer_stream_get_name (stream));
        return NULL;
    }

    pulse = PULSE_STREAM (stream);
    index = pulse_sink_get_monitor_index (PULSE_STREAM (parent));

    if (G_UNLIKELY (index == PA_INVALID_INDEX)) {
        g_debug ("Not creating monitor for client stream %s as it is not available",
                 mate_mixer_stream_get_name (stream));
        return NULL;
    }

    return pulse_connection_create_monitor (pulse_stream_get_connection (pulse),
                                            index,
                                            pulse_stream_get_index (pulse));
}
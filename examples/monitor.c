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

#include <glib.h>
#include <glib-object.h>
#include <string.h>
#include <locale.h>

#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

#include <libmatemixer/matemixer.h>

static MateMixerControl *control;
static GMainLoop *mainloop;

static gchar *
create_app_string (const gchar *app_name,
                   const gchar *app_id,
                   const gchar *app_version)
{
    GString *string;

    string = g_string_new (app_name);

    if (app_version)
        g_string_append_printf (string, " %s", app_version);
    if (app_id)
        g_string_append_printf (string, " (%s)", app_id);

    return g_string_free (string, FALSE);
}

static gchar *
create_volume_bar (MateMixerStream *stream, double *percent)
{
    GString *string;
    gint64   volume;
    gint64   volume_min;
    gint64   volume_max;
    double   p;
    int      i;
    int      length = 30;
    int      stars;

    volume     = mate_mixer_stream_get_volume (stream);
    volume_min = mate_mixer_stream_get_min_volume (stream);
    volume_max = mate_mixer_stream_get_normal_volume (stream);

    string = g_string_new ("[");

    p = (double) (volume - volume_min) / (volume_max - volume_min) * 100;
    if (percent != NULL)
        *percent = p;

    stars = (int) ((p / 100) * length);

    for (i = 0; i < length; i++)
        g_string_append_c (string, (i < stars) ? '*' : '.');

    return g_string_free (g_string_append_c (string, ']'), FALSE);
}

static void
print_devices (void)
{
    const GList      *devices;
    const GList      *ports;
    const GList      *profiles;
    MateMixerProfile *active_profile;

    devices = mate_mixer_control_list_devices (control);

    while (devices) {
        MateMixerDevice *device = MATE_MIXER_DEVICE (devices->data);

        g_print ("Device %s\n"
                 "       |-| Description : %s\n"
                 "       |-| Icon Name   : %s\n\n",
                 mate_mixer_device_get_name (device),
                 mate_mixer_device_get_description (device),
                 mate_mixer_device_get_icon (device));

        ports = mate_mixer_device_list_ports (device);
        while (ports) {
            MateMixerPort *port = MATE_MIXER_PORT (ports->data);

            g_print ("       |-| Port %s\n"
                     "                |-| Description : %s\n"
                     "                |-| Icon Name   : %s\n"
                     "                |-| Priority    : %lu\n"
                     "                |-| Status      : \n\n",
                     mate_mixer_port_get_name (port),
                     mate_mixer_port_get_description (port),
                     mate_mixer_port_get_icon (port),
                     mate_mixer_port_get_priority (port));

            ports = ports->next;
        }

        profiles = mate_mixer_device_list_profiles (device);

        active_profile = mate_mixer_device_get_active_profile (device);
        while (profiles) {
            MateMixerProfile *profile = MATE_MIXER_PROFILE (profiles->data);

            g_print ("       |%c| Profile %s\n"
                     "                |-| Description : %s\n"
                     "                |-| Priority    : %lu\n\n",
                     (profile == active_profile)
                        ? 'A'
                        : '-',
                     mate_mixer_profile_get_name (profile),
                     mate_mixer_profile_get_description (profile),
                     mate_mixer_profile_get_priority (profile));

            profiles = profiles->next;
        }
        g_print ("\n");

        devices = devices->next;
    }
}

static void
print_streams (void)
{
    const GList *streams;
    const GList *ports;

    streams = mate_mixer_control_list_streams (control);

    while (streams) {
        MateMixerStream *stream = MATE_MIXER_STREAM (streams->data);
        gchar           *volume_bar;
        gdouble          volume;

        /* Ignore event streams */
        if (mate_mixer_stream_get_flags (stream) & MATE_MIXER_STREAM_EVENT) {
            streams = streams->next;
            continue;
        }

        volume_bar = create_volume_bar (stream, &volume);

        g_print ("Stream %s\n"
                 "       |-| Description : %s\n"
                 "       |-| Volume      : %s %.1f %%\n"
                 "       |-| Muted       : %s\n"
                 "       |-| Channels    : %d\n"
                 "       |-| Balance     : %.1f\n"
                 "       |-| Fade        : %.1f\n",
                 mate_mixer_stream_get_name (stream),
                 mate_mixer_stream_get_description (stream),
                 volume_bar,
                 volume,
                 mate_mixer_stream_get_mute (stream) ? "Yes" : "No",
                 mate_mixer_stream_get_num_channels (stream),
                 mate_mixer_stream_get_balance (stream),
                 mate_mixer_stream_get_fade (stream));

        if (mate_mixer_stream_get_flags (stream) & MATE_MIXER_STREAM_APPLICATION) {
            MateMixerClientStream *client;
            gchar *app;

            /* The application-specific details are accessible through the client
             * interface, which all client streams implement */
            client = MATE_MIXER_CLIENT_STREAM (stream);

            app = create_app_string (mate_mixer_client_stream_get_app_name (client),
                                     mate_mixer_client_stream_get_app_id (client),
                                     mate_mixer_client_stream_get_app_version (client));

            g_print ("       |-| Application : %s\n", app);
            g_free (app);
        }

        g_print ("\n");
        g_free (volume_bar);

        ports = mate_mixer_stream_list_ports (stream);
        while (ports) {
            MateMixerPort *port = MATE_MIXER_PORT (ports->data);

            g_print ("       |-| Port %s\n"
                     "                |-| Description : %s\n"
                     "                |-| Icon Name   : %s\n"
                     "                |-| Priority    : %lu\n"
                     "                |-| Status      : \n\n",
                     mate_mixer_port_get_name (port),
                     mate_mixer_port_get_description (port),
                     mate_mixer_port_get_icon (port),
                     mate_mixer_port_get_priority (port));

            ports = ports->next;
        }

        streams = streams->next;
    }
}

static void
connected (void)
{
    g_print ("Connected using the %s backend.\n\n",
             mate_mixer_control_get_backend_name (control));

    print_devices ();
    print_streams ();

    g_print ("Waiting for events. Hit CTRL+C to quit.\n");
}

static void
state_cb (void)
{
    MateMixerState state;

    state = mate_mixer_control_get_state (control);

    switch (state) {
    case MATE_MIXER_STATE_READY:
        connected ();
        break;
    case MATE_MIXER_STATE_FAILED:
        g_printerr ("Connection failed.\n");
        g_main_loop_quit (mainloop);
        break;
    default:
        break;
    }
}

#ifdef G_OS_UNIX
static gboolean
signal_cb (gpointer mainloop)
{
    g_idle_add ((GSourceFunc) g_main_loop_quit, mainloop);
    return FALSE;
}
#endif

int main (int argc, char *argv[])
{
    MateMixerState  state;
    GOptionContext *context;
    gchar          *backend = NULL;
    gchar          *server = NULL;
    GError         *error = NULL;

    GOptionEntry    entries[] = {
        { "backend", 'b', 0, G_OPTION_ARG_STRING, &backend, "Sound system to use (pulse, null)", NULL },
        { "server",  's', 0, G_OPTION_ARG_STRING, &server,  "Sound server address", NULL },
        { NULL }
    };

    /* Initialize the library, if the function returns FALSE, it is not usable */
    if (!mate_mixer_init ())
        return 1;

    context = g_option_context_new ("- libmatemixer monitor");

    g_option_context_add_main_entries (context, entries, NULL);

    if (!g_option_context_parse (context, &argc, &argv, &error)) {
        g_printerr ("%s\n", error->message);
        g_error_free (error);
        return 1;
    }

    setlocale (LC_ALL, "");

    /* Set up the controller, through which we access the main functionality */
    control = mate_mixer_control_new ();

    /* Some details about our application, only used with the PulseAudio backend */
    mate_mixer_control_set_app_name (control, "MateMixer Monitor");
    mate_mixer_control_set_app_id (control, "org.mate-desktop.libmatemixer-monitor");
    mate_mixer_control_set_app_version (control, "1.0");
    mate_mixer_control_set_app_icon (control, "multimedia-volume-control");

    if (backend) {
        if (!strcmp (backend, "pulse"))
            mate_mixer_control_set_backend_type (control, MATE_MIXER_BACKEND_PULSE);
        else if (!strcmp (backend, "null"))
            mate_mixer_control_set_backend_type (control, MATE_MIXER_BACKEND_NULL);
        else
            g_printerr ("Sound system backend '%s' is unknown, it will be auto-detected.",
                        backend);

        g_free (backend);
    }

    if (server) {
        mate_mixer_control_set_server_address (control, server);
        g_free (server);
    }

    /* Initiate connection to the sound server */
    if (!mate_mixer_control_open (control)) {
        g_object_unref (control);
        mate_mixer_deinit ();
        return 1;
    }

    /* If mate_mixer_control_open() returns TRUE, the state will be either
     * MATE_MIXER_STATE_READY or MATE_MIXER_STATE_CONNECTING.
     *
     * In case mate_mixer_control_open() returned FALSE, the current state
     * would be MATE_MIXER_STATE_FAILED */
    state = mate_mixer_control_get_state (control);

    switch (state) {
    case MATE_MIXER_STATE_READY:
        connected ();
        break;
    case MATE_MIXER_STATE_CONNECTING:
        g_print ("Waiting for connection...\n");

        /* Wait for the state transition */
        g_signal_connect (control,
                          "notify::state",
                          G_CALLBACK (state_cb),
                          NULL);
        break;
    default:
        g_assert_not_reached ();
        break;
    }

    mainloop = g_main_loop_new (NULL, FALSE);

#ifdef G_OS_UNIX
    g_unix_signal_add (SIGTERM, signal_cb, mainloop);
    g_unix_signal_add (SIGINT,  signal_cb, mainloop);
#endif

    g_main_loop_run (mainloop);

    g_object_unref (control);
    mate_mixer_deinit ();

    return 0;
}
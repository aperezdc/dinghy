/*
 * dy-sysfs-mode-monitor.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "dy-mode-monitor.h"
#include "dy-sysfs-mode-monitor.h"
#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>


#ifndef DY_SYSFS_MODE_MONITOR_RATE_LIMIT
#define DY_SYSFS_MODE_MONITOR_RATE_LIMIT 1000
#endif /* !DY_SYSFS_MODE_MONITOR_RATE_LIMIT */


struct _DySysfsModeMonitor
{
    GObject           parent;
    DyModeMonitorInfo mode_info;
    GFileMonitor     *filemon;
    GFile            *file;
    char             *path;  /* Caches result of g_file_get_path(). */
};


static const DyModeMonitorInfo*
dy_sysfs_mode_monitor_get_info (DyModeMonitor *monitor)
{
    g_assert (DY_IS_SYSFS_MODE_MONITOR (monitor));
    return &(DY_SYSFS_MODE_MONITOR (monitor)->mode_info);
}


static void
dy_mode_monitor_interface_init (DyModeMonitorInterface *iface)
{
    iface->get_info = dy_sysfs_mode_monitor_get_info;
}


G_DEFINE_TYPE_WITH_CODE (DySysfsModeMonitor,
                         dy_sysfs_mode_monitor,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (DY_TYPE_MODE_MONITOR,
                                                dy_mode_monitor_interface_init))


enum {
    PROP_0,
    PROP_MODE_ID,
    N_OVERRIDEN_PROPERTIES = PROP_MODE_ID,
    PROP_PATH,
    N_PROPERTIES,
};

static GParamSpec *s_properties[N_PROPERTIES] = { NULL, };


static void
dy_sysfs_mode_monitor_get_property (GObject    *object,
                                    unsigned    prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
    DySysfsModeMonitor *monitor = DY_SYSFS_MODE_MONITOR (object);
    switch (prop_id) {
        case PROP_MODE_ID:
            g_value_set_string (value, monitor->mode_info.mode_id);
            break;
        case PROP_PATH:
            g_value_set_string (value, dy_sysfs_mode_monitor_get_path (monitor));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
dy_sysfs_mode_monitor_dispose (GObject *object)
{
    DySysfsModeMonitor *monitor = DY_SYSFS_MODE_MONITOR (object);

    g_clear_object (&monitor->filemon);
    g_clear_object (&monitor->file);
    g_clear_pointer (&monitor->path, g_free);
    g_clear_pointer (&monitor->mode_info.mode_id, g_free);

    G_OBJECT_CLASS (dy_sysfs_mode_monitor_parent_class)->dispose (object);
}


static void
dy_sysfs_mode_monitor_class_init (DySysfsModeMonitorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = dy_sysfs_mode_monitor_dispose;
    object_class->get_property = dy_sysfs_mode_monitor_get_property;

    DyModeMonitorInterface *mmon_iface =
        g_type_default_interface_ref (DY_TYPE_MODE_MONITOR);

    s_properties[PROP_PATH] =
        g_param_spec_string ("path",
                             "SysFS Path",
                             "SysFS path to the device being monitored",
                             NULL,
                             G_PARAM_READABLE |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class,
                                       N_PROPERTIES - N_OVERRIDEN_PROPERTIES,
                                       s_properties + N_OVERRIDEN_PROPERTIES);

    s_properties[PROP_MODE_ID] =
        g_object_interface_find_property (mmon_iface, "mode-id");
    g_object_class_override_property (object_class,
                                      PROP_MODE_ID,
                                      "mode-id");

    g_type_default_interface_unref (mmon_iface);
}


static void
dy_sysfs_mode_monitor_init (DySysfsModeMonitor *monitor)
{
}


static inline void
dy_sysfs_mode_monitor_fill_info_from_mode_id (DyModeMonitorInfo *info)
{
    char leading_char, lacing;
    uint32_t refresh_rate;

    info->width = info->height = 0;
    sscanf (info->mode_id,
            "%c:%" SCNu32 "x%" SCNu32 "%c-%" PRIu32,
            &leading_char,
            &info->width,
            &info->height,
            &lacing,
            &refresh_rate);
}


static gboolean
dy_sysfs_mode_monitor_read_mode_sync (DySysfsModeMonitor *monitor,
                                      GError            **error)
{
    g_assert_nonnull (monitor);

    g_autoptr(GInputStream) stream =
        G_INPUT_STREAM (g_file_read (monitor->file, NULL, error));
    if (!stream)
        return FALSE;

    g_autoptr(GDataInputStream) datain = g_data_input_stream_new (stream);
    g_data_input_stream_set_newline_type (datain, G_DATA_STREAM_NEWLINE_TYPE_LF);

    size_t line_len = 0;
    g_autoptr(GError) read_error = NULL;
    g_autofree char *line = g_data_input_stream_read_line (datain,
                                                           &line_len,
                                                           NULL,
                                                           &read_error);
    gboolean success = TRUE;
    if (!line && read_error) {
        if (error)
            *error = g_steal_pointer (&read_error);
        success = FALSE;
    }

    g_debug ("Monitor [%s] mode: %s -> %s",
             monitor->path, monitor->mode_info.mode_id, line);

    if (g_strcmp0 (monitor->mode_info.mode_id, line)) {
        /* Value has changed. Update and notify. */
        g_clear_pointer (&monitor->mode_info.mode_id, g_free);
        monitor->mode_info.mode_id = g_steal_pointer (&line);
        dy_sysfs_mode_monitor_fill_info_from_mode_id (&monitor->mode_info);
        g_object_notify_by_pspec (G_OBJECT (monitor), s_properties[PROP_MODE_ID]);
    }

    return success;
}


static void
on_file_monitor_changed (GFileMonitor       *filemon,
                         GFile              *file,
                         GFile              *other_file,
                         GFileMonitorEvent   event,
                         DySysfsModeMonitor *monitor)
{
    if (event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT) {
        g_autoptr(GError) error = NULL;
        if (!dy_sysfs_mode_monitor_read_mode_sync (monitor, &error)) {
            g_warning ("Cannot read '%s': %s",
                       dy_sysfs_mode_monitor_get_path (monitor),
                       error->message);
        }
    }
}


DySysfsModeMonitor*
dy_sysfs_mode_monitor_new (GFile   *file,
                           GError **error)
{
    g_return_val_if_fail (file != NULL, NULL);

    g_autoptr(DySysfsModeMonitor) monitor =
        g_object_new (DY_TYPE_SYSFS_MODE_MONITOR, NULL);
    monitor->file = g_object_ref_sink (file);
    monitor->path = g_file_get_path (file);

    if (g_access (monitor->path, R_OK)) {
        g_set_error (error,
                     G_FILE_ERROR,
                     g_file_error_from_errno (errno),
                     "%s", monitor->path);
        return NULL;
    }

    /*
     * Do not emit property updates while reading the initial mode and
     * setting up the file monitor object, so client code doesn't get
     * spurious property change notifications.
     */
    g_object_freeze_notify (G_OBJECT (monitor));

    if (!dy_sysfs_mode_monitor_read_mode_sync (monitor, error) ||
        !(monitor->filemon = g_file_monitor_file (file,
                                                  G_FILE_MONITOR_NONE,
                                                  NULL,
                                                  error)))
        return NULL;

    g_file_monitor_set_rate_limit (monitor->filemon,
                                   DY_SYSFS_MODE_MONITOR_RATE_LIMIT);


    g_signal_connect (monitor->filemon,
                      "changed",
                      G_CALLBACK (on_file_monitor_changed),
                      monitor);

    g_object_thaw_notify (G_OBJECT (monitor));
    return g_steal_pointer (&monitor);
}


const char*
dy_sysfs_mode_monitor_get_path (DySysfsModeMonitor *monitor)
{
    g_return_val_if_fail (monitor != NULL, NULL);
    return monitor->path;
}


#ifdef DY_SYSFS_MODE_MONITOR_TEST_MAIN
#include <string.h>

static void
on_monitor_mode_changed (DyModeMonitor *monitor,
                         GParamSpec    *pspec,
                         void          *user_data)
{
    const DyModeMonitorInfo *info = dy_mode_monitor_get_info (monitor);
    g_print ("Monitor [%s] mode %" PRIu32 "x%" PRIu32 " (%s)\n",
             dy_sysfs_mode_monitor_get_path (DY_SYSFS_MODE_MONITOR (monitor)),
             info->width, info->height, info->mode_id);
}

int
main (int argc, char **argv)
{
    const char *slash = strrchr (argv[0], '/');
    g_set_prgname (slash ? slash + 1 : argv[0]);

    if (argc != 2) {
        g_printerr ("Usage: %s PATH\n", g_get_prgname ());
        return 1;
    }

    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = g_file_new_for_commandline_arg (argv[1]);
    g_autoptr(DySysfsModeMonitor) monitor = dy_sysfs_mode_monitor_new (file, &error);
    if (!monitor) {
        g_autofree char *path = g_file_get_path (file);
        g_printerr ("%s: Cannot monitor '%s': %s", g_get_prgname (), path, error->message);
        return 2;
    }
    g_clear_object (&file);

    g_signal_connect (monitor, "notify::mode-id",
                      G_CALLBACK (on_monitor_mode_changed), NULL);
    g_autoptr(GMainLoop) mainloop = g_main_loop_new (NULL, FALSE);
    g_main_loop_run (mainloop);
    return 0;
}
#endif

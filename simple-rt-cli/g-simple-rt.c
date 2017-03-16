/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * SimpleRT: Reverse tethering utility for Android
 *
 * Copyright (C) 2014-2017 Gary Bisson <bisson.gary@gmail.com>
 *   For the linux-adk bits, see:
 *   https://github.com/gibsson/linux-adk.
 *
 * Copyright (C) 2016-2017 Konstantin Menyaev <konstantin.menyaev@gmail.com>
 *   For the original SimpleRT implementation and logic, see:
 *   https://github.com/vvviperrr/SimpleRT
 *
 * Copyright (C) 2017 Zodiac Inflight Innovations
 * Copyright (C) 2017 Aleksander Morgado <aleksander@aleksander.es>
 *   For the GLib/GUdev based port, see:
 *   https://github.com/aleksander0m/SimpleRT
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <netinet/ip.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>

#include <libusb.h>

#include <glib.h>
#include <glib-unix.h>

#include <gudev/gudev.h>

/* Android Open Accessory protocol defines */
#define AOA_GET_PROTOCOL            51
#define AOA_SEND_IDENT              52
#define AOA_START_ACCESSORY         53
#define AOA_REGISTER_HID            54
#define AOA_UNREGISTER_HID          55
#define AOA_SET_HID_REPORT_DESC     56
#define AOA_SEND_HID_EVENT          57
#define AOA_AUDIO_SUPPORT           58

/* String IDs */
#define AOA_STRING_MAN_ID           0
#define AOA_STRING_MOD_ID           1
#define AOA_STRING_DSC_ID           2
#define AOA_STRING_VER_ID           3
#define AOA_STRING_URL_ID           4
#define AOA_STRING_SER_ID           5

/* Product IDs / Vendor IDs */
#define AOA_ACCESSORY_VID           0x18D1	/* Google */
#define AOA_ACCESSORY_PID           0x2D00	/* accessory */
#define AOA_ACCESSORY_ADB_PID       0x2D01	/* accessory + adb */
#define AOA_AUDIO_PID               0x2D02	/* audio */
#define AOA_AUDIO_ADB_PID           0x2D03	/* audio + adb */
#define AOA_ACCESSORY_AUDIO_PID     0x2D04	/* accessory + audio */
#define AOA_ACCESSORY_AUDIO_ADB_PID 0x2D05	/* accessory + audio + adb */

static const guint16 aoa_pids[] = {
    AOA_ACCESSORY_PID,
    AOA_ACCESSORY_ADB_PID,
    AOA_AUDIO_PID,
    AOA_AUDIO_ADB_PID,
    AOA_ACCESSORY_AUDIO_PID,
    AOA_ACCESSORY_AUDIO_ADB_PID
};

/* Endpoint Addresses TODO get from interface descriptor */
#define AOA_ACCESSORY_EP_IN  0x81
#define AOA_ACCESSORY_EP_OUT 0x02

static const char *default_manufacturer = "The SimpleRT developers";
static const char *default_model        = "gSimpleRT";
static const char *default_description  = "Simple Reverse Tethering";
static const char *default_version      = "1.0";
static const char *default_url          = "https://github.com/aleksander0m/SimpleRT";

/******************************************************************************/
typedef enum {
    ACTION_TETHERING,
    ACTION_RESET,
} Action;

typedef struct {
    Action          action;
    guint16         vid;
    guint16         pid;
    gchar          *interface;
    GMainLoop      *loop;
    GUdevClient    *udev;
    GList          *tracked_devices;
    libusb_context *usb_context;
    guint8          next_subnet;
    GHashTable     *subnets;
} Context;

typedef struct {
    Context  *context;
    guint16   vid;
    guint16   pid;
    guint     busnum;
    guint     devnum;
    gchar    *sysfs_path;
    gboolean  aoa;
    guint     timeout_id;

    libusb_device        *usb_device;
    libusb_device_handle *usb_handle;

    guint8 subnet;

    gchar tun_name[IFNAMSIZ];
    gint  tun_fd;

    GMutex    mutex;
    gboolean  halt;
    GThread  *conn_thread;
    GThread  *tun_thread;
    GThread  *acc_thread;
} Device;

static void
device_free (Device *device)
{
    if (device->timeout_id)
        g_source_remove (device->timeout_id);
    if (device->usb_device)
        libusb_unref_device (device->usb_device);
    g_free (device->sysfs_path);
    g_slice_free (Device, device);
}

/******************************************************************************/
/* Subnet management */

static guint
select_subnet (Context     *context,
               const gchar *sysfs_path)
{
    guint val;

    val = GPOINTER_TO_UINT (g_hash_table_lookup (context->subnets, sysfs_path));
    if (!val) {
        val = context->next_subnet++;
        if (val == 0)
            g_printerr ("error: too many subnets!\n");
        else {
            g_print ("subnet mapping added: %s --> 10.11.%u.0\n", sysfs_path, val);
            g_hash_table_insert (context->subnets, g_strdup (sysfs_path), GUINT_TO_POINTER (val));
        }
    }
    return val;
}

/******************************************************************************/
/* Tethering */

#define ACC_BUFFER_SIZE 4096
#define ACC_TIMEOUT     200

static void *
tun_thread_func (Device *device)
{
    gsize nread;
    gint ret;
    gint transferred;
    guint8 acc_buf[ACC_BUFFER_SIZE];

    while (1) {
        gint           status;
        fd_set         rfds;
        struct timeval tv;
        gboolean       halt_thread;

        g_mutex_lock (&device->mutex);
        halt_thread = device->halt;
        g_mutex_unlock (&device->mutex);

        if (halt_thread)
            break;

        FD_ZERO (&rfds);
        FD_SET  (device->tun_fd, &rfds);

        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        if ((status = select (device->tun_fd + 1, &rfds, NULL, NULL, &tv)) < 0) {
            if (errno == EINTR)
                continue;
            g_printerr ("error: waiting to write: %s", g_strerror (errno));
            break;
        }

        if (status == 0)
            continue;

        nread = read (device->tun_fd, acc_buf, sizeof (acc_buf));
        if (nread > 0) {
            if ((ret = libusb_bulk_transfer (device->usb_handle,
                                             AOA_ACCESSORY_EP_OUT,
                                             acc_buf,
                                             nread,
                                             &transferred,
                                             ACC_TIMEOUT)) < 0) {
                if (ret == LIBUSB_ERROR_TIMEOUT)
                    continue;
                g_printerr ("error: bulk transfer failed: %s\n", libusb_strerror (ret));
                break;
            }
            continue;
        }

        if (nread < 0) {
            g_printerr ("error: couldn't read from TUN device: %s\n", g_strerror (errno));
            break;
        }

        /* EOF received */
        break;
    }

    g_mutex_lock (&device->mutex);
    device->halt = TRUE;
    g_mutex_unlock (&device->mutex);
    return NULL;
}

static void *
acc_thread_func (Device *device)
{
    gint ret;
    gint transferred;
    guint8 acc_buf[ACC_BUFFER_SIZE];

    while (1) {
        gboolean halt_thread;

        g_mutex_lock (&device->mutex);
        halt_thread = device->halt;
        g_mutex_unlock (&device->mutex);

        if (halt_thread)
            break;

        if ((ret = libusb_bulk_transfer (device->usb_handle,
                                         AOA_ACCESSORY_EP_IN,
                                         acc_buf,
                                         sizeof (acc_buf),
                                         &transferred,
                                         ACC_TIMEOUT)) < 0) {
            if (ret == LIBUSB_ERROR_TIMEOUT)
                continue;

            g_printerr ("error: bulk transfer error: %s\n", libusb_strerror (ret));
            break;
        }

        if (write (device->tun_fd, acc_buf, transferred) < 0) {
            g_printerr ("error: couldn't write to TUN device: %s\n", g_strerror (errno));
            break;
        }
    }

    g_mutex_lock (&device->mutex);
    device->halt = TRUE;
    g_mutex_unlock (&device->mutex);
    return NULL;
}

static void *
conn_thread_func (Device *device)
{
    static const gchar *clonedev = "/dev/net/tun";
    struct ifreq        ifr;
    gchar              *cmd = NULL;
    gchar              *network;
    gchar              *host_address;
    gint                ret;

    device->timeout_id = 0;

    if ((device->tun_fd = open (clonedev, O_RDWR)) < 0 ) {
        g_printerr ("error: couldn't open TUN clone device: %s", g_strerror (errno));
        goto out;
    }

    memset(&ifr, 0, sizeof (ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

    if (ioctl (device->tun_fd, TUNSETIFF, (void *) &ifr) < 0) {
        close (device->tun_fd);
        device->tun_fd = 0;
        g_printerr ("error: couldn't create TUN device: %s", g_strerror (errno));
        goto out;
    }

    strncpy (device->tun_name, ifr.ifr_name, sizeof (device->tun_name) - 1);

    network      = g_strdup_printf ("10.11.%u.0", device->subnet);
    host_address = g_strdup_printf ("10.11.%u.1", device->subnet);

    cmd = g_strdup_printf ("g-simple-rt-iface-up.sh linux %s %s %s 30 %s\n",
                           device->tun_name,
                           device->context->interface,
                           network,
                           host_address);
    if (system (cmd) != 0) {
        fprintf(stderr, "error: unable set iface %s up\n", device->tun_name);
        goto out;
    }

    /* Trying to open supplied device */
    if ((ret = libusb_open (device->usb_device, &device->usb_handle)) < 0) {
        g_printerr ("error: unable to open device: %s\n", libusb_strerror (ret));
        goto out;
    }

    /* Claiming first (accessory) interface from the opened device */
    if ((ret = libusb_claim_interface (device->usb_handle, 0)) < 0) {
        g_printerr ("error: couldn't claim interface: %s\n", libusb_strerror (ret));
        goto out;
    }

    device->tun_thread = g_thread_new (NULL, (GThreadFunc) tun_thread_func, device);
    device->acc_thread = g_thread_new (NULL, (GThreadFunc) acc_thread_func, device);

    /* Wait for children to exit themselves */
    g_clear_pointer (&device->tun_thread, g_thread_join);
    g_clear_pointer (&device->acc_thread, g_thread_join);

out:
    if (device->tun_fd) {
        close (device->tun_fd);
        device->tun_fd = 0;
    }

    if (device->usb_handle != NULL) {
        libusb_release_interface (device->usb_handle, 0);
        libusb_close (device->usb_handle);
        device->usb_handle = NULL;
    }

    g_free (network);
    g_free (host_address);
    g_free (cmd);
    return NULL;
}

static gboolean
device_setup_tethering (Device *device)
{
    device->subnet = select_subnet (device->context, device->sysfs_path);
    if (device->subnet != 0)
        device->conn_thread = g_thread_new (NULL, (GThreadFunc) conn_thread_func, device);

    return G_SOURCE_REMOVE;
}

/******************************************************************************/
/* USB device processing */

#define TIMEOUT_AFTER_PROTOCOL_PROBE_MS 10

static gboolean
device_setup_aoa (Device *device)
{
    gint   ret;
    gchar *device_address;

    device->timeout_id = 0;

    device->subnet = select_subnet (device->context, device->sysfs_path);
    if (device->subnet == 0) {
        g_printerr ("[%03o,%03o] subnet allocation failed\n", device->busnum, device->devnum);
        goto out;
    }

    device_address = g_strdup_printf ("10.11.%u.2", device->subnet);
    g_print ("[%03o,%03o] subnet allocated: 10.11.%u.0\n",
             device->busnum, device->devnum, device->subnet);

    g_print ("[%03o,%03o] sending manufacturer: %s\n", device->busnum, device->devnum, default_manufacturer);
    if ((ret = libusb_control_transfer (device->usb_handle,
                                        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
                                        AOA_SEND_IDENT,
                                        0,
                                        AOA_STRING_MAN_ID,
                                        (uint8_t *) default_manufacturer,
                                        strlen (default_manufacturer) + 1,
                                        0)) < 0)
        goto out;

    g_print ("[%03o,%03o] sending model: %s\n", device->busnum, device->devnum, default_model);
    if ((ret = libusb_control_transfer (device->usb_handle,
                                        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
                                        AOA_SEND_IDENT,
                                        0,
                                        AOA_STRING_MOD_ID,
                                        (uint8_t *) default_model,
                                        strlen (default_model) + 1,
                                        0)) < 0)
        goto out;

    g_print ("[%03o,%03o] sending description: %s\n", device->busnum, device->devnum, default_description);
    if ((ret = libusb_control_transfer (device->usb_handle,
                                        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
                                        AOA_SEND_IDENT,
                                        0,
                                        AOA_STRING_DSC_ID,
                                        (uint8_t *) default_description,
                                        strlen (default_description) + 1,
                                        0)) < 0)
        goto out;

    g_print ("[%03o,%03o] sending version: %s\n", device->busnum, device->devnum, default_version);
    if ((ret = libusb_control_transfer (device->usb_handle,
                                        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
                                        AOA_SEND_IDENT,
                                        0,
                                        AOA_STRING_VER_ID,
                                        (uint8_t *) default_version,
                                        strlen (default_version) + 1,
                                        0)) < 0)
        goto out;

    g_print ("[%03o,%03o] sending url: %s\n", device->busnum, device->devnum, default_url);
    if ((ret = libusb_control_transfer (device->usb_handle,
                                        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
                                        AOA_SEND_IDENT,
                                        0,
                                        AOA_STRING_URL_ID,
                                        (uint8_t *) default_url,
                                        strlen (default_url) + 1,
                                        0)) < 0)
        goto out;

    g_print ("[%03o,%03o] sending serial: %s\n", device->busnum, device->devnum, device_address);
    if ((ret = libusb_control_transfer (device->usb_handle,
                                        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
                                        AOA_SEND_IDENT,
                                        0,
                                        AOA_STRING_SER_ID,
                                        (uint8_t *) device_address,
                                        strlen (device_address) + 1,
                                        0)) < 0)
        goto out;

    g_print ("[%03o,%03o] switching device into accessory mode...\n", device->busnum, device->devnum);
    if ((ret = libusb_control_transfer (device->usb_handle,
                                        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
                                        AOA_START_ACCESSORY,
                                        0,
                                        0,
                                        NULL,
                                        0,
                                        0)) < 0)
        goto out;

    g_print ("[%03o,%03o] switch requested\n", device->busnum, device->devnum);

out:
    if (ret < 0)
        g_printerr ("[%03o,%03o] accessory initialization failed: %s\n", device->busnum, device->devnum, libusb_strerror (ret));

    g_clear_pointer (&device->usb_handle, (GDestroyNotify) libusb_close);
    g_free (device_address);

    return G_SOURCE_REMOVE;
}

static gboolean
device_probe_aoa (Device *device)
{
    gint    ret;
    guint16 aoa_version;

    g_print ("[%03o,%03o] checking AOA support...\n", device->busnum, device->devnum);

    /* Trying to open supplied device */
    if ((ret = libusb_open (device->usb_device, &device->usb_handle)) < 0) {
        g_printerr ("error: unable to open device: %s\n", libusb_strerror (ret));
        return FALSE;
    }

    /* Check whether a kernel driver is attached. If so, we'll need to detach it. */
    if (libusb_kernel_driver_active (device->usb_handle, 0)) {
        g_print ("[%03o,%03o] detaching kernel driver...\n", device->busnum, device->devnum);
        if ((ret = libusb_detach_kernel_driver (device->usb_handle, 0)) < 0) {
            g_printerr ("error: couldn't detach kernel driver: %s\n", libusb_strerror (ret));
            return FALSE;
        }
        g_print ("[%03o,%03o] kernel driver detached...\n", device->busnum, device->devnum);
    }

    /* Now ask if device supports AOA protocol */
    if ((ret = libusb_control_transfer (device->usb_handle,
                                        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR,
                                        AOA_GET_PROTOCOL,
                                        0,
                                        0,
                                        (uint8_t *) &aoa_version,
                                        sizeof (aoa_version),
                                        0)) < 0) {
        g_printerr ("error: AOA probing failed: %s\n", libusb_strerror (ret));
        return FALSE;
    }

    aoa_version = GUINT16_FROM_LE (aoa_version);
    g_print ("[%03o,%03o] device supports AOA %" G_GUINT16_FORMAT "\n", device->busnum, device->devnum, aoa_version);

    return TRUE;
}

/******************************************************************************/
/* Find libusb_device */

static libusb_device *
find_usb_device (libusb_context *usb_context,
                 guint           busnum,
                 guint           devnum)
{
    libusb_device  *found_device = NULL;
    libusb_device **devices = NULL;
    ssize_t         n_devices = 0;
    unsigned int    i;

    n_devices = libusb_get_device_list (usb_context, &devices);
    if (!n_devices || !devices) {
        g_printerr ("error: libusb device enumeration failed\n");
        return NULL;
    }

    /* Go over the devices and find the one we want */
    for (i = 0; !found_device && i < n_devices; i++) {
        if (libusb_get_bus_number (devices[i]) == busnum &&
            libusb_get_device_address (devices[i]) == devnum)
            found_device = libusb_ref_device (devices[i]);
    }

    libusb_free_device_list (devices, 1);

    if (!found_device)
        g_printerr ("error: libusb device not found\n");

    return found_device;
}

/******************************************************************************/
/* Device tracking/untracking */

static GList *
find_device (Context     *context,
             const gchar *sysfs_path)
{
    GList *l;

    for (l = context->tracked_devices; l; l = g_list_next (l)) {
        if (g_strcmp0 (((Device *) (l->data))->sysfs_path, sysfs_path) == 0) {
            return l;
        }
    }
    return NULL;
}

static void
untrack_device (Context     *context,
                const gchar *sysfs_path)
{
    GList  *l;
    Device *device;

    l = find_device (context, sysfs_path);
    if (!l)
        return;

    device = (Device *) (l->data);
    g_print ("device: 0x%04x:0x%04x [%03u:%03u]: untracked (%s)\n",
             device->vid, device->pid, device->busnum, device->devnum, device->aoa ? "Android Open Accessory" : "candidate");
    device_free (device);

    context->tracked_devices = g_list_delete_link (context->tracked_devices, l);
}

static void
track_device (Context     *context,
              gboolean     aoa_device,
              const gchar *sysfs_path,
              guint16      vid,
              guint16      pid,
              guint        busnum,
              guint        devnum)
{
    Device *device;

    if (find_device (context, sysfs_path)) {
        fprintf (stderr, "error: device already tracked\n");
        return;
    }

    device = g_slice_new0 (Device);
    device->context = context;
    device->sysfs_path = g_strdup (sysfs_path);
    device->vid = vid;
    device->pid = pid;
    device->busnum = busnum;
    device->devnum = devnum;
    device->aoa = aoa_device;

    device->usb_device = find_usb_device (context->usb_context, busnum, devnum);
    if (!device->usb_device) {
        device_free (device);
        return;
    }

    /* check AOA support before tracking */
    if (!device->aoa) {
        if (!device_probe_aoa (device)) {
            device_free (device);
            return;
        }

        /* Schedule switch to AOA */
        device->timeout_id = g_timeout_add (TIMEOUT_AFTER_PROTOCOL_PROBE_MS, (GSourceFunc) device_setup_aoa, device);
    } else {
        /* Schedule tethering start */
        device->timeout_id = g_timeout_add (TIMEOUT_AFTER_PROTOCOL_PROBE_MS, (GSourceFunc) device_setup_tethering, device);
    }

    /* track */
    context->tracked_devices = g_list_prepend (context->tracked_devices, device);

    g_print ("device: 0x%04x:0x%04x [%03u:%03u]: tracked (%s)\n",
             device->vid, device->pid, device->busnum, device->devnum, device->aoa ? "Android Open Accessory" : "candidate");
}

/******************************************************************************/
/* Udev monitoring */

static void
device_added (Context     *context,
              GUdevDevice *device)
{
    const gchar *aux;
    const gchar *sysfs_path;
    gulong vid = 0, pid = 0, busnum = 0, devnum = 0;

    if ((aux = g_udev_device_get_sysfs_attr (device, "idVendor")) != NULL)
        vid = strtoul (aux, NULL, 16);
    if ((aux = g_udev_device_get_sysfs_attr (device, "idProduct")) != NULL)
        pid = strtoul (aux, NULL, 16);
    if (vid == 0 || pid == 0)
        return;

    if ((aux = g_udev_device_get_sysfs_attr (device, "busnum")) != NULL)
        busnum = strtoul (aux, NULL, 10);
    if ((aux = g_udev_device_get_sysfs_attr (device, "devnum")) != NULL)
        devnum = strtoul (aux, NULL, 10);
    if (busnum == 0 || devnum == 0)
        return;

    if ((sysfs_path = g_udev_device_get_sysfs_path (device)) == NULL)
        return;

    /* Default USB device? */
    if (vid == context->vid && (pid == context->pid || context->pid == 0))
        track_device (context, FALSE, sysfs_path, vid, pid, busnum, devnum);

    /* AOA device already? */
    if (vid == AOA_ACCESSORY_VID) {
        guint i;

        for (i = 0; i < G_N_ELEMENTS (aoa_pids); i++) {
            if (pid == aoa_pids[i]) {
                track_device (context, TRUE, sysfs_path, vid, pid, busnum, devnum);
                break;
            }
        }
    }
}

static void
device_removed (Context     *context,
                GUdevDevice *device)
{
    const gchar *sysfs_path;

    sysfs_path = g_udev_device_get_sysfs_path (device);
    if (!sysfs_path)
        return;

    untrack_device (context, sysfs_path);
}

static void
handle_uevent (GUdevClient *client,
               const char  *action,
               GUdevDevice *device,
               Context     *context)
{
    g_print ("uevent: %s %s\n", action, g_udev_device_get_sysfs_path (device));

    if (g_strcmp0 (action, "add") == 0) {
        device_added (context, device);
        return;
    }

    if (g_strcmp0 (action, "remove") == 0) {
        device_removed (context, device);
        return;
    }
}

static void
initial_list_tethering (Context *context)
{
    GList *devices, *l;

    devices = g_udev_client_query_by_subsystem (context->udev, "usb");
    for (l = devices; l; l = g_list_next (l)) {
        GUdevDevice *device = G_UDEV_DEVICE (l->data);

        device_added (context, device);
        g_object_unref (device);
    }
}

/******************************************************************************/

static gboolean
reset_device (guint busnum, guint devnum)
{
    gint      fd;
	gchar    *path;
    gboolean  reseted = FALSE;

    path = g_strdup_printf ("/dev/bus/usb/%03u/%03u", busnum, devnum);

	fd = open (path, O_WRONLY);
	if (fd > -1) {
		if (ioctl (fd, USBDEVFS_RESET, 0) < 0 && errno != ENODEV)
			g_printerr ("failed reseting device [%03u,%03u]: %s\n", busnum, devnum, g_strerror (errno));
		else {
            reseted = TRUE;
			g_print ("reset device [%03u,%03u]: done\n", busnum, devnum);
        }
		close (fd);
	} else
        g_printerr ("error: cannot open %s: %s\n", path, g_strerror (errno));

    g_free (path);

    return reseted;
}

static void
initial_list_reset (Context *context)
{
    GList *devices, *l;
    guint n_resets = 0;

    devices = g_udev_client_query_by_subsystem (context->udev, "usb");
    for (l = devices; l; l = g_list_next (l)) {
        GUdevDevice *device = G_UDEV_DEVICE (l->data);
        const gchar *aux;
        gulong vid = 0, pid = 0, busnum = 0, devnum = 0;
        guint  i;

        /* Validate AOA VID */
        if ((aux = g_udev_device_get_sysfs_attr (device, "idVendor")) != NULL)
            vid = strtoul (aux, NULL, 16);
        if (vid != AOA_ACCESSORY_VID)
            continue;

        /* Validate AOA PID */
        if ((aux = g_udev_device_get_sysfs_attr (device, "idProduct")) != NULL)
            pid = strtoul (aux, NULL, 16);
        for (i = 0; i < G_N_ELEMENTS (aoa_pids); i++) {
            if (pid == aoa_pids[i])
                break;
        }
        if (i == G_N_ELEMENTS (aoa_pids))
            continue;

        /* Get busnum:devnum */
        if ((aux = g_udev_device_get_sysfs_attr (device, "busnum")) != NULL)
            busnum = strtoul (aux, NULL, 10);
        if ((aux = g_udev_device_get_sysfs_attr (device, "devnum")) != NULL)
            devnum = strtoul (aux, NULL, 10);
        if (busnum == 0 || devnum == 0)
            continue;

        /* Run reset */
        if (reset_device (busnum, devnum))
            n_resets++;

        g_object_unref (device);
    }

    if (!n_resets)
        g_printerr ("error: no AOA devices were reseted\n");
    else
        g_print ("success: a total of %u AOA devices were reseted\n", n_resets);
}

/******************************************************************************/

/* General context */
static gchar    *vid_str;
static gchar    *pid_str;
static gchar    *interface_str;
static gboolean  reset_flag;
static gboolean  version_flag;
static gboolean  help_flag;

static gboolean
quit_cb (Context *context)
{
    GList *l;

    for (l = context->tracked_devices; l; l = g_list_next (l)) {
        Device *device;

        device = (Device *)(l->data);
        g_mutex_lock (&device->mutex);
        device->halt = TRUE;
        g_mutex_unlock (&device->mutex);
    }

    g_idle_add ((GSourceFunc) g_main_loop_quit, context->loop);

    return G_SOURCE_CONTINUE;
}

static GOptionEntry tethering_entries[] = {
    { "vid", 'v', 0, G_OPTION_ARG_STRING, &vid_str,
      "Device USB vendor ID (mandatory)",
      "[VID]"
    },
    { "pid", 'p', 0, G_OPTION_ARG_STRING, &pid_str,
      "Device USB product ID (optional)",
      "[PID]"
    },
    { "interface", 'i', 0, G_OPTION_ARG_STRING, &interface_str,
      "Network interface (mandatory)",
      "[IFACE]"
    },
    { NULL }
};

static GOptionEntry reset_entries[] = {
    { "reset", 'r', 0, G_OPTION_ARG_NONE, &reset_flag,
      "Reset AOA devices",
      NULL
    },
    { NULL }
};

static GOptionEntry main_entries[] = {
    { "version", 'V', 0, G_OPTION_ARG_NONE, &version_flag,
      "Print version",
      NULL
    },
    { "help", 'h', 0, G_OPTION_ARG_NONE, &help_flag,
      "Show help.",
      NULL
    },
    { NULL }
};

static void
print_version_and_exit (void)
{
    g_print ("\n"
             PACKAGE_NAME " " PACKAGE_VERSION "\n"
             "Copyright (C) 2016-2017 Konstantin Menyaev\n"
             "Copyright (C) 2017 Zodiac Inflight Innovations\n"
             "Copyright (C) 2017 Aleksander Morgado\n"
             "\n");
    exit (EXIT_SUCCESS);
}

static void
print_help_and_exit (GOptionContext *context)
{
    gchar *str;

    /* Always print --help-all */
    str = g_option_context_get_help (context, FALSE, NULL);
    g_print ("%s", str);
    g_free (str);
    exit (EXIT_SUCCESS);
}

static void
process_input_args (int argc, char **argv, Context *context)
{
    GOptionContext *option_context;
    GOptionGroup   *group;
    gulong          aux;

    /* Setup option context, process it and destroy it */
    option_context = g_option_context_new ("- Reverse tethering");

    group = g_option_group_new ("tethering", "Tethering options", "", NULL, NULL);
    g_option_group_add_entries (group, tethering_entries);
    g_option_context_add_group (option_context, group);

    group = g_option_group_new ("reset", "Reset options", "", NULL, NULL);
    g_option_group_add_entries (group, reset_entries);
    g_option_context_add_group (option_context, group);

    g_option_context_add_main_entries (option_context, main_entries, NULL);
    g_option_context_set_help_enabled (option_context, FALSE);
    g_option_context_parse (option_context, &argc, &argv, NULL);

    if (version_flag)
        print_version_and_exit ();

    if (help_flag)
        print_help_and_exit (option_context);

    /* Setup action */
    context->action = (reset_flag ? ACTION_RESET : ACTION_TETHERING);

    /* Validate options in tethering mode */
    if (context->action == ACTION_TETHERING) {
        if (!vid_str) {
            g_printerr ("error: --vid is mandatory\n");
            exit (EXIT_FAILURE);
        }

        aux = strtoul (vid_str, NULL, 16);
        if (aux == 0 || aux > G_MAXUINT16) {
            g_printerr ("error: invalid --vid value given: '%s'\n", vid_str);
            exit (EXIT_FAILURE);
        }
        context->vid = (guint16) aux;

        if (pid_str) {
            aux = strtoul (pid_str, NULL, 16);
            if (aux == 0 || aux > G_MAXUINT16) {
                g_printerr ("error: invalid --pid value given: '%s'\n", pid_str);
                exit (EXIT_FAILURE);
            }
            context->pid = (guint16) aux;
        }

        if (!interface_str) {
            g_printerr ("error: --interface is mandatory\n");
            exit (EXIT_FAILURE);
        }
        context->interface = g_strdup (interface_str);
    }

    /* Validate options in reset mode */
    if (context->action == ACTION_RESET) {
        if (vid_str)
            g_printerr ("warning: --vid is ignored when using --reset\n");
        if (pid_str)
            g_printerr ("warning: --pid is ignored when using --reset\n");
        if (interface_str)
            g_printerr ("warning: --interface is ignored when using --reset\n");
    }

    g_option_context_free (option_context);
}

int main (int argc, char **argv)
{
    static const gchar *subsystems[] = { "usb/usb_device", NULL };
    Context             context;

    /* Setup application context */
    memset (&context, 0, sizeof (context));
    libusb_init (&context.usb_context);
    context.subnets = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    context.next_subnet = 1;

    /* Process input options */
    process_input_args (argc, argv, &context);

    /* Tethering action */
    if (context.action == ACTION_TETHERING) {
        /* Clean exit handlers */
        g_unix_signal_add (SIGINT,  (GSourceFunc) quit_cb, &context);
        g_unix_signal_add (SIGTERM, (GSourceFunc) quit_cb, &context);
        g_unix_signal_add (SIGHUP,  (GSourceFunc) quit_cb, &context);

        /* Setup udev monitoring for any kind of usb device */
        context.udev = g_udev_client_new ((const gchar * const *) subsystems);
        g_signal_connect (context.udev, "uevent", G_CALLBACK (handle_uevent), &context);
        initial_list_tethering (&context);

        /* Run loop */
        context.loop = g_main_loop_new (NULL, FALSE);
        g_main_loop_run (context.loop);
        g_main_loop_unref (context.loop);
        goto out;
    }

    /* Reset action */
    if (context.action == ACTION_RESET) {
        context.udev = g_udev_client_new (NULL);
        initial_list_reset (&context);
        goto out;
    }

    g_assert_not_reached ();

 out:
    g_free (context.interface);
    g_hash_table_unref (context.subnets);
    g_object_unref (context.udev);
    libusb_exit (context.usb_context);

    return EXIT_SUCCESS;
}

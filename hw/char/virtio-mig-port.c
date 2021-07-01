/*
 * Virtio Console and Generic Serial Port Devices
 *
 * Copyright Red Hat, Inc. 2009, 2010
 *
 * Authors:
 *  Amit Shah <amit.shah@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "qemu/error-report.h"
#include "trace.h"
#include "hw/virtio/virtio-serial.h"
#include "qapi/error.h"
#include "qapi/qapi-events-char.h"

#define TYPE_VIRTIO_MIGRATION_SERIAL_PORT "migport"
#define VIRTIO_MIGPORT(obj) \
    OBJECT_CHECK(MigPort, (obj), TYPE_VIRTIO_MIGRATION_SERIAL_PORT)

typedef struct MigPort {
    VirtIOSerialPort parent_obj;

    CharBackend chr;
    guint watch;
} MigPort;

/*
 * Callback function that's called from chardevs when backend becomes
 * writable.
 */
static gboolean chr_write_unblocked(GIOChannel *chan, GIOCondition cond,
                                    void *opaque)
{
    MigPort *mport = opaque;

    mport->watch = 0;
    virtio_serial_throttle_port(VIRTIO_SERIAL_PORT(mport), false);
    return FALSE;
}

/* Callback function that's called when the guest sends us data */
static ssize_t flush_buf(VirtIOSerialPort *port,
                         const uint8_t *buf, ssize_t len)
{
    MigPort *mport = VIRTIO_MIGPORT(port);
    ssize_t ret;

    if (!qemu_chr_fe_backend_connected(&mport->chr)) {
        /* If there's no backend, we can just say we consumed all data. */
        return len;
    }

    ret = qemu_chr_fe_write(&mport->chr, buf, len);
    trace_virtio_console_flush_buf(port->id, len, ret);

    if (ret < len) {
        VirtIOSerialPortClass *k = VIRTIO_SERIAL_PORT_GET_CLASS(port);

        /*
         * Ideally we'd get a better error code than just -1, but
         * that's what the chardev interface gives us right now.  If
         * we had a finer-grained message, like -EPIPE, we could close
         * this connection.
         */
        if (ret < 0)
            ret = 0;

        /* XXX we should be queuing data to send later for the
         * console devices too rather than silently dropping
         * console data on EAGAIN. The Linux virtio-console
         * hvc driver though does sends with spinlocks held,
         * so if we enable throttling that'll stall the entire
         * guest kernel, not merely the process writing to the
         * console.
         *
         * While we could queue data for later write without
         * enabling throttling, this would result in the guest
         * being able to trigger arbitrary memory usage in QEMU
         * buffering data for later writes.
         *
         * So fixing this problem likely requires fixing the
         * Linux virtio-console hvc driver to not hold spinlocks
         * while writing, and instead merely block the process
         * that's writing. QEMU would then need some way to detect
         * if the guest had the fixed driver too, before we can
         * use throttling on host side.
         */
        if (!k->is_console) {
            virtio_serial_throttle_port(port, true);
            if (!mport->watch) {
                mport->watch = qemu_chr_fe_add_watch(&mport->chr,
                                                    G_IO_OUT|G_IO_HUP,
                                                    chr_write_unblocked, mport);
            }
        }
    }
    return ret;
}

/* Callback function that's called when the guest opens/closes the port */
static void set_guest_connected(VirtIOSerialPort *port, int guest_connected)
{
    MigPort *mport = VIRTIO_MIGPORT(port);
    DeviceState *dev = DEVICE(port);
    VirtIOSerialPortClass *k = VIRTIO_SERIAL_PORT_GET_CLASS(port);

    if (!k->is_console) {
        qemu_chr_fe_set_open(&mport->chr, guest_connected);
    }

    if (dev->id) {
        qapi_event_send_vserport_change(dev->id, guest_connected);
    }
}

static void guest_writable(VirtIOSerialPort *port)
{
    MigPort *mport = VIRTIO_MIGPORT(port);

    qemu_chr_fe_accept_input(&mport->chr);
}

/* Readiness of the guest to accept data on a port */
static int chr_can_read(void *opaque)
{
    MigPort *mport = opaque;

    return virtio_serial_guest_ready(VIRTIO_SERIAL_PORT(mport));
}

/* Send data from a char device over to the guest */
static void chr_read(void *opaque, const uint8_t *buf, int size)
{
    MigPort *mport = opaque;
    VirtIOSerialPort *port = VIRTIO_SERIAL_PORT(mport);

    trace_virtio_console_chr_read(port->id, size);
    virtio_serial_write(port, buf, size);
}

static void chr_event(void *opaque, int event)
{
    MigPort *mport = opaque;
    VirtIOSerialPort *port = VIRTIO_SERIAL_PORT(mport);

    trace_virtio_console_chr_event(port->id, event);
    switch (event) {
    case CHR_EVENT_OPENED:
        virtio_serial_open(port);
        break;
    case CHR_EVENT_CLOSED:
        if (mport->watch) {
            g_source_remove(mport->watch);
            mport->watch = 0;
        }
        virtio_serial_close(port);
        break;
    }
}

static int chr_be_change(void *opaque)
{
    MigPort *mport = opaque;
    VirtIOSerialPort *port = VIRTIO_SERIAL_PORT(mport);
    VirtIOSerialPortClass *k = VIRTIO_SERIAL_PORT_GET_CLASS(port);

    if (k->is_console) {
        qemu_chr_fe_set_handlers(&mport->chr, chr_can_read, chr_read,
                                 NULL, chr_be_change, mport, NULL, true);
    } else {
        qemu_chr_fe_set_handlers(&mport->chr, chr_can_read, chr_read,
                                 chr_event, chr_be_change, mport, NULL, false);
    }

    if (mport->watch) {
        g_source_remove(mport->watch);
        mport->watch = qemu_chr_fe_add_watch(&mport->chr,
                                            G_IO_OUT | G_IO_HUP,
                                            chr_write_unblocked, mport);
    }

    return 0;
}

static void mig_enable_backend(VirtIOSerialPort *port, bool enable)
{
    MigPort *mport = VIRTIO_MIGPORT(port);

    if (!qemu_chr_fe_backend_connected(&mport->chr)) {
        return;
    }

    if (enable) {
        VirtIOSerialPortClass *k = VIRTIO_SERIAL_PORT_GET_CLASS(port);

        qemu_chr_fe_set_handlers(&mport->chr, chr_can_read, chr_read,
                                 k->is_console ? NULL : chr_event,
                                 chr_be_change, mport, NULL, false);
    } else {
        qemu_chr_fe_set_handlers(&mport->chr, NULL, NULL, NULL,
                                 NULL, NULL, NULL, false);
    }
}

static void mig_realize(DeviceState *dev, Error **errp)
{
    VirtIOSerialPort *port = VIRTIO_SERIAL_PORT(dev);
    MigPort *mport = VIRTIO_MIGPORT(dev);
    VirtIOSerialPortClass *k = VIRTIO_SERIAL_PORT_GET_CLASS(dev);

    printf("LOG : mig_realize\n");
    if (port->id == 0 && !k->is_console) {
        error_setg(errp, "Port number 0 on virtio-serial devices reserved "
                   "for virtconsole devices for backward compatibility.");
        return;
    }

    if (qemu_chr_fe_backend_connected(&mport->chr)) {
        /*
         * For consoles we don't block guest data transfer just
         * because nothing is connected - we'll just let it go
         * whetherever the chardev wants - /dev/null probably.
         *
         * For serial ports we need 100% reliable data transfer
         * so we use the opened/closed signals from chardev to
         * trigger open/close of the device
         */
        if (k->is_console) {
            qemu_chr_fe_set_handlers(&mport->chr, chr_can_read, chr_read,
                                     NULL, chr_be_change,
                                     mport, NULL, true);
            virtio_serial_open(port);
        } else {
            qemu_chr_fe_set_handlers(&mport->chr, chr_can_read, chr_read,
                                     chr_event, chr_be_change,
                                     mport, NULL, false);
        }
    }
	printf("LOG : mig_port_realize: %p\n", g_mig_port);
	g_mig_port = port;
}

static void mig_unrealize(DeviceState *dev, Error **errp)
{
    MigPort *mport = VIRTIO_MIGPORT(dev);

    if (mport->watch) {
        g_source_remove(mport->watch);
    }
}

static Property mig_port_properties[] = {
    DEFINE_PROP_CHR("chardev", MigPort, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void mig_port_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtIOSerialPortClass *k = VIRTIO_SERIAL_PORT_CLASS(klass);
	printf("LOG : mig_port_class_init\n");

    k->realize = mig_realize;
    k->unrealize = mig_unrealize;
    k->have_data = flush_buf;
    k->set_guest_connected = set_guest_connected;
    k->enable_backend = mig_enable_backend;
    k->guest_writable = guest_writable;
    dc->props = mig_port_properties;
    k->is_console = true;
}

static const TypeInfo mig_port_info = {
    .name          = TYPE_VIRTIO_MIGRATION_SERIAL_PORT,
    .parent        = TYPE_VIRTIO_SERIAL_PORT,
    .instance_size = sizeof(MigPort),
    .class_init    = mig_port_class_init,
};

static void mig_port_register_types(void)
{
    printf("LOG : mig_port_register_types\n");
    type_register_static(&mig_port_info);
}

VirtIOSerialPort *g_mig_port;
type_init(mig_port_register_types)

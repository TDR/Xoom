/* qmidevice.c - gobi QMI device
 * Copyright (c) 2010, Code Aurora Forum. All rights reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "qmidevice.h"
#include "qcusbnet.h"
#include <linux/poll.h>

struct readreq {
	struct list_head node;
	void *data;
	u16 tid;
	u16 size;
};

struct notifyreq {
	struct list_head node;
	void (*func)(struct qcusbnet *, u16, void *);
	u16  tid;
	void *data;
};

struct client {
	struct list_head node;
	u16 cid;
	struct list_head reads;
	struct list_head notifies;
	struct list_head urbs;
	struct qmihandle *handle;
};

struct urbsetup {
	u8 type;
	u8 code;
	u16 value;
	u16 index;
	u16 len;
};

struct qmihandle {
	u16 cid;
	struct qcusbnet *dev;
	wait_queue_head_t read_wait;
};

extern int qcusbnet_debug;
static int qcusbnet2k_fwdelay;

static struct client *client_bycid(struct qcusbnet *dev, u16 cid);
static bool client_addread(struct client *client, u16 tid,
					void *data, u16 size);
static bool client_delread(struct client *client, u16 tid,
					void **data, u16 *size);
static bool client_addurb(struct client *client, struct urb *urb);
static struct urb *client_delurb(struct client *client);
static bool client_addnotify(struct client *client, u16 tid,
			     void (*hook)(struct qcusbnet *, u16 cid, void *),
			     void *data);
static bool client_notify(struct client *client, struct qcusbnet *dev, u16 tid);

static int resubmit_int_urb(struct urb *urb);

static bool qmi_ready(struct qcusbnet *dev, u16 timeout);
static void wds_callback(struct qcusbnet *dev, u16 cid, void *data);
static int setup_wds_callback(struct qcusbnet *dev);
static int qmidms_getmeid(struct qcusbnet *dev);

#define IOCTL_QMI_GET_SERVICE_FILE	(0x8BE0 + 1)
#define IOCTL_QMI_GET_DEVICE_VIDPID	(0x8BE0 + 2)
#define IOCTL_QMI_GET_DEVICE_MEID	(0x8BE0 + 3)
#define IOCTL_QMI_CLOSE			(0x8BE0 + 4)
#define CDC_GET_MASK			0xFFFFll
#define CDC_GET_ENCAPSULATED_RESPONSE	0x01A1ll
#define CDC_CONNECTION_SPEED_CHANGE	0x08000000002AA1ll

static inline void assert_locked(struct qcusbnet *dev)
{
	BUG_ON(!spin_is_locked(&dev->qmi.clients_lock));
}

static inline bool device_connected(struct qcusbnet *dev)
{
	return dev->valid && (!dev->dying);
}

void qc_setdown(struct qcusbnet *dev, u8 reason)
{
	set_bit(reason, &dev->down);
	netif_carrier_off(dev->usbnet->net);
}

void qc_cleardown(struct qcusbnet *dev, u8 reason)
{
	clear_bit(reason, &dev->down);
	if (!dev->down)
		netif_carrier_on(dev->usbnet->net);
}

bool qc_isdown(struct qcusbnet *dev, u8 reason)
{
	return test_bit(reason, &dev->down);
}

static int resubmit_int_urb(struct urb *urb)
{
	int status;
	int interval;
	if (!urb || !urb->dev)
		return -EINVAL;
	interval = urb->dev->speed == USB_SPEED_HIGH ? 7 : 3;
	usb_fill_int_urb(urb, urb->dev, urb->pipe, urb->transfer_buffer,
			 urb->transfer_buffer_length, urb->complete,
			 urb->context, interval);
	status = usb_submit_urb(urb, GFP_ATOMIC);
	if (status)
		DBG("status %d", status);
	return status;
}

static void read_callback(struct urb *urb)
{
	struct list_head *node;
	int result;
	u16 cid;
	struct client *client;
	void *data;
	void *copy;
	u16 size;
	struct qcusbnet *dev;
	unsigned long flags;
	u16 tid;

	dev = urb->context;

	switch (urb->status) {
	case 0:
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
	case -EPROTO:
		DBG("urb terminated, status %d\n", urb->status);
		return;
	default:
		ERR("non-zero status %d\n", urb->status);
		resubmit_int_urb(dev->qmi.inturb);
		return;
	}

	DBG("Read %d bytes\n", urb->actual_length);

	data = urb->transfer_buffer;
	size = urb->actual_length;

	if (qcusbnet_debug)
		print_hex_dump(KERN_INFO, "QCUSBNet2k: ", DUMP_PREFIX_OFFSET,
		       16, 1, data, size, true);

	result = qmux_parse(&cid, data, size);
	if (result < 0) {
		ERR("Read error parsing QMUX %d\n", result);
		resubmit_int_urb(dev->qmi.inturb);
		return;
	}

	if (size < result + 3) {
		DBG("Data buffer too small to parse\n");
		resubmit_int_urb(dev->qmi.inturb);
		return;
	}

	if (cid == QMICTL)
		tid = *(u8 *)(data + result + 1);
	else
		tid = *(u16 *)(data + result + 1);
	spin_lock_irqsave(&dev->qmi.clients_lock, flags);

	list_for_each(node, &dev->qmi.clients) {
		client = list_entry(node, struct client, node);
		if (client->cid == cid || (client->cid | 0xff00) == cid) {
			copy = kmalloc(size, GFP_ATOMIC);
			if (!copy) {
				ERR("malloc failed\n");
				spin_unlock_irqrestore(&dev->qmi.clients_lock,
								flags);
				return;
			}
			memcpy(copy, data, size);
			if (!client_addread(client, tid, copy, size)) {
				ERR("Error allocating pReadMemListEntry "
					  "read will be discarded\n");
				kfree(copy);
				spin_unlock_irqrestore(&dev->qmi.clients_lock,
								flags);
				resubmit_int_urb(dev->qmi.inturb);
				return;
			}

			if (client->handle)
				wake_up_interruptible(&client->handle->read_wait);

			DBG("Creating new readListEntry for client 0x%04X, TID %x\n",
			    cid, tid);

			client_notify(client, dev, tid);

			if (cid >> 8 != 0xff)
				break;
		}
	}

	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
	resubmit_int_urb(dev->qmi.inturb);
}

static void int_callback(struct urb *urb)
{
	int status;
	struct qcusbnet *dev = (struct qcusbnet *)urb->context;

	if (urb->status) {
		DBG("Int status = %d\n", urb->status);
		if (urb->status != -EOVERFLOW)
			return;
	} else {
		if ((urb->actual_length == 8) &&
		    (*(u64 *)urb->transfer_buffer & CDC_GET_MASK) ==
					CDC_GET_ENCAPSULATED_RESPONSE) {
			usb_fill_control_urb(dev->qmi.readurb, dev->usbnet->udev,
					     usb_rcvctrlpipe(dev->usbnet->udev, 0),
					     (unsigned char *)dev->qmi.readsetup,
					     dev->qmi.readbuf,
					     DEFAULT_READ_URB_LENGTH,
					     read_callback, dev);
			status = usb_submit_urb(dev->qmi.readurb, GFP_ATOMIC);
			if (status) {
				ERR("Error submitting Read URB %d\n", status);
				return;
			}
		} else if ((urb->actual_length == 16) &&
			   (*(u64 *)urb->transfer_buffer == CDC_CONNECTION_SPEED_CHANGE)) {
			/* if upstream or downstream is 0, stop traffic.
			 * Otherwise resume it */
			if ((*(u32 *)(urb->transfer_buffer + 8) == 0) ||
			    (*(u32 *)(urb->transfer_buffer + 12) == 0)) {
				qc_setdown(dev, DOWN_CDC_CONNECTION_SPEED);
				DBG("traffic stopping due to CONNECTION_SPEED_CHANGE\n");
			} else {
				qc_cleardown(dev, DOWN_CDC_CONNECTION_SPEED);
				DBG("resuming traffic due to CONNECTION_SPEED_CHANGE\n");
			}
		} else {
			DBG("ignoring invalid interrupt in packet\n");
			if (qcusbnet_debug)
				print_hex_dump(KERN_INFO, "QCUSBNet2k: ",
				       DUMP_PREFIX_OFFSET, 16, 1,
				       urb->transfer_buffer,
				       urb->actual_length, true);
		}
	}

	resubmit_int_urb(dev->qmi.inturb);
	return;
}

int qc_startread(struct qcusbnet *dev)
{
	int interval;
	int numends;
	int i;
	struct usb_host_endpoint *endpoint = NULL;

	dev->qmi.readurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->qmi.readurb) {
		ERR("Error allocating read urb\n");
		return -ENOMEM;
	}

	dev->qmi.inturb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->qmi.inturb) {
		usb_free_urb(dev->qmi.readurb);
		ERR("Error allocating int urb\n");
		return -ENOMEM;
	}

	dev->qmi.readbuf = kmalloc(DEFAULT_READ_URB_LENGTH, GFP_KERNEL);
	if (!dev->qmi.readbuf) {
		usb_free_urb(dev->qmi.readurb);
		usb_free_urb(dev->qmi.inturb);
		ERR("Error allocating read buffer\n");
		return -ENOMEM;
	}

	dev->qmi.intbuf = kmalloc(DEFAULT_READ_URB_LENGTH, GFP_KERNEL);
	if (!dev->qmi.intbuf) {
		usb_free_urb(dev->qmi.readurb);
		usb_free_urb(dev->qmi.inturb);
		kfree(dev->qmi.readbuf);
		ERR("Error allocating int buffer\n");
		return -ENOMEM;
	}

	dev->qmi.readsetup = kmalloc(sizeof(*dev->qmi.readsetup), GFP_KERNEL);
	if (!dev->qmi.readsetup) {
		usb_free_urb(dev->qmi.readurb);
		usb_free_urb(dev->qmi.inturb);
		kfree(dev->qmi.readbuf);
		kfree(dev->qmi.intbuf);
		ERR("Error allocating setup packet buffer\n");
		return -ENOMEM;
	}

	dev->qmi.readsetup->type = 0xA1;
	dev->qmi.readsetup->code = 1;
	dev->qmi.readsetup->value = 0;
	dev->qmi.readsetup->index =
		dev->iface->cur_altsetting->desc.bInterfaceNumber;
	dev->qmi.readsetup->len = DEFAULT_READ_URB_LENGTH;

	interval = (dev->usbnet->udev->speed == USB_SPEED_HIGH) ? 7 : 3;

	numends = dev->iface->cur_altsetting->desc.bNumEndpoints;
	for (i = 0; i < numends; i++) {
		endpoint = dev->iface->cur_altsetting->endpoint + i;
		if (!endpoint) {
			ERR("invalid endpoint %u\n", i);
			return -EINVAL;
		}

		if (usb_endpoint_dir_in(&endpoint->desc)
		  && usb_endpoint_xfer_int(&endpoint->desc)) {
			DBG("Interrupt endpoint is %x\n", endpoint->desc.bEndpointAddress);
			break;
		}
	}

	usb_fill_int_urb(dev->qmi.inturb, dev->usbnet->udev,
			 usb_rcvintpipe(dev->usbnet->udev, endpoint->desc.bEndpointAddress),
			 dev->qmi.intbuf, DEFAULT_READ_URB_LENGTH,
			 int_callback, dev, interval);

	return usb_submit_urb(dev->qmi.inturb, GFP_KERNEL);
}

void qc_stopread(struct qcusbnet *dev)
{
	if (dev->qmi.readurb) {
		DBG("Killing read URB\n");
		usb_kill_urb(dev->qmi.readurb);
	}

	if (dev->qmi.inturb) {
		DBG("Killing int URB\n");
		usb_kill_urb(dev->qmi.inturb);
	}

	kfree(dev->qmi.readsetup);
	dev->qmi.readsetup = NULL;
	kfree(dev->qmi.readbuf);
	dev->qmi.readbuf = NULL;
	kfree(dev->qmi.intbuf);
	dev->qmi.intbuf = NULL;

	usb_free_urb(dev->qmi.readurb);
	dev->qmi.readurb = NULL;
	usb_free_urb(dev->qmi.inturb);
	dev->qmi.inturb = NULL;
}

static int read_async(struct qcusbnet *dev, u16 cid, u16 tid,
		      void (*hook)(struct qcusbnet *, u16, void *),
		      void *data)
{
	struct list_head *node;
	struct client *client;
	struct readreq *readreq;

	unsigned long flags;

	spin_lock_irqsave(&dev->qmi.clients_lock, flags);

	client = client_bycid(dev, cid);
	if (!client) {
		ERR("Could not find matching client ID 0x%04X\n", cid);
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		return -ENXIO;
	}

	list_for_each(node, &client->reads) {
		readreq = list_entry(node, struct readreq, node);
		if (!tid || tid == readreq->tid) {
			spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
			hook(dev, cid, data);
			return 0;
		}
	}

	if (!client_addnotify(client, tid, hook, data))
		ERR("Unable to register for notification\n");

	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
	return 0;
}

static void upsem(struct qcusbnet *dev, u16 cid, void *data)
{
	DBG("0x%04X\n", cid);
	up((struct semaphore *)data);
}

static int read_sync(struct qcusbnet *dev, void **buf, u16 cid, u16 tid)
{
	struct list_head *node;
	int result;
	struct client *client;
	struct notifyreq *notify;
	struct semaphore sem;
	void *data;
	unsigned long flags;
	u16 size;

	mutex_lock(&dev->mutex);
	if (!device_connected(dev)) {
		mutex_unlock(&dev->mutex);
		return -ENXIO;
	}
	spin_lock_irqsave(&dev->qmi.clients_lock, flags);

	client = client_bycid(dev, cid);
	if (!client) {
		ERR("Could not find matching client ID 0x%04X\n", cid);
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		mutex_unlock(&dev->mutex);
		return -ENXIO;
	}

	while (!client_delread(client, tid, &data, &size)) {
		sema_init(&sem, 0);
		if (!client_addnotify(client, tid, upsem, &sem)) {
			ERR("unable to register for notification\n");
			spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
			mutex_unlock(&dev->mutex);
			return -EFAULT;
		}

		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		mutex_unlock(&dev->mutex);

		result = down_interruptible(&sem);

		mutex_lock(&dev->mutex);

		if (result) {
			DBG("Interrupted %d\n", result);
			spin_lock_irqsave(&dev->qmi.clients_lock, flags);
			list_for_each(node, &client->notifies) {
				notify = list_entry(node, struct notifyreq, node);
				if (notify->data == &sem) {
					list_del(&notify->node);
					kfree(notify);
					break;
				}
			}

			spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
			mutex_unlock(&dev->mutex);
			return -EINTR;
		}

		if (!device_connected(dev)) {
			mutex_unlock(&dev->mutex);
			return -ENXIO;
		}

		spin_lock_irqsave(&dev->qmi.clients_lock, flags);
	}

	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
	mutex_unlock(&dev->mutex);
	*buf = data;
	return size;
}

static void write_callback(struct urb *urb)
{
	if (!urb) {
		ERR("null urb\n");
		return;
	}

	DBG("Write status/size %d/%d\n", urb->status, urb->actual_length);
	up((struct semaphore *)urb->context);
}

static int write_sync(struct qcusbnet *dev, char *buf, int size, u16 cid)
{
	int result;
	struct semaphore sem;
	struct urb *urb;
	struct client *client;
	struct urbsetup setup;
	unsigned long flags;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		ERR("URB mem error\n");
		return -ENOMEM;
	}

	result = qmux_fill(cid, buf, size);
	if (result < 0) {
		usb_free_urb(urb);
		return result;
	}

	/* CDC Send Encapsulated Request packet */
	setup.type = 0x21;
	setup.code = 0;
	setup.value = 0;
	setup.index = dev->iface->cur_altsetting->desc.bInterfaceNumber;
	setup.len = 0;
	setup.len = size;

	usb_fill_control_urb(urb, dev->usbnet->udev,
			     usb_sndctrlpipe(dev->usbnet->udev, 0),
			     (unsigned char *)&setup, (void *)buf, size,
			     NULL, dev);

	DBG("Actual Write:\n");
	if (qcusbnet_debug)
		print_hex_dump(KERN_INFO,  "QCUSBNet2k: ", DUMP_PREFIX_OFFSET,
		       16, 1, buf, size, true);

	sema_init(&sem, 0);

	urb->complete = write_callback;
	urb->context = &sem;

	result = usb_autopm_get_interface(dev->iface);
	if (result < 0) {
		ERR("unable to resume interface: %d\n", result);
		usb_free_urb(urb);
		return result;
	}

	spin_lock_irqsave(&dev->qmi.clients_lock, flags);

	client = client_bycid(dev, cid);
	if (!client) {
		usb_free_urb(urb);
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		usb_autopm_put_interface(dev->iface);
		return -EINVAL;
	}

	if (!client_addurb(client, urb)) {
		usb_free_urb(urb);
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		usb_autopm_put_interface(dev->iface);
		return -EINVAL;
	}

	result = usb_submit_urb(urb, GFP_ATOMIC);
	if (result < 0)	{
		ERR("submit URB error %d\n", result);
		if (client_delurb(client) != urb)
			ERR("Didn't get write URB back\n");

		usb_free_urb(urb);

		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		usb_autopm_put_interface(dev->iface);
		return result;
	}

	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
	result = down_interruptible(&sem);

	usb_autopm_put_interface(dev->iface);
	spin_lock_irqsave(&dev->qmi.clients_lock, flags);
	if (client_delurb(client) != urb) {
		ERR("Didn't get write URB back\n");
		usb_free_urb(urb);
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		return -EINVAL;
	}
	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);

	if (!result) {
		if (!urb->status) {
			result = size;
		} else {
			ERR("bad status = %d\n", urb->status);
			result = urb->status;
		}
	} else {
		ERR("Interrupted %d !!!\n", result);
		ERR("Device may be in bad state and need reset !!!\n");
		usb_kill_urb(urb);
	}

	usb_free_urb(urb);
	return result;
}

static struct client *client_alloc(struct qcusbnet *dev, u8 type)
{
	u16 cid;
	struct client *client;
	int result;
	void *wbuf;
	size_t wbufsize;
	void *rbuf;
	u16 rbufsize;
	unsigned long flags;
	u8 tid;

	mutex_lock(&dev->mutex);
	if (!device_connected(dev)) {
		mutex_unlock(&dev->mutex);
		return ERR_PTR(-ENXIO);
	}

	if (type) {
		mutex_unlock(&dev->mutex);

		tid = atomic_add_return(1, &dev->qmi.qmitid);
		if (!tid)
			tid = atomic_add_return(1, &dev->qmi.qmitid);
		wbuf = qmictl_new_getcid(tid, type, &wbufsize);
		if (!wbuf) {
			mutex_unlock(&dev->mutex);
			return ERR_PTR(-ENOMEM);
		}

		result = write_sync(dev, wbuf, wbufsize, QMICTL);
		kfree(wbuf);

		if (result < 0)
			return ERR_PTR(result);

		result = read_sync(dev, &rbuf, QMICTL, tid);
		if (result < 0) {
			ERR("bad read data %d\n", result);
			return ERR_PTR(result);
		}
		rbufsize = result;

		result = qmictl_alloccid_resp(rbuf, rbufsize, &cid);
		kfree(rbuf);

		if (result < 0)
			return ERR_PTR(result);

		mutex_lock(&dev->mutex);
		if (!device_connected(dev)) {
			mutex_unlock(&dev->mutex);
			return ERR_PTR(-ENXIO);
		}
	} else {
		cid = 0;
	}

	spin_lock_irqsave(&dev->qmi.clients_lock, flags);
	if (client_bycid(dev, cid)) {
		DBG("Client memory already exists\n");
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		mutex_unlock(&dev->mutex);
		return ERR_PTR(-ETOOMANYREFS);
	}

	client = kmalloc(sizeof(*client), GFP_ATOMIC);
	if (!client) {
		ERR("Error allocating read list\n");
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		mutex_unlock(&dev->mutex);
		return ERR_PTR(-ENOMEM);
	}

	list_add_tail(&client->node, &dev->qmi.clients);
	client->cid = cid;
	client->handle = NULL;
	INIT_LIST_HEAD(&client->reads);
	INIT_LIST_HEAD(&client->notifies);
	INIT_LIST_HEAD(&client->urbs);
	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
	mutex_unlock(&dev->mutex);
	return client;
}

static void client_free(struct qcusbnet *dev, u16 cid)
{
	int result;
	struct client *client;
	struct urb *urb;
	void *data;
	u16 size;
	void *wbuf;
	size_t wbufsize;
	void *rbuf;
	u16 rbufsize;
	unsigned long flags;
	u8 tid;

	DBG("releasing 0x%04X\n", cid);

	mutex_lock(&dev->mutex);
	if (!dev->valid) {
		mutex_unlock(&dev->mutex);
		return;
	}

	if (cid != QMICTL && !dev->dying) {
		mutex_unlock(&dev->mutex);
		tid = atomic_add_return(1, &dev->qmi.qmitid);
		if (!tid)
			tid = atomic_add_return(1, &dev->qmi.qmitid);
		wbuf = qmictl_new_releasecid(tid, cid, &wbufsize);
		if (!wbuf) {
			ERR("memory error\n");
		} else {
			result = write_sync(dev, wbuf, wbufsize, QMICTL);
			kfree(wbuf);

			if (result < 0) {
				ERR("bad write status %d\n", result);
			} else {
				result = read_sync(dev, &rbuf, QMICTL, tid);
				if (result < 0) {
					ERR("bad read status %d\n", result);
				} else {
					rbufsize = result;
					result = qmictl_freecid_resp(rbuf, rbufsize);
					if (result < 0)
						ERR("error %d parsing response\n", result);
					kfree(rbuf);
				}
			}
		}
		mutex_lock(&dev->mutex);
		if (!dev->valid) {
			mutex_unlock(&dev->mutex);
			return;
		}
	}

	spin_lock_irqsave(&dev->qmi.clients_lock, flags);
	client = client_bycid(dev, cid);
	if (!client) {
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		mutex_unlock(&dev->mutex);
		return;
	}

	list_del(&client->node);

	while (client_notify(client, dev, 0))
		;

	if (client->handle)
		wake_up(&client->handle->read_wait);

	while ((urb = client_delurb(client))) {
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		usb_kill_urb(urb);
		usb_free_urb(urb);
		spin_lock_irqsave(&dev->qmi.clients_lock, flags);
	}

	while (client_delread(client, 0, &data, &size))
		kfree(data);

	kfree(client);

	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
	mutex_unlock(&dev->mutex);
}

struct client *client_bycid(struct qcusbnet *dev, u16 cid)
{
	struct list_head *node;
	struct client *client;

	assert_locked(dev);

	list_for_each(node, &dev->qmi.clients) {
		client = list_entry(node, struct client, node);
		if (client->cid == cid)
			return client;
	}

	DBG("Could not find client mem 0x%04X\n", cid);
	return NULL;
}

static bool
client_addread(struct client *client, u16 tid, void *data, u16 size)
{
	struct readreq *req;

	req = kmalloc(sizeof(*req), GFP_ATOMIC);
	if (!req) {
		ERR("Mem error\n");
		return false;
	}

	req->data = data;
	req->size = size;
	req->tid = tid;

	list_add_tail(&req->node, &client->reads);

	return true;
}

static bool
client_delread(struct client *client, u16 tid, void **data, u16 *size)
{
	struct readreq *req;
	struct list_head *node;

	list_for_each(node, &client->reads) {
		req = list_entry(node, struct readreq, node);
		if (!tid || tid == req->tid) {
			*data = req->data;
			*size = req->size;
			list_del(&req->node);
			kfree(req);
			return true;
		}

		DBG("skipping 0x%04X data TID = %x\n", client->cid, req->tid);
	}

	DBG("No read memory to pop, Client 0x%04X, TID=%x\n", client->cid, tid);
	return false;
}

static bool client_addnotify(struct client *client, u16 tid,
			     void (*hook)(struct qcusbnet *, u16, void *),
			     void *data)
{
	struct notifyreq *req;

	req = kmalloc(sizeof(*req), GFP_ATOMIC);
	if (!req) {
		ERR("Mem error\n");
		return false;
	}

	list_add_tail(&req->node, &client->notifies);
	req->func = hook;
	req->data = data;
	req->tid = tid;

	return true;
}

static bool client_notify(struct client *client, struct qcusbnet *dev, u16 tid)
{
	struct notifyreq *delnotify = NULL;
	struct notifyreq *notify;
	struct list_head *node;

	assert_locked(dev);

	delnotify = NULL;

	list_for_each(node, &client->notifies) {
		notify = list_entry(node, struct notifyreq, node);
		if (!tid || !notify->tid || tid == notify->tid) {
			delnotify = notify;
			break;
		}

		DBG("skipping data TID = %x\n", notify->tid);
	}

	if (delnotify) {
		list_del(&delnotify->node);
		if (delnotify->func) {
			spin_unlock(&dev->qmi.clients_lock);
			delnotify->func(dev, client->cid, delnotify->data);
			spin_lock(&dev->qmi.clients_lock);
		}
		kfree(delnotify);
		return true;
	}

	DBG("no one to notify for TID %x\n", tid);
	return false;
}

static bool client_addurb(struct client *client, struct urb *urb)
{
	struct urbreq *req;

	req = kmalloc(sizeof(*req), GFP_ATOMIC);
	if (!req) {
		ERR("Mem error\n");
		return false;
	}

	req->urb = urb;
	list_add_tail(&req->node, &client->urbs);

	return true;
}

static struct urb *client_delurb(struct client *client)
{
	struct urbreq *req;
	struct urb *urb;

	if (list_empty(&client->urbs)) {
		DBG("No URB's to pop\n");
		return NULL;
	}

	req = list_first_entry(&client->urbs, struct urbreq, node);
	list_del(&req->node);
	urb = req->urb;
	kfree(req);
	return urb;
}

static int devqmi_open(struct inode *inode, struct file *file)
{
	struct qmihandle *handle;
	struct qcusbnet *dev;
	struct qcusbnet *ref;

	dev = cdev_to_qcusbnet(inode->i_cdev);
	if (!dev)
		return -ENXIO;

	/* We need an extra ref on the device per fd, since we stash a ref
	 * inside the handle. If qcusbnet_get() returns NULL, that means the
	 * device has been removed from the list - no new refs for us. */
	ref = qcusbnet_get(dev);
	if (!ref)
		return -ENXIO;

	file->private_data = kmalloc(sizeof(struct qmihandle), GFP_KERNEL);
	if (!file->private_data) {
		ERR("Mem error\n");
		return -ENOMEM;
	}

	handle = (struct qmihandle *)file->private_data;
	handle->cid = (u16)-1;
	handle->dev = ref;
	init_waitqueue_head(&handle->read_wait);

	DBG("%p %04x", handle, handle->cid);

	return 0;
}

static long devqmi_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int result;
	u32 vidpid;
	struct client *client;
	struct qmihandle *handle = (struct qmihandle *)file->private_data;
	struct qcusbnet *dev = handle->dev;
	unsigned long flags;

	DBG("%p %04x %08x", handle, handle->cid, cmd);

	switch (cmd) {
	case IOCTL_QMI_GET_SERVICE_FILE:

		DBG("Setting up QMI for service %lu\n", arg);
		if (!(u8)arg) {
			ERR("Cannot use QMICTL from userspace\n");
			return -EINVAL;
		}

		if (handle->cid != (u16)-1) {
			ERR("Close the current connection before opening a new one\n");
			return -EBADR;
		}

		client = client_alloc(dev, (u8)arg);
		if (IS_ERR(client))
			return PTR_ERR(client);

		mutex_lock(&dev->mutex);
		if (!device_connected(dev)) {
			mutex_unlock(&dev->mutex);
			return -ENXIO;
		}

		handle->cid = client->cid;

		spin_lock_irqsave(&dev->qmi.clients_lock, flags);
		client->handle = handle;
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);

		mutex_unlock(&dev->mutex);
		return 0;
		break;

	/* Okay, all aboard the nasty hack express. If we don't have this
	 * ioctl() (and we just rely on userspace to close() the file
	 * descriptors), if userspace has any refs left to this fd (like, say, a
	 * pending read()), then the read might hang around forever. Userspace
	 * needs a way to cause us to kick people off those waitqueues before
	 * closing the fd for good.
	 *
	 * If this driver used workqueues, the correct approach here would
	 * instead be to make the file descriptor select()able, and then just
	 * use select() instead of aio in userspace (thus allowing us to get
	 * away with one thread total and avoiding the recounting mess
	 * altogether).
	 */
	case IOCTL_QMI_CLOSE:
		DBG("Tearing down QMI for service %lu", arg);
		if (handle->cid == (u16)-1) {
			DBG("no qmi cid");
			return -EBADR;
		}

		file->private_data = NULL;
		client_free(dev, handle->cid);
		qcusbnet_put(dev);
		kfree(handle);
		return 0;
		break;

	case IOCTL_QMI_GET_DEVICE_VIDPID:
		if (!arg) {
			ERR("Bad VIDPID buffer\n");
			return -EINVAL;
		}

		if (!dev->usbnet) {
			ERR("Bad usbnet\n");
			return -ENOMEM;
		}

		if (!dev->usbnet->udev) {
			ERR("Bad udev\n");
			return -ENOMEM;
		}

		mutex_lock(&dev->mutex);
		if (!device_connected(dev)) {
			mutex_unlock(&dev->mutex);
			return -ENXIO;
		}

		vidpid = ((le16_to_cpu(dev->usbnet->udev->descriptor.idVendor) << 16)
			  + le16_to_cpu(dev->usbnet->udev->descriptor.idProduct));

		result = copy_to_user((unsigned int *)arg, &vidpid, 4);
		if (result)
			ERR("Copy to userspace failure\n");

		mutex_unlock(&dev->mutex);
		return result;
		break;

	case IOCTL_QMI_GET_DEVICE_MEID:
		if (!arg) {
			ERR("Bad MEID buffer\n");
			return -EINVAL;
		}

		mutex_lock(&dev->mutex);
		if (!device_connected(dev)) {
			mutex_unlock(&dev->mutex);
			return -ENXIO;
		}

		result = copy_to_user((unsigned int *)arg, &dev->meid[0], 14);
		if (result)
			ERR("copy to userspace failure\n");

		mutex_unlock(&dev->mutex);
		return result;
		break;
	default:
		return -EBADRQC;
	}
}

static int devqmi_release(struct inode *inode, struct file *file)
{
	struct qmihandle *handle = (struct qmihandle *)file->private_data;

	if (!handle)
		return 0;
	file->private_data = NULL;

	if (handle->cid != (u16)-1)
		client_free(handle->dev, handle->cid);

	qcusbnet_put(handle->dev);
	kfree(handle);
	return 0;
}

static ssize_t devqmi_read(struct file *file, char __user *buf, size_t size,
			   loff_t *pos)
{
	int result;
	void *data = NULL;
	void *smalldata;
	struct qmihandle *handle = (struct qmihandle *)file->private_data;
	struct qcusbnet *dev = handle->dev;

	mutex_lock(&dev->mutex);
	if (!device_connected(dev)) {
		mutex_unlock(&dev->mutex);
		return -ENXIO;
	}

	if (handle->cid == (u16)-1) {
		ERR("Client ID must be set before reading 0x%04X\n",
		    handle->cid);
		mutex_unlock(&dev->mutex);
		return -EBADR;
	}

	mutex_unlock(&dev->mutex);

	result = read_sync(dev, &data, handle->cid, 0);
	if (result <= 0)
		return result;

	result -= qmux_size;
	smalldata = data + qmux_size;

	if (result > size) {
		ERR("Read data is too large for amount user has requested\n");
		kfree(data);
		return -EOVERFLOW;
	}

	if (copy_to_user(buf, smalldata, result)) {
		ERR("Error copying read data to user\n");
		result = -EFAULT;
	}

	kfree(data);
	return result;
}

static ssize_t devqmi_write(struct file *file, const char __user * buf,
			    size_t size, loff_t *pos)
{
	int status;
	void *wbuf;
	struct qmihandle *handle = (struct qmihandle *)file->private_data;
	struct qcusbnet *dev = handle->dev;

	mutex_lock(&dev->mutex);
	if (!device_connected(dev)) {
		mutex_unlock(&dev->mutex);
		return -ENXIO;
	}

	if (handle->cid == (u16)-1) {
		ERR("Client ID must be set before writing 0x%04X\n",
			  handle->cid);
		mutex_unlock(&dev->mutex);
		return -EBADR;
	}

	mutex_unlock(&dev->mutex);

	wbuf = kmalloc(size + qmux_size, GFP_KERNEL);
	if (!wbuf)
		return -ENOMEM;

	status = copy_from_user(wbuf + qmux_size, buf, size);
	if (status) {
		ERR("Unable to copy data from userspace %d\n", status);
		kfree(wbuf);
		return status;
	}

	status = write_sync(dev, wbuf, size + qmux_size,
			    handle->cid);

	kfree(wbuf);
	if (status == size + qmux_size)
		return size;
	return status;
}

static unsigned int devqmi_poll(struct file *file, poll_table *wait)
{
	struct qmihandle *handle = (struct qmihandle *)file->private_data;
	struct qcusbnet *dev = handle->dev;
	struct client *client;
	unsigned int mask = 0;
	unsigned long flags;

	poll_wait(file, &handle->read_wait, wait);

	mutex_lock(&dev->mutex);
	if (!device_connected(dev)) {
		mutex_unlock(&dev->mutex);
		return POLLERR | POLLHUP;
	}

	if (handle->cid == (u16)-1) {
		ERR("Client ID must be set before polling 0x%04X\n",
			  handle->cid);
		mutex_unlock(&dev->mutex);
		return POLLERR | POLLNVAL;
	}

	spin_lock_irqsave(&dev->qmi.clients_lock, flags);

	client = client_bycid(dev, handle->cid);
	if (!client) {
		ERR("Could not find matching client ID 0x%04X\n", handle->cid);
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		mutex_unlock(&dev->mutex);
		return POLLERR | POLLHUP;
	}

	if (!list_empty(&client->reads))
		mask |= POLLIN | POLLRDNORM;

	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
	mutex_unlock(&dev->mutex);

	return mask;
}

static const struct file_operations devqmi_fops = {
	.owner = THIS_MODULE,
	.read  = devqmi_read,
	.write = devqmi_write,
	.unlocked_ioctl = devqmi_ioctl,
	.open  = devqmi_open,
	.release = devqmi_release,
	.poll  = devqmi_poll,
};

int qc_register(struct qcusbnet *dev)
{
	int result;
	int qmiidx = 0;
	dev_t devno;
	char *name;
	struct client *client;

	dev->valid = true;
	dev->dying = false;
	client = client_alloc(dev, QMICTL);
	if (IS_ERR(client)) {
		dev->valid = false;
		return PTR_ERR(client);
	}
	atomic_set(&dev->qmi.qmitid, 1);

	result = qc_startread(dev);
	if (result)
		goto fail_start;

	if (!qmi_ready(dev, 30000)) {
		ERR("Device unresponsive to QMI\n");
		result = -ETIMEDOUT;
		goto fail_qmi;
	}

	result = setup_wds_callback(dev);
	if (result)
		goto fail_qmi;

	result = qmidms_getmeid(dev);
	if (result)
		goto fail_qmi;

	result = alloc_chrdev_region(&devno, 0, 1, "qcqmi");
	if (result < 0)
		goto fail_qmi;

	dev->qmi.cdev = cdev_alloc();
	dev->qmi.cdev->owner = THIS_MODULE;
	dev->qmi.cdev->ops = &devqmi_fops;

	result = cdev_add(dev->qmi.cdev, devno, 1);
	if (result) {
		ERR("error adding cdev\n");
		goto fail_cdev;
	}

	name = strstr(dev->usbnet->net->name, "qmi");
	if (!name) {
		ERR("Bad net name: %s\n", dev->usbnet->net->name);
		result = -ENXIO;
		goto fail_name;
	}
	name += strlen("qmi");
	qmiidx = simple_strtoul(name, NULL, 10);
	if (qmiidx < 0) {
		ERR("Bad minor number\n");
		result = -ENXIO;
		goto fail_name;
	}

	printk(KERN_INFO "creating qcqmi%d\n", qmiidx);
	device_create(dev->qmi.devclass, NULL, devno, NULL, "qcqmi%d", qmiidx);

	dev->qmi.devnum = devno;
	return 0;

fail_name:
	cdev_del(dev->qmi.cdev);
fail_cdev:
	unregister_chrdev_region(devno, 1);
fail_qmi:
	qc_stopread(dev);
fail_start:
	dev->valid = false;
	return 0;
}

void qc_deregister(struct qcusbnet *dev)
{
	struct client *client;
	unsigned long flags;

	mutex_lock(&dev->mutex);
	if (!dev->valid) {
		mutex_unlock(&dev->mutex);
		return;
	}
	dev->dying = true;
	qc_stopread(dev);

	spin_lock_irqsave(&dev->qmi.clients_lock, flags);
	while (!list_empty(&dev->qmi.clients)) {
		client = list_first_entry(&dev->qmi.clients,
					struct client, node);
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		mutex_unlock(&dev->mutex);

		DBG("release 0x%04X\n", client->cid);
		client_free(dev, client->cid);

		mutex_lock(&dev->mutex);
		spin_lock_irqsave(&dev->qmi.clients_lock, flags);
	}
	dev->valid = false;

	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
	mutex_unlock(&dev->mutex);

	if (!IS_ERR(dev->qmi.devclass))
		device_destroy(dev->qmi.devclass, dev->qmi.devnum);
	cdev_del(dev->qmi.cdev);
	unregister_chrdev_region(dev->qmi.devnum, 1);
}

static bool qmi_ready(struct qcusbnet *dev, u16 timeout)
{
	int result;
	void *wbuf = NULL;
	size_t wbufsize;
	void *rbuf;
	u16 rbufsize;
	struct semaphore sem;
	u16 now;
	unsigned long flags;
	struct client *client;
	u8 tid;

	for (now = 0; now < timeout; now += 100) {
		sema_init(&sem, 0);

		tid = atomic_add_return(1, &dev->qmi.qmitid);
		if (!tid)
			tid = atomic_add_return(1, &dev->qmi.qmitid);
		kfree(wbuf);
		wbuf = qmictl_new_ready(tid, &wbufsize);
		if (!wbuf)
			return -ENOMEM;

		result = read_async(dev, QMICTL, tid, upsem, &sem);
		if (result) {
			kfree(wbuf);
			return false;
		}

		write_sync(dev, wbuf, wbufsize, QMICTL);

		msleep(100);
		if (!down_trylock(&sem)) {
			spin_lock_irqsave(&dev->qmi.clients_lock, flags);
			client = client_bycid(dev, QMICTL);
			if (!client || client_delread(client, tid, &rbuf,
								&rbufsize)) {
				spin_unlock_irqrestore(&dev->qmi.clients_lock,
									flags);
				kfree(rbuf);
				break;
			} else {
				spin_unlock_irqrestore(&dev->qmi.clients_lock,
									flags);
			}
		} else {
			spin_lock_irqsave(&dev->qmi.clients_lock, flags);
			client = client_bycid(dev, QMICTL);
			if (!client) {
				spin_unlock_irqrestore(&dev->qmi.clients_lock,
									flags);
				break;
			}
			client_notify(client, dev, tid);
			spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		}
	}

	kfree(wbuf);

	if (now >= timeout)
		return false;

	DBG("QMI Ready after %u milliseconds\n", now);

	/* 3580 and newer doesn't need a delay; older needs 5000ms */
	if (qcusbnet2k_fwdelay)
		msleep(qcusbnet2k_fwdelay * 1000);

	return true;
}

static void wds_callback(struct qcusbnet *dev, u16 cid, void *data)
{
	bool ret;
	int result;
	void *rbuf;
	u16 rbufsize;
	struct client *client;

	struct net_device_stats *stats = &(dev->usbnet->net->stats);

	struct qmiwds_stats dstats = {
		.txok = (u32)-1,
		.rxok = (u32)-1,
		.txerr = (u32)-1,
		.rxerr = (u32)-1,
		.txofl = (u32)-1,
		.rxofl = (u32)-1,
		.txbytesok = (u64)-1,
		.rxbytesok = (u64)-1,
	};
	unsigned long flags;

	spin_lock_irqsave(&dev->qmi.clients_lock, flags);
	client = client_bycid(dev, cid);
	if (!client) {
		spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);
		return;
	}
	ret = client_delread(client, 0, &rbuf, &rbufsize);
	spin_unlock_irqrestore(&dev->qmi.clients_lock, flags);

	if (!ret) {
		ERR("WDS callback failed to get data\n");
		return;
	}

	dstats.linkstate = !qc_isdown(dev, DOWN_NO_NDIS_CONNECTION);
	dstats.reconfigure = false;

	result = qmiwds_event_resp(rbuf, rbufsize, &dstats);
	if (result < 0) {
		ERR("bad WDS packet\n");
	} else {
		if (dstats.txofl != (u32)-1)
			stats->tx_fifo_errors = dstats.txofl;

		if (dstats.rxofl != (u32)-1)
			stats->rx_fifo_errors = dstats.rxofl;

		if (dstats.txerr != (u32)-1)
			stats->tx_errors = dstats.txerr;

		if (dstats.rxerr != (u32)-1)
			stats->rx_errors = dstats.rxerr;

		if (dstats.txok != (u32)-1)
			stats->tx_packets = dstats.txok + stats->tx_errors;

		if (dstats.rxok != (u32)-1)
			stats->rx_packets = dstats.rxok + stats->rx_errors;

		if (dstats.txbytesok != (u64)-1)
			stats->tx_bytes = dstats.txbytesok;

		if (dstats.rxbytesok != (u64)-1)
			stats->rx_bytes = dstats.rxbytesok;

		if (dstats.reconfigure) {
			DBG("Net device link reset\n");
			qc_setdown(dev, DOWN_NO_NDIS_CONNECTION);
			qc_cleardown(dev, DOWN_NO_NDIS_CONNECTION);
		} else {
			if (dstats.linkstate) {
				DBG("Net device link is connected\n");
				qc_cleardown(dev, DOWN_NO_NDIS_CONNECTION);
			} else {
				DBG("Net device link is disconnected\n");
				qc_setdown(dev, DOWN_NO_NDIS_CONNECTION);
			}
		}
	}

	kfree(rbuf);

	result = read_async(dev, cid, 0, wds_callback, data);
	if (result != 0)
		ERR("unable to setup next async read\n");
}

static int setup_wds_callback(struct qcusbnet *dev)
{
	int result;
	void *buf;
	size_t size;
	u16 cid;
	struct client *client;

	client = client_alloc(dev, QMIWDS);
	if (IS_ERR(client))
		return PTR_ERR(client);
	cid = client->cid;

	buf = qmiwds_new_seteventreport(1, &size);
	if (!buf)
		return -ENOMEM;

	result = write_sync(dev, buf, size, cid);
	kfree(buf);

	if (result < 0)
		return result;

	buf = qmiwds_new_getpkgsrvcstatus(2, &size);
	if (buf == NULL)
		return -ENOMEM;

	result = write_sync(dev, buf, size, cid);
	kfree(buf);

	if (result < 0)
		return result;

	result = read_async(dev, cid, 0, wds_callback, NULL);
	if (result) {
		ERR("unable to setup async read\n");
		return result;
	}

	result = usb_control_msg(dev->usbnet->udev,
			usb_sndctrlpipe(dev->usbnet->udev, 0),
			0x22, 0x21, 1,
			dev->iface->cur_altsetting->desc.bInterfaceNumber,
			NULL, 0, 100);
	if (result < 0) {
		ERR("Bad SetControlLineState status %d\n", result);
		return result;
	}

	return 0;
}

static int qmidms_getmeid(struct qcusbnet *dev)
{
	int result;
	void *wbuf;
	size_t wbufsize;
	void *rbuf;
	u16 rbufsize;
	u16 cid;
	struct client *client;

	client = client_alloc(dev, QMIDMS);
	if (IS_ERR(client))
		return PTR_ERR(client);
	cid = client->cid;

	wbuf = qmidms_new_getmeid(1, &wbufsize);
	if (!wbuf)
		return -ENOMEM;

	result = write_sync(dev, wbuf, wbufsize, cid);
	kfree(wbuf);

	if (result < 0)
		return result;

	result = read_sync(dev, &rbuf, cid, 1);
	if (result < 0)
		return result;
	rbufsize = result;

	result = qmidms_meid_resp(rbuf, rbufsize, &dev->meid[0], 14);
	kfree(rbuf);

	if (result < 0) {
		ERR("bad get MEID resp\n");
		memset(&dev->meid[0], '0', 14);
	}

	client_free(dev, cid);
	return 0;
}

module_param(qcusbnet2k_fwdelay, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(qcusbnet2k_fwdelay, "Delay for old firmware");

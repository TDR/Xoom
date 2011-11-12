/* qcusbnet.c - gobi network device
 * Copyright (c) 2011, Code Aurora Forum. All rights reserved.

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

#include "structs.h"
#include "qmidevice.h"
#include "qmi.h"
#include "qcusbnet.h"

#include <linux/ctype.h>

#define DRIVER_VERSION "1.0.110+google"
#define DRIVER_AUTHOR "Qualcomm Innovation Center"
#define DRIVER_DESC "QCUSBNet2k"

static LIST_HEAD(qcusbnet_list);
static DEFINE_MUTEX(qcusbnet_lock);

int qcusbnet_debug;
static struct class *devclass;

#ifdef CONFIG_HAS_WAKELOCK
#define QC_WAKE_LOCK wake_lock
#define QC_WAKE_UNLOCK(X) wake_lock_timeout(X, HZ/10)
#else
#define QC_WAKE_LOCK
#define QC_WAKE_UNLOCK
#endif /* CONFIG_HAS_WAKELOCK */

static void free_dev(struct kref *ref)
{
	struct qcusbnet *dev = container_of(ref, struct qcusbnet, refcount);
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock_destroy(&dev->wake_lock);
#endif /* CONFIG_HAS_WAKELOCK */
	list_del(&dev->node);
	kfree(dev);
}

void qcusbnet_put(struct qcusbnet *dev)
{
	mutex_lock(&qcusbnet_lock);
	kref_put(&dev->refcount, free_dev);
	mutex_unlock(&qcusbnet_lock);
}

struct qcusbnet *qcusbnet_get(struct qcusbnet *key)
{
	/* Given a putative qcusbnet struct, return either the struct itself
	 * (with a ref taken) if the struct is still visible, or NULL if it's
	 * not. This prevents object-visibility races where someone is looking
	 * up an object as the last ref gets dropped; dropping the last ref and
	 * removing the object from the list are atomic with respect to getting
	 * a new ref. */
	struct qcusbnet *entry;
	mutex_lock(&qcusbnet_lock);
	list_for_each_entry(entry, &qcusbnet_list, node) {
		if (entry == key) {
			kref_get(&entry->refcount);
			mutex_unlock(&qcusbnet_lock);
			return entry;
		}
	}
	mutex_unlock(&qcusbnet_lock);
	return NULL;
}

struct qcusbnet *cdev_to_qcusbnet(struct cdev *cdev)
{
	struct qcusbnet *entry;
	mutex_lock(&qcusbnet_lock);
	list_for_each_entry(entry, &qcusbnet_list, node) {
		if (entry->qmi.cdev == cdev) {
			mutex_unlock(&qcusbnet_lock);
			return entry;
		}
	}
	mutex_unlock(&qcusbnet_lock);
	return NULL;
}

int qc_suspend(struct usb_interface *iface, pm_message_t event)
{
	struct usbnet *usbnet;
	struct qcusbnet *dev;
	int ret;

	if (!iface)
		return -ENOMEM;

	usbnet = usb_get_intfdata(iface);

	if (!usbnet || !usbnet->net) {
		ERR("failed to get netdevice\n");
		return -ENXIO;
	}

	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		ERR("failed to get QMIDevice\n");
		return -ENXIO;
	}

	if (!(event.event & PM_EVENT_AUTO)) {
		DBG("device suspended to power level %d\n",
		    event.event);
		qc_setdown(dev, DOWN_DRIVER_SUSPENDED);
	} else {
		DBG("device autosuspend\n");
	}

	if (event.event & PM_EVENT_SUSPEND) {
		qc_stopread(dev);
		usbnet->udev->reset_resume = 0;
		iface->dev.power.power_state.event = event.event;
	} else {
		usbnet->udev->reset_resume = 1;
	}

	ret = usbnet_suspend(iface, event);
	if (!ret && (event.event & PM_EVENT_AUTO)) {
		QC_WAKE_UNLOCK(&dev->wake_lock);
	}

	return ret;
}

static int qc_resume(struct usb_interface *iface)
{
	struct usbnet *usbnet;
	struct qcusbnet *dev;
	int ret;
	int oldstate;

	if (iface == 0)
		return -ENOMEM;

	usbnet = usb_get_intfdata(iface);

	if (!usbnet || !usbnet->net) {
		ERR("failed to get netdevice\n");
		return -ENXIO;
	}

	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		ERR("failed to get QMIDevice\n");
		return -ENXIO;
	}

	oldstate = iface->dev.power.power_state.event;
	iface->dev.power.power_state.event = PM_EVENT_ON;
	DBG("resuming from power mode %d\n", oldstate);

	if (oldstate & PM_EVENT_SUSPEND) {
		QC_WAKE_LOCK(&dev->wake_lock);
		qc_cleardown(dev, DOWN_DRIVER_SUSPENDED);
		netif_start_queue(usbnet->net);
		ret = usbnet_resume(iface);
		if (ret) {
			ERR("usbnet_resume error %d\n", ret);
			QC_WAKE_UNLOCK(&dev->wake_lock);
			return ret;
		}

		ret = qc_startread(dev);
		if (ret) {
			ERR("qc_startread error %d\n", ret);
			QC_WAKE_UNLOCK(&dev->wake_lock);
			return ret;
		}

		if (dev->worker.thread)
			wake_up_process(dev->worker.thread);

		/* Force autopm to schedule an auto suspend */
		usb_autopm_get_interface_no_resume(iface);
		usb_autopm_put_interface_async(iface);
	} else {
		DBG("nothing to resume\n");
		return 0;
	}

	return ret;
}

static int qc_reset_resume(struct usb_interface *iface)
{
	return qc_resume(iface);
}

static int qcnet_bind(struct usbnet *usbnet, struct usb_interface *iface)
{
	int numends;
	int i;
	struct usb_host_endpoint *endpoint = NULL;
	struct usb_host_endpoint *in = NULL;
	struct usb_host_endpoint *out = NULL;

	if (iface->num_altsetting != 1) {
		ERR("invalid num_altsetting %u\n", iface->num_altsetting);
		return -EINVAL;
	}

	numends = iface->cur_altsetting->desc.bNumEndpoints;
	for (i = 0; i < numends; i++) {
		endpoint = iface->cur_altsetting->endpoint + i;
		if (!endpoint) {
			ERR("invalid endpoint %u\n", i);
			return -EINVAL;
		}

		if (usb_endpoint_is_bulk_in(&endpoint->desc))
			in = endpoint;
		else if (usb_endpoint_is_bulk_out(&endpoint->desc))
			out = endpoint;
	}

	if (!in || !out) {
		ERR("invalid bulk endpoints\n");
		return -EINVAL;
	}

	if (usb_set_interface(usbnet->udev,
			      iface->cur_altsetting->desc.bInterfaceNumber, 0))	{
		ERR("unable to set interface\n");
		return -EINVAL;
	}

	usbnet->in = usb_rcvbulkpipe(usbnet->udev, in->desc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);
	usbnet->out = usb_sndbulkpipe(usbnet->udev, out->desc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);

	DBG("in %x, out %x\n",
	    in->desc.bEndpointAddress,
	    out->desc.bEndpointAddress);

	strcpy(usbnet->net->name, "qmi%d");
	random_ether_addr(&usbnet->net->dev_addr[0]);

	return 0;
}

#define XOOM_DOWNLINK_MTU 1500
static int xoom_qcnet_bind(struct usbnet *usbnet, struct usb_interface *iface)
{
	int status = qcnet_bind(usbnet, iface);

	if (!status) {
		usbnet->rx_urb_size = XOOM_DOWNLINK_MTU +
					usbnet->net->hard_header_len;
		usbnet->udev->autosuspend_delay = msecs_to_jiffies(1000);
		usbnet->udev->parent->autosuspend_delay = 0;
	}

	return status;
}

static void qcnet_unbind(struct usbnet *usbnet, struct usb_interface *iface)
{
	struct qcusbnet *dev = (struct qcusbnet *)usbnet->data[0];

	netif_carrier_off(usbnet->net);
	qc_deregister(dev);

	kfree(usbnet->net->netdev_ops);
	usbnet->net->netdev_ops = NULL;
	/* drop the list's ref */
	qcusbnet_put(dev);
}

static void qcnet_urbhook(struct urb *urb)
{
	unsigned long flags;
	struct worker *worker = urb->context;
	if (!worker) {
		ERR("bad context\n");
		return;
	}

	if (urb->status)
		ERR("urb finished with error %d\n", urb->status);

	spin_lock_irqsave(&worker->active_lock, flags);
	worker->active = ERR_PTR(-EAGAIN);
	spin_unlock_irqrestore(&worker->active_lock, flags);
	wake_up_process(worker->thread);
	usb_free_urb(urb);
}

static void qcnet_killactive(struct worker *worker)
{
	struct urb *active;
	unsigned long flags;

	spin_lock_irqsave(&worker->active_lock, flags);
	active = worker->active;
	if (IS_ERR_OR_NULL(active)) {
		spin_unlock_irqrestore(&worker->active_lock, flags);
		return;
	}
	usb_get_urb(active);
	spin_unlock_irqrestore(&worker->active_lock, flags);

	usb_kill_urb(active);
	usb_put_urb(active);
}

static void qcnet_txtimeout(struct net_device *netdev)
{
	struct list_head *node, *tmp;
	struct qcusbnet *dev;
	struct worker *worker;
	struct urbreq *req;
	unsigned long flags;
	struct usbnet *usbnet = netdev_priv(netdev);

	if (!usbnet || !usbnet->net) {
		ERR("failed to get usbnet device\n");
		return;
	}

	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		ERR("failed to get QMIDevice\n");
		return;
	}
	worker = &dev->worker;

	DBG("\n");

	qcnet_killactive(worker);

	spin_lock_irqsave(&worker->urbs_lock, flags);
	list_for_each_safe(node, tmp, &worker->urbs) {
		req = list_entry(node, struct urbreq, node);
		usb_free_urb(req->urb);
		list_del(&req->node);
		kfree(req);
	}
	spin_unlock_irqrestore(&worker->urbs_lock, flags);

	wake_up_process(worker->thread);
}

static int qcnet_worker(void *arg)
{
	struct list_head *node, *tmp;
	unsigned long activeflags, listflags;
	struct urbreq *req;
	int status;
        bool log_errors = 1;
	struct usb_device *usbdev;
	struct usb_interface *iface;
	struct worker *worker = arg;
	if (!worker) {
		ERR("passed null pointer\n");
		return -EINVAL;
	}

	iface = worker->iface;
	usbdev = interface_to_usbdev(iface);

	DBG("traffic thread started\n");

	while (1) {
		if (kthread_should_stop()) {
			qcnet_killactive(worker);

			spin_lock_irqsave(&worker->urbs_lock, listflags);
			list_for_each_safe(node, tmp, &worker->urbs) {
				req = list_entry(node, struct urbreq, node);
				usb_free_urb(req->urb);
				list_del(&req->node);
				kfree(req);
			}
			spin_unlock_irqrestore(&worker->urbs_lock, listflags);

			break;
		}

		set_current_state(TASK_INTERRUPTIBLE);

		spin_lock_irqsave(&worker->active_lock, activeflags);
		if (IS_ERR(worker->active) && PTR_ERR(worker->active) == -EAGAIN) {
			worker->active = NULL;
			spin_unlock_irqrestore(&worker->active_lock, activeflags);
			usb_autopm_put_interface(iface);
			spin_lock_irqsave(&worker->active_lock, activeflags);
		}

		if (worker->active) {
			spin_unlock_irqrestore(&worker->active_lock, activeflags);
			schedule();
			continue;
		}

		spin_lock_irqsave(&worker->urbs_lock, listflags);
		if (list_empty(&worker->urbs)) {
			spin_unlock_irqrestore(&worker->urbs_lock, listflags);
			spin_unlock_irqrestore(&worker->active_lock, activeflags);
			schedule();
			continue;
		}

		set_current_state(TASK_RUNNING);

		req = list_first_entry(&worker->urbs, struct urbreq, node);
		list_del(&req->node);
		spin_unlock_irqrestore(&worker->urbs_lock, listflags);

		worker->active = req->urb;
		spin_unlock_irqrestore(&worker->active_lock, activeflags);

		device_lock(&iface->dev);
		if (iface->dev.power.status >= DPM_OFF ||
				iface->dev.power.status == DPM_RESUMING) {
			usb_autopm_get_interface_no_resume(iface);
			status = 0;
		} else {
			status = usb_autopm_get_interface(iface);
		}
		device_unlock(&iface->dev);

		if (status < 0) {
                        if (log_errors) {
			    ERR("unable to autoresume interface: %d, power status: %d\n", status, iface->dev.power.status);
                            log_errors = 0;
                        }

			if (status == -EPERM)
				qc_suspend(iface, PMSG_SUSPEND);

			spin_lock_irqsave(&worker->urbs_lock, listflags);
			list_add(&req->node, &worker->urbs);
			spin_unlock_irqrestore(&worker->urbs_lock, listflags);

			spin_lock_irqsave(&worker->active_lock, activeflags);
			worker->active = NULL;
			spin_unlock_irqrestore(&worker->active_lock, activeflags);

                        msleep(20);
			continue;
		}

		status = usb_submit_urb(worker->active, GFP_KERNEL);
		if (status < 0) {
			ERR("Failed to submit URB: %d.  Packet dropped\n", status);
			spin_lock_irqsave(&worker->active_lock, activeflags);
			usb_free_urb(worker->active);
			worker->active = NULL;
			spin_unlock_irqrestore(&worker->active_lock, activeflags);
			usb_autopm_put_interface(iface);
			wake_up_process(worker->thread);
		}

		kfree(req);

                log_errors = 1;
	}

	DBG("traffic thread exiting\n");
	worker->thread = NULL;
	return 0;
}

static int qcnet_startxmit(struct sk_buff *skb, struct net_device *netdev)
{
	unsigned long listflags;
	struct qcusbnet *dev;
	struct worker *worker;
	struct urbreq *req;
	void *data;
	struct usbnet *usbnet = netdev_priv(netdev);

	DBG("\n");

	if (!usbnet || !usbnet->net) {
		ERR("failed to get usbnet device\n");
		return NETDEV_TX_BUSY;
	}

	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		ERR("failed to get QMIDevice\n");
		return NETDEV_TX_BUSY;
	}
	worker = &dev->worker;

	if (qc_isdown(dev, DOWN_DRIVER_SUSPENDED)) {
		ERR("device is suspended\n");
		netif_stop_queue(usbnet->net);
		if (qcusbnet_debug)
			dump_stack();
		return NETDEV_TX_BUSY;
	}

	req = kmalloc(sizeof(*req), GFP_ATOMIC);
	if (!req) {
		ERR("unable to allocate URBList memory\n");
		return NETDEV_TX_BUSY;
	}

	req->urb = usb_alloc_urb(0, GFP_ATOMIC);

	if (!req->urb) {
		kfree(req);
		ERR("unable to allocate URB\n");
		return NETDEV_TX_BUSY;
	}

	data = kmalloc(skb->len, GFP_ATOMIC);
	if (!data) {
		usb_free_urb(req->urb);
		kfree(req);
		ERR("unable to allocate URB data\n");
		return NETDEV_TX_BUSY;
	}
	memcpy(data, skb->data, skb->len);

	usb_fill_bulk_urb(req->urb, dev->usbnet->udev, dev->usbnet->out,
			  data, skb->len, qcnet_urbhook, worker);
	req->urb->transfer_flags |= URB_FREE_BUFFER;

	spin_lock_irqsave(&worker->urbs_lock, listflags);
	list_add_tail(&req->node, &worker->urbs);
	spin_unlock_irqrestore(&worker->urbs_lock, listflags);

	wake_up_process(worker->thread);

	netdev->trans_start = jiffies;
	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

static int qcnet_open(struct net_device *netdev)
{
	int status = 0;
	struct qcusbnet *dev;
	struct usbnet *usbnet = netdev_priv(netdev);

	if (!usbnet) {
		ERR("failed to get usbnet device\n");
		return -ENXIO;
	}

	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		ERR("failed to get QMIDevice\n");
		return -ENXIO;
	}

	DBG("\n");

	dev->worker.iface = dev->iface;
	INIT_LIST_HEAD(&dev->worker.urbs);
	dev->worker.active = NULL;
	spin_lock_init(&dev->worker.urbs_lock);
	spin_lock_init(&dev->worker.active_lock);

	dev->worker.thread = kthread_run(qcnet_worker, &dev->worker, "qcnet_worker");
	if (IS_ERR(dev->worker.thread)) {
		ERR("AutoPM thread creation error\n");
		return PTR_ERR(dev->worker.thread);
	}

	qc_cleardown(dev, DOWN_NET_IFACE_STOPPED);
	if (dev->open) {
		status = dev->open(netdev);
		if (status == 0)
			usb_autopm_put_interface(dev->iface);
	} else {
		ERR("no USBNetOpen defined\n");
	}

	return status;
}

int qcnet_stop(struct net_device *netdev)
{
	struct qcusbnet *dev;
	struct usbnet *usbnet = netdev_priv(netdev);

	if (!usbnet || !usbnet->net) {
		ERR("failed to get netdevice\n");
		return -ENXIO;
	}

	dev = (struct qcusbnet *)usbnet->data[0];
	if (!dev) {
		ERR("failed to get QMIDevice\n");
		return -ENXIO;
	}

	qc_setdown(dev, DOWN_NET_IFACE_STOPPED);
	kthread_stop(dev->worker.thread);
	DBG("thread stopped\n");

	if (dev->stop != NULL) {
		/* stop interface will do a put on this iface */
		usb_autopm_get_interface_no_resume(dev->iface);
		return dev->stop(netdev);
	}
	return 0;
}

static const struct driver_info qc_netinfo = {
	.description   = "QCUSBNet Ethernet Device",
	.flags         = FLAG_ETHER,
	.bind          = qcnet_bind,
	.unbind        = qcnet_unbind,
	.data          = 0,
};

static const struct driver_info xoom_qc_netinfo = {
	.description   = "Xoom QCUSBNet Ethernet Device",
	.flags         = FLAG_ETHER,
	.bind          = xoom_qcnet_bind,
	.unbind        = qcnet_unbind,
	.data          = 0,
};

#define MKVIDPID(v, p)					\
{							\
	USB_DEVICE(v, p),				\
	.driver_info = (unsigned long)&qc_netinfo,	\
}

static const struct usb_device_id qc_vidpids[] = {
	MKVIDPID(0x05c6, 0x9215),	/* Acer Gobi 2000 */
	MKVIDPID(0x05c6, 0x9265),	/* Asus Gobi 2000 */
	MKVIDPID(0x16d8, 0x8002),	/* CMOTech Gobi 2000 */
	MKVIDPID(0x413c, 0x8186),	/* Dell Gobi 2000 */
	MKVIDPID(0x1410, 0xa010),	/* Entourage Gobi 2000 */
	MKVIDPID(0x1410, 0xa011),	/* Entourage Gobi 2000 */
	MKVIDPID(0x1410, 0xa012),	/* Entourage Gobi 2000 */
	MKVIDPID(0x1410, 0xa013),	/* Entourage Gobi 2000 */
	MKVIDPID(0x03f0, 0x251d),	/* HP Gobi 2000 */
	MKVIDPID(0x05c6, 0x9205),	/* Lenovo Gobi 2000 */
	MKVIDPID(0x05c6, 0x920b),	/* Generic Gobi 2000 */
	MKVIDPID(0x04da, 0x250f),	/* Panasonic Gobi 2000 */
	MKVIDPID(0x05c6, 0x9245),	/* Samsung Gobi 2000 */
	MKVIDPID(0x1199, 0x9001),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9002),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9003),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9004),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9005),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9006),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9007),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9008),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x9009),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x1199, 0x900a),	/* Sierra Wireless Gobi 2000 */
	MKVIDPID(0x05c6, 0x9225),	/* Sony Gobi 2000 */
	MKVIDPID(0x05c6, 0x9235),	/* Top Global Gobi 2000 */
	MKVIDPID(0x05c6, 0x9275),	/* iRex Technologies Gobi 2000 */

	MKVIDPID(0x05c6, 0x920d),	/* Qualcomm Gobi 3000 */
	MKVIDPID(0x1410, 0xa021),	/* Novatel Gobi 3000 */

	{
		USB_DEVICE_AND_INTERFACE_INFO(0x22B8, 0x2A70, 0xff, 0xfb, 0xff), /* Motorola Xoom */
		.driver_info = (unsigned long)&xoom_qc_netinfo
	},
	{ }
};

MODULE_DEVICE_TABLE(usb, qc_vidpids);

int qcnet_probe(struct usb_interface *iface, const struct usb_device_id *vidpids)
{
	int status;
	struct usbnet *usbnet;
	struct qcusbnet *dev;
	struct net_device_ops *netdevops;

	status = usbnet_probe(iface, vidpids);
	if (status < 0) {
		ERR("usbnet_probe failed %d\n", status);
		return status;
	}

	usbnet = usb_get_intfdata(iface);

	if (!usbnet || !usbnet->net) {
		ERR("failed to get netdevice\n");
		return -ENXIO;
	}

	dev = kzalloc(sizeof(struct qcusbnet), GFP_KERNEL);
	if (!dev) {
		ERR("failed to allocate device buffers\n");
		return -ENOMEM;
	}
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock_init(&dev->wake_lock, WAKE_LOCK_SUSPEND,
		iface->dev.driver->name);
#endif /* CONFIG_HAS_WAKELOCK */


	usbnet->data[0] = (unsigned long)dev;

	dev->usbnet = usbnet;

	netdevops = kmalloc(sizeof(struct net_device_ops), GFP_KERNEL);
	if (!netdevops) {
		ERR("failed to allocate net device ops\n");
		return -ENOMEM;
	}
	memcpy(netdevops, usbnet->net->netdev_ops, sizeof(struct net_device_ops));

	dev->open = netdevops->ndo_open;
	netdevops->ndo_open = qcnet_open;
	dev->stop = netdevops->ndo_stop;
	netdevops->ndo_stop = qcnet_stop;
	netdevops->ndo_start_xmit = qcnet_startxmit;
	netdevops->ndo_tx_timeout = qcnet_txtimeout;

	usbnet->net->netdev_ops = netdevops;

	memset(&(dev->usbnet->net->stats), 0, sizeof(struct net_device_stats));

	dev->iface = iface;
	memset(&(dev->meid), '0', 14);

	mutex_init(&dev->mutex);
	dev->valid = false;
	memset(&dev->qmi, 0, sizeof(dev->qmi));

	dev->qmi.devclass = devclass;

	kref_init(&dev->refcount);
	INIT_LIST_HEAD(&dev->node);
	INIT_LIST_HEAD(&dev->qmi.clients);
	spin_lock_init(&dev->qmi.clients_lock);

	dev->down = 0;
	qc_setdown(dev, DOWN_NO_NDIS_CONNECTION);
	qc_setdown(dev, DOWN_NET_IFACE_STOPPED);

	status = qc_register(dev);
	if (status)
		return status;

	mutex_lock(&qcusbnet_lock);
	/* Give our initial ref to the list */
	list_add(&dev->node, &qcusbnet_list);
	mutex_unlock(&qcusbnet_lock);

	return status;
}
EXPORT_SYMBOL_GPL(qcnet_probe);

static struct usb_driver qcusbnet = {
	.name		= "QCUSBNet2k",
	.id_table	= qc_vidpids,
	.probe		= qcnet_probe,
	.disconnect	= usbnet_disconnect,
	.suspend	= qc_suspend,
	.resume		= qc_resume,
	.reset_resume	= qc_reset_resume,
	.supports_autosuspend = true,
};

static int __init modinit(void)
{
	devclass = class_create(THIS_MODULE, "QCQMI");
	if (IS_ERR(devclass)) {
		ERR("error at class_create %ld\n", PTR_ERR(devclass));
		return -ENOMEM;
	}
	printk(KERN_INFO "%s: %s\n", DRIVER_DESC, DRIVER_VERSION);
	return usb_register(&qcusbnet);
}
module_init(modinit);

static void __exit modexit(void)
{
	usb_deregister(&qcusbnet);
	class_destroy(devclass);
}
module_exit(modexit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("Dual BSD/GPL");

module_param(qcusbnet_debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(qcusbnet_debug, "Debugging enabled or not");

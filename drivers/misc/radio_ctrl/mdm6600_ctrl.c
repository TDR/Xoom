/*
     Copyright (C) 2010 Motorola, Inc.

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation.

     This program is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
     GNU General Public License for more details.

     You should have received a copy of the GNU General Public License
     along with this program; if not, write to the Free Software
     Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
     02111-1307  USA
*/
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/radio_ctrl/mdm6600_ctrl.h>
#include <linux/radio_ctrl/radio_class.h>

#define AP_STATUS_BP_PANIC_ACK      0x00
#define AP_STATUS_DATA_ONLY_BYPASS  0x01
#define AP_STATUS_FULL_BYPASS       0x02
#define AP_STATUS_NO_BYPASS         0x03
#define AP_STATUS_BP_SHUTDOWN_REQ   0x04
#define AP_STATUS_UNDEFINED         0x07

#define BP_STATUS_PANIC             0x00
#define BP_STATUS_PANIC_BUSY_WAIT   0x01
#define BP_STATUS_QC_DLOAD          0x02
#define BP_STATUS_RAM_DOWNLOADER    0x03
#define BP_STATUS_PHONE_CODE_AWAKE  0x04
#define BP_STATUS_PHONE_CODE_ASLEEP 0x05
#define BP_STATUS_SHUTDOWN_ACK      0x06
#define BP_STATUS_UNDEFINED         0x07


#define LOOP_DELAY_TIME_MS          100

static const char *mdmctrl = "mdm6600_ctrl";

static const char *bp_status[8] = {
	[BP_STATUS_PANIC] = "panic",
	[BP_STATUS_PANIC_BUSY_WAIT] = "panic busy wait",
	[BP_STATUS_QC_DLOAD] = "qc dload",
	[BP_STATUS_RAM_DOWNLOADER] = "ram downloader",
	[BP_STATUS_PHONE_CODE_AWAKE] = "awake",
	[BP_STATUS_PHONE_CODE_ASLEEP] = "asleep",
	[BP_STATUS_SHUTDOWN_ACK] = "shutdown ack",
	[BP_STATUS_UNDEFINED] = "undefined",
};

static const char *bp_power_state[2] = {
	"off",
	"on",
};

#define BP_STATUS_MAX_LENGTH        32
#define BP_COMMAND_MAX_LENGTH       32

/* structure to keep track of gpio, irq, and irq enabled info */
struct gpio_info {
	int irq;
	struct work_struct work;
};

struct mdm_ctrl_info {
	struct mdm_ctrl_platform_data *pdata;
	struct gpio_info gpios[MDM_CTRL_NUM_GPIOS];
};

static struct mdm_ctrl_info mdm_ctrl;

static DEFINE_MUTEX(mdm_ctrl_info_lock);

struct workqueue_struct *working_queue = NULL;

struct class *radio_cls = NULL;
static struct radio_dev radio_cdev;

static unsigned int bp_status_idx = BP_STATUS_UNDEFINED;
static unsigned int bp_power_idx = 0;

static void __devexit mdm_ctrl_shutdown(struct platform_device *pdev);
static void mdm_ctrl_powerup(void);
static void mdm_ctrl_set_bootmode(int mode);
static void mdm_ctrl_dump_log(void);

static const char *bp_status_string(unsigned int stat)
{
	if (stat < ARRAY_SIZE(bp_status))
		return bp_status[stat];
	else
		return "status out of range";
}

static const char *bp_power_state_string(unsigned int stat)
{
	if (stat < ARRAY_SIZE(bp_power_state))
		return bp_power_state[stat];
	else
		return "status out of range";
}

static ssize_t mdm_status_show(struct radio_dev *dev, char *buff)
{
	ssize_t status = 0;
	status = snprintf(buff, BP_STATUS_MAX_LENGTH, "%s\n",
			  bp_status_string(bp_status_idx));

	return status;
}

static ssize_t mdm_power_show(struct radio_dev *rdev, char *buff)
{
	ssize_t status = 0;
	status = snprintf(buff, BP_STATUS_MAX_LENGTH, "%s\n",
			  bp_power_state_string(bp_power_idx));

	return status;
}

static ssize_t mdm_user_command(struct radio_dev *rdev, char *post_strip)
{

	pr_info("%s: user command = %s\n", mdmctrl, post_strip);

	if (strcmp(post_strip,"shutdown") == 0) {
		mdm_ctrl_shutdown(NULL);
	} else if (strcmp(post_strip,"powerup") == 0) {
		mdm_ctrl_powerup();
	} else if (strcmp(post_strip,"bootmode_normal") == 0) {
		mdm_ctrl_set_bootmode(0);
	} else if (strcmp(post_strip,"bootmode_flash") == 0) {
		mdm_ctrl_set_bootmode(1);
	} else if (strcmp(post_strip,"dump_log") == 0) {
		mdm_ctrl_dump_log();
	} else {
		return -EINVAL;
	}

	return 0;
}

static unsigned int mdm_gpio_get_value(struct mdm_ctrl_gpio gpio)
{
	return gpio_get_value(gpio.number);
}

static void mdm_gpio_set_value(struct mdm_ctrl_gpio gpio,
	unsigned int value)
{
	gpio_set_value(gpio.number, value);
}

static void mdm_gpio_free(struct mdm_ctrl_gpio *gpio)
{
	if (gpio->allocated)
		gpio_free(gpio->number);
	gpio->allocated = 0;
}

static int mdm_gpio_setup(struct mdm_ctrl_gpio *gpio)
{
	if (gpio_request(gpio->number, gpio->name))  {
		printk(KERN_ERR "failed to aquire gpio %s", gpio->name);
		return -1;
	}
	gpio->allocated = 1;
	gpio_export(gpio->number, false);
	if (gpio->direction == MDM_GPIO_DIRECTION_IN)
		gpio_direction_input(gpio->number);
	else if (gpio->direction == MDM_GPIO_DIRECTION_OUT)
		gpio_direction_output(gpio->number, gpio->default_value);
	return 0;
}

static unsigned int get_bp_status(void)
{
	unsigned int status = BP_STATUS_UNDEFINED;
	unsigned int bp_status[3] = {0};

	mutex_lock(&mdm_ctrl_info_lock);
	if (mdm_ctrl.pdata) {
		bp_status[0] = mdm_gpio_get_value(
			mdm_ctrl.pdata->gpios[MDM_CTRL_GPIO_BP_STATUS_0]);
		bp_status[1] = mdm_gpio_get_value(
			mdm_ctrl.pdata->gpios[MDM_CTRL_GPIO_BP_STATUS_1]);
		bp_status[2] = mdm_gpio_get_value(
			mdm_ctrl.pdata->gpios[MDM_CTRL_GPIO_BP_STATUS_2]);
	}
	mutex_unlock(&mdm_ctrl_info_lock);

	status = ((bp_status[2] & 0x1) << 2) |
		 ((bp_status[1] & 0x1) << 1) |
		  (bp_status[0] & 0x1);

	return status;
}

static unsigned int get_bp_power_status(void)
{
	unsigned int status = 0;

	mutex_lock(&mdm_ctrl_info_lock);
	if (mdm_ctrl.pdata) {
		status = mdm_gpio_get_value(
			mdm_ctrl.pdata->gpios[MDM_CTRL_GPIO_BP_RESOUT]);
	}

	mutex_unlock(&mdm_ctrl_info_lock);

	return status & 0x1;
}

static unsigned int get_ap_status(void)
{
	unsigned int status = AP_STATUS_UNDEFINED;
	unsigned int ap_status[3] =  {0};

	mutex_lock(&mdm_ctrl_info_lock);
	if (mdm_ctrl.pdata) {
		ap_status[0] = mdm_gpio_get_value(
			mdm_ctrl.pdata->gpios[MDM_CTRL_GPIO_AP_STATUS_0]);
		ap_status[1] = mdm_gpio_get_value(
			mdm_ctrl.pdata->gpios[MDM_CTRL_GPIO_AP_STATUS_1]);
		ap_status[2] = mdm_gpio_get_value(
			mdm_ctrl.pdata->gpios[MDM_CTRL_GPIO_AP_STATUS_2]);
	}
	mutex_unlock(&mdm_ctrl_info_lock);

	status = ((ap_status[2] & 0x1) << 2) |
		 ((ap_status[1] & 0x1) << 1) |
		  (ap_status[0] & 0x1);

	return status;
}

static void set_ap_status(unsigned int status)
{
	mutex_lock(&mdm_ctrl_info_lock);
	if (mdm_ctrl.pdata) {
		mdm_gpio_set_value(
			mdm_ctrl.pdata->gpios[MDM_CTRL_GPIO_AP_STATUS_0],
			(status & 0x1));
		mdm_gpio_set_value(
			mdm_ctrl.pdata->gpios[MDM_CTRL_GPIO_AP_STATUS_1],
			(status >> 1) & 0x1);
		mdm_gpio_set_value(
			mdm_ctrl.pdata->gpios[MDM_CTRL_GPIO_AP_STATUS_2],
			(status >> 2) & 0x1);
	}
	mutex_unlock(&mdm_ctrl_info_lock);
}

static void set_bp_pwron(int on)
{
	mutex_lock(&mdm_ctrl_info_lock);
	if ((mdm_ctrl.pdata) && ((on == 1) || (on == 0))) {
		mdm_gpio_set_value(
			mdm_ctrl.pdata->gpios[MDM_CTRL_GPIO_BP_PWRON],
			on);
	}
	mutex_unlock(&mdm_ctrl_info_lock);
}

static void set_bp_resin(int on)
{
	mutex_lock(&mdm_ctrl_info_lock);
	if ((mdm_ctrl.pdata) && ((on == 1) || (on == 0))) {
		mdm_gpio_set_value(
			mdm_ctrl.pdata->gpios[MDM_CTRL_GPIO_BP_RESIN],
			on);
	}
	mutex_unlock(&mdm_ctrl_info_lock);
}

static void update_bp_status(void) {

	static int bp_status_prev_idx = BP_STATUS_UNDEFINED;

	bp_status_prev_idx = bp_status_idx;
	bp_status_idx = get_bp_status();
	bp_power_idx = get_bp_power_status();

	pr_info("%s: modem status: %s -> %s [power %s]", mdmctrl,
		bp_status_string(bp_status_prev_idx),
		bp_status_string(bp_status_idx),
		bp_power_state_string(bp_power_idx));

	kobject_uevent(&radio_cdev.dev->kobj, KOBJ_CHANGE);
}

static void mdm_ctrl_powerup(void)
{
	unsigned int bp_status;

	pr_info("%s: Starting up modem.", mdmctrl);

	bp_status = get_bp_status();
	pr_info("%s: Initial Modem status %s [0x%x]",
		mdmctrl, bp_status_string(bp_status), bp_status);

	set_ap_status(AP_STATUS_NO_BYPASS);
	pr_info("%s: ap_status set to %d", mdmctrl, get_ap_status());
	msleep(100);
	set_bp_resin(0);
	msleep(100);
	/* Toggle the power, delaying to allow modem to respond */
	set_bp_pwron(1);
	msleep(100);
	set_bp_pwron(0);

	/* now let user handles bp status change through uevent */
}

static void mdm_ctrl_set_bootmode(int mode)
{
	mutex_lock(&mdm_ctrl_info_lock);
	if (mdm_ctrl.pdata && ((mode == 0) || (mode == 1))) {
		gpio_request(mdm_ctrl.pdata->cmd_gpios.cmd1,
			     "BP Command 1");
		gpio_direction_output(mdm_ctrl.pdata->cmd_gpios.cmd1,
				      mode);
		gpio_request(mdm_ctrl.pdata->cmd_gpios.cmd2,
			     "BP Command 2");
		gpio_direction_output(mdm_ctrl.pdata->cmd_gpios.cmd2,
				      mode);

	}
	mutex_unlock(&mdm_ctrl_info_lock);
}

static void irq_worker(struct work_struct *work)
{
	struct gpio_info *gpio = container_of(work, struct gpio_info, work);
	update_bp_status();
	enable_irq(gpio->irq);
}

static irqreturn_t irq_handler(int irq, void *data)
{
	struct gpio_info *gpio = (struct gpio_info *) data;

	disable_irq_nosync(irq);
	queue_work(working_queue, &gpio->work);

	return IRQ_HANDLED;
}

static int mdm_gpio_setup_internal(struct mdm_ctrl_platform_data *pdata)
{
	int i;
	int rv = 0;
	struct gpio_info *gpio_data = NULL;

	mutex_lock(&mdm_ctrl_info_lock);
	memset(&mdm_ctrl, 0, sizeof (mdm_ctrl));

	mdm_ctrl.pdata = pdata;

	for (i = 0; i < MDM_CTRL_NUM_GPIOS; i++) {
		gpio_data = &mdm_ctrl.gpios[i];
		if (pdata->gpios[i].direction == MDM_GPIO_DIRECTION_IN) {
			INIT_WORK(&gpio_data->work, irq_worker);
			gpio_data->irq = gpio_to_irq(pdata->gpios[i].number);
			rv = request_irq(gpio_data->irq, irq_handler,
					 IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING,
					 pdata->gpios[i].name, gpio_data);
                        if (rv < 0) {
				pr_err("%s: Cannot request IRQ (%d) from kernel!",
				       mdmctrl, gpio_data->irq);
			} else {
				enable_irq_wake(gpio_data->irq);
			}
		}
        }

	mutex_unlock(&mdm_ctrl_info_lock);
	return rv;
}

static void mdm_gpio_cleanup_internal(void)
{
	int i;
	struct gpio_info *gpio_data = NULL;

	mutex_lock(&mdm_ctrl_info_lock);

	for (i = 0; i < MDM_CTRL_NUM_GPIOS; i++) {
		gpio_data = &mdm_ctrl.gpios[i];

		if (gpio_data->irq) {
			disable_irq_wake(gpio_data->irq);
			free_irq(gpio_data->irq, gpio_data);
		}
	}
	memset(&mdm_ctrl, 0, sizeof (mdm_ctrl));
	mutex_unlock(&mdm_ctrl_info_lock);
}

static struct radio_dev radio_cdev = {
	.name = "mdm6600",
	.power_status = mdm_power_show,
	.status = mdm_status_show,
        .command = mdm_user_command,
};

static int __devinit mdm_ctrl_probe(struct platform_device *pdev)
{
	int i;
	struct mdm_ctrl_platform_data *pdata = pdev->dev.platform_data;

	dev_info(&pdev->dev, "mdm_ctrl_probe");

	pr_debug("%s: radio_cdev = %p\n", __func__, &radio_cdev);

	for (i = 0; i < MDM_CTRL_NUM_GPIOS; i++) {
		if (mdm_gpio_setup(&pdata->gpios[i])) {
			dev_err(&pdev->dev, "failed to aquire gpio %d\n",
				pdata->gpios[i].number);
			goto probe_cleanup;
		}
	}

	working_queue = create_singlethread_workqueue("mdm_ctrl_wq");
	if (!working_queue) {
		dev_err(&pdev->dev, "Cannot create work queue.");
		goto probe_err;
	}

	if (mdm_gpio_setup_internal(pdata) < 0) {
		dev_err(&pdev->dev, "Failed to setup bp  status irq");
		goto err_setup;
	}

	if (radio_dev_register(&radio_cdev)) {
		pr_err("%s: failed to register mdm_ctr device\n", __func__);
		goto err_setup;
	}

	update_bp_status();

	return 0;

err_setup:
	mdm_gpio_cleanup_internal();

probe_err:
	destroy_workqueue(working_queue);

probe_cleanup:
	for (i = 0; i < MDM_CTRL_NUM_GPIOS; i++)
		mdm_gpio_free(&pdata->gpios[i]);

	return -1;
}

static int __devexit mdm_ctrl_remove(struct platform_device *pdev)
{
	int i;
	struct mdm_ctrl_platform_data *pdata = pdev->dev.platform_data;

	dev_info(&pdev->dev, "cleanup\n");

	radio_dev_unregister(&radio_cdev);

	mdm_gpio_cleanup_internal();

        if (working_queue)
		destroy_workqueue(working_queue);

	for (i = 0; i < MDM_CTRL_NUM_GPIOS; i++)
		mdm_gpio_free(&pdata->gpios[i]);

	return 0;
}

static unsigned int __devexit bp_shutdown_wait(unsigned int delay_sec)
{
	unsigned int i, loop_count;
	unsigned int bp_status;
	unsigned int gpio_value;
	unsigned int bp_pd_ack = 0;
	unsigned int pd_failure = 1;

	loop_count = (delay_sec * 1000) / LOOP_DELAY_TIME_MS;

	for (i = 0; i < loop_count; i++) {
		if (!bp_pd_ack) {
			bp_status = get_bp_status();
			if (bp_status == BP_STATUS_SHUTDOWN_ACK) {
				pr_info("%s: Modem ack'd power off.", mdmctrl);
				bp_pd_ack = 1;
				set_bp_pwron(0);
			}
		}

		gpio_value = get_bp_power_status();
		if (gpio_value == 0) {
			pr_info("%s: Modem powered off.", mdmctrl);
			pd_failure = 0;
			break;
		}
		msleep(LOOP_DELAY_TIME_MS);
	}
	set_bp_pwron(0);
	return pd_failure;
}

static void __devexit mdm_ctrl_shutdown(struct platform_device *pdev)
{
	unsigned int pd_failure;
	unsigned int bp_status;

	pr_info("%s: Shutting down modem.", mdmctrl);

	bp_status = get_bp_status();
	pr_info("%s: Initial Modem status %s [0x%x]",
		mdmctrl, bp_status_string(bp_status), bp_status);

	/* Do an initial check of BP power before attempting shutdown */
	pd_failure = get_bp_power_status();
	if (!pd_failure) {
		pr_err("%s: Modem powered off before shutdown.", mdmctrl);
		/* Force uevent update */
		/* If we hit this case, there is a status     */
		/* misalignment between user space and kernel */
		update_bp_status();
	} else {
		set_ap_status(AP_STATUS_BP_SHUTDOWN_REQ);

		/* Allow modem to process status */
		msleep(100);
		pr_info("%s: ap_status set to %d", mdmctrl, get_ap_status());

		/* Assert PWRON to trigger modem to shutdown */
		/* until acknowledged or wait times out */
		set_bp_pwron(1);
		msleep(100);

		/* This should be enough to power down the modem */
		/* if this doesn't work, reset the modem and try */
		/* one more time, ultimately the modem will be   */
		/* hard powered off */
		pd_failure = bp_shutdown_wait(5);
		if (pd_failure) {
			pr_info("%s: Resetting unresponsive modem.", mdmctrl);
			set_bp_resin(1);
			pd_failure = bp_shutdown_wait(5);
		}
	}

	if (pd_failure)
		pr_err("%s: Modem failed to power down.", mdmctrl);
}

static void mdm_ctrl_dump_log(void)
{
	pr_info("%s: Dumping modem log", mdmctrl);

	/* To implement the dump, the BP expects BP_PWRON to be asserted
	 * while the AP status pins remain normal.
	 */
	set_bp_pwron(1);
	msleep(100);
	set_bp_pwron(0);

	/* Allow enough time for the log to dump fully to EFS on the BP,
	 * so that users of this don't have to wait themselves.
	 */
	msleep(500);
}

static struct platform_driver mdm6x00_ctrl_driver = {
	.probe = mdm_ctrl_probe,
	.remove = __devexit_p(mdm_ctrl_remove),
	.shutdown = __devexit_p(mdm_ctrl_shutdown),
	.driver = {
		.name = MDM_CTRL_MODULE_NAME,
		.owner = THIS_MODULE,
	},
};

static int __init mdm6600_ctrl_init(void)
{
	printk(KERN_DEBUG "mdm6600_ctrl_init\n");
	return platform_driver_register(&mdm6x00_ctrl_driver);
}

static void __exit mdm6600_ctrl_exit(void)
{
	printk(KERN_DEBUG "mdm6600_ctrl_exit\n");
	platform_driver_unregister(&mdm6x00_ctrl_driver);
}

module_init(mdm6600_ctrl_init);
module_exit(mdm6600_ctrl_exit);

MODULE_AUTHOR("Motorola");
MODULE_DESCRIPTION("MDM6X00 Control Driver");
MODULE_VERSION("1.1.4");
MODULE_LICENSE("GPL");

/*
 * Driver for keys on GPIO lines capable of generating interrupts.
 *
 * Copyright 2005 Phil Blundell
 * Copyright 2010, 2011 David Jander <david@protonic.nl>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/gpio_keys.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>


#define SUSPEND_DEDGE_TEST	1

extern int gSleep_Mode_Suspend;

struct gpio_button_data {
	const struct gpio_keys_button *button;
	struct input_dev *input;
	struct gpio_desc *gpiod;

	struct timer_list release_timer;
	unsigned int release_delay;	/* in msecs, for IRQ-only buttons */

	struct delayed_work work;
	unsigned int software_debounce;	/* in msecs, for GPIO-driven buttons */

	unsigned int irq;
	spinlock_t lock;
	bool disabled;
	int key_pressed;
	char cDeepSlp_wake_enable_name_bufA[128];
	struct device_attribute attr_deepslp_wake_en;
	int deep_sleep_wakeup;
	unsigned long report_jiffies;
	int interrupt_state;
	irq_handler_t isr;
	unsigned long irqflags;
	
};

struct gpio_keys_drvdata {
	const struct gpio_keys_platform_data *pdata;
	struct input_dev *input;
	struct mutex disable_lock;
	struct gpio_button_data data[0];
};

struct gpio_keys_drvdata *gt_gpio_keys_ddata;
struct pinctrl *pinctrl;
struct pinctrl_state *pin_state_active;
struct pinctrl_state *pin_state_sleep;
struct pinctrl_state *pin_state_deepsleep;
struct pinctrl_state *pin_state_current;
struct pinctrl_state *pin_state_select;
/*
 * SYSFS interface for enabling/disabling keys and switches:
 *
 * There are 4 attributes under /sys/devices/platform/gpio-keys/
 *	keys [ro]              - bitmap of keys (EV_KEY) which can be
 *	                         disabled
 *	switches [ro]          - bitmap of switches (EV_SW) which can be
 *	                         disabled
 *	disabled_keys [rw]     - bitmap of keys currently disabled
 *	disabled_switches [rw] - bitmap of switches currently disabled
 *
 * Userland can change these values and hence disable event generation
 * for each key (or switch). Disabling a key means its interrupt line
 * is disabled.
 *
 * For example, if we have following switches set up as gpio-keys:
 *	SW_DOCK = 5
 *	SW_CAMERA_LENS_COVER = 9
 *	SW_KEYPAD_SLIDE = 10
 *	SW_FRONT_PROXIMITY = 11
 * This is read from switches:
 *	11-9,5
 * Next we want to disable proximity (11) and dock (5), we write:
 *	11,5
 * to file disabled_switches. Now proximity and dock IRQs are disabled.
 * This can be verified by reading the file disabled_switches:
 *	11,5
 * If we now want to enable proximity (11) switch we write:
 *	5
 * to disabled_switches.
 *
 * We can disable only those keys which don't allow sharing the irq.
 */

/**
 * get_n_events_by_type() - returns maximum number of events per @type
 * @type: type of button (%EV_KEY, %EV_SW)
 *
 * Return value of this function can be used to allocate bitmap
 * large enough to hold all bits for given type.
 */
static int get_n_events_by_type(int type)
{
	BUG_ON(type != EV_SW && type != EV_KEY);

	return (type == EV_KEY) ? KEY_CNT : SW_CNT;
}

/**
 * get_bm_events_by_type() - returns bitmap of supported events per @type
 * @input: input device from which bitmap is retrieved
 * @type: type of button (%EV_KEY, %EV_SW)
 *
 * Return value of this function can be used to allocate bitmap
 * large enough to hold all bits for given type.
 */
static const unsigned long *get_bm_events_by_type(struct input_dev *dev,
						  int type)
{
	BUG_ON(type != EV_SW && type != EV_KEY);

	return (type == EV_KEY) ? dev->keybit : dev->swbit;
}

/**
 * gpio_keys_disable_button() - disables given GPIO button
 * @bdata: button data for button to be disabled
 *
 * Disables button pointed by @bdata. This is done by masking
 * IRQ line. After this function is called, button won't generate
 * input events anymore. Note that one can only disable buttons
 * that don't share IRQs.
 *
 * Make sure that @bdata->disable_lock is locked when entering
 * this function to avoid races when concurrent threads are
 * disabling buttons at the same time.
 */
static void gpio_keys_disable_button(struct gpio_button_data *bdata)
{
	if (!bdata->disabled) {
		/*
		 * Disable IRQ and associated timer/work structure.
		 */
		pr_info("%s():irq=%d\n",__func__,bdata->irq);
		disable_irq(bdata->irq);

		if (bdata->gpiod)
			cancel_delayed_work_sync(&bdata->work);
		else
			del_timer_sync(&bdata->release_timer);

		bdata->disabled = true;
	}
}

/**
 * gpio_keys_enable_button() - enables given GPIO button
 * @bdata: button data for button to be disabled
 *
 * Enables given button pointed by @bdata.
 *
 * Make sure that @bdata->disable_lock is locked when entering
 * this function to avoid races with concurrent threads trying
 * to enable the same button at the same time.
 */
static void gpio_keys_enable_button(struct gpio_button_data *bdata)
{
	if (bdata->disabled) {
		pr_info("%s():irq=%d\n",__func__,bdata->irq);
		enable_irq(bdata->irq);
		bdata->disabled = false;
	}
}

/**
 * gpio_keys_attr_show_helper() - fill in stringified bitmap of buttons
 * @ddata: pointer to drvdata
 * @buf: buffer where stringified bitmap is written
 * @type: button type (%EV_KEY, %EV_SW)
 * @only_disabled: does caller want only those buttons that are
 *                 currently disabled or all buttons that can be
 *                 disabled
 *
 * This function writes buttons that can be disabled to @buf. If
 * @only_disabled is true, then @buf contains only those buttons
 * that are currently disabled. Returns 0 on success or negative
 * errno on failure.
 */
static ssize_t gpio_keys_attr_show_helper(struct gpio_keys_drvdata *ddata,
					  char *buf, unsigned int type,
					  bool only_disabled)
{
	int n_events = get_n_events_by_type(type);
	unsigned long *bits;
	ssize_t ret;
	int i;

	bits = kcalloc(BITS_TO_LONGS(n_events), sizeof(*bits), GFP_KERNEL);
	if (!bits)
		return -ENOMEM;

	for (i = 0; i < ddata->pdata->nbuttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];

		if (bdata->button->type != type)
			continue;

		if (only_disabled && !bdata->disabled)
			continue;

		__set_bit(bdata->button->code, bits);
	}

	ret = scnprintf(buf, PAGE_SIZE - 1, "%*pbl", n_events, bits);
	buf[ret++] = '\n';
	buf[ret] = '\0';

	kfree(bits);

	return ret;
}

/**
 * gpio_keys_attr_store_helper() - enable/disable buttons based on given bitmap
 * @ddata: pointer to drvdata
 * @buf: buffer from userspace that contains stringified bitmap
 * @type: button type (%EV_KEY, %EV_SW)
 *
 * This function parses stringified bitmap from @buf and disables/enables
 * GPIO buttons accordingly. Returns 0 on success and negative error
 * on failure.
 */
static ssize_t gpio_keys_attr_store_helper(struct gpio_keys_drvdata *ddata,
					   const char *buf, unsigned int type)
{
	int n_events = get_n_events_by_type(type);
	const unsigned long *bitmap = get_bm_events_by_type(ddata->input, type);
	unsigned long *bits;
	ssize_t error;
	int i;

	bits = kcalloc(BITS_TO_LONGS(n_events), sizeof(*bits), GFP_KERNEL);
	if (!bits)
		return -ENOMEM;

	error = bitmap_parselist(buf, bits, n_events);
	if (error)
		goto out;

	/* First validate */
	if (!bitmap_subset(bits, bitmap, n_events)) {
		error = -EINVAL;
		goto out;
	}

	for (i = 0; i < ddata->pdata->nbuttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];

		if (bdata->button->type != type)
			continue;

		if (test_bit(bdata->button->code, bits) &&
		    !bdata->button->can_disable) {
			error = -EINVAL;
			goto out;
		}
	}

	mutex_lock(&ddata->disable_lock);

	for (i = 0; i < ddata->pdata->nbuttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];

		if (bdata->button->type != type)
			continue;

		if (test_bit(bdata->button->code, bits))
			gpio_keys_disable_button(bdata);
		else
			gpio_keys_enable_button(bdata);
	}

	mutex_unlock(&ddata->disable_lock);

out:
	kfree(bits);
	return error;
}

#define ATTR_SHOW_FN(name, type, only_disabled)				\
static ssize_t gpio_keys_show_##name(struct device *dev,		\
				     struct device_attribute *attr,	\
				     char *buf)				\
{									\
	struct platform_device *pdev = to_platform_device(dev);		\
	struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);	\
									\
	return gpio_keys_attr_show_helper(ddata, buf,			\
					  type, only_disabled);		\
}

ATTR_SHOW_FN(keys, EV_KEY, false);
ATTR_SHOW_FN(switches, EV_SW, false);
ATTR_SHOW_FN(disabled_keys, EV_KEY, true);
ATTR_SHOW_FN(disabled_switches, EV_SW, true);

/*
 * ATTRIBUTES:
 *
 * /sys/devices/platform/gpio-keys/keys [ro]
 * /sys/devices/platform/gpio-keys/switches [ro]
 */
static DEVICE_ATTR(keys, S_IRUGO, gpio_keys_show_keys, NULL);
static DEVICE_ATTR(switches, S_IRUGO, gpio_keys_show_switches, NULL);

#define ATTR_STORE_FN(name, type)					\
static ssize_t gpio_keys_store_##name(struct device *dev,		\
				      struct device_attribute *attr,	\
				      const char *buf,			\
				      size_t count)			\
{									\
	struct platform_device *pdev = to_platform_device(dev);		\
	struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);	\
	ssize_t error;							\
									\
	error = gpio_keys_attr_store_helper(ddata, buf, type);		\
	if (error)							\
		return error;						\
									\
	return count;							\
}

ATTR_STORE_FN(disabled_keys, EV_KEY);
ATTR_STORE_FN(disabled_switches, EV_SW);

/*
 * ATTRIBUTES:
 *
 * /sys/devices/platform/gpio-keys/disabled_keys [rw]
 * /sys/devices/platform/gpio-keys/disables_switches [rw]
 */
static DEVICE_ATTR(disabled_keys, S_IWUSR | S_IRUGO,
		   gpio_keys_show_disabled_keys,
		   gpio_keys_store_disabled_keys);
static DEVICE_ATTR(disabled_switches, S_IWUSR | S_IRUGO,
		   gpio_keys_show_disabled_switches,
		   gpio_keys_store_disabled_switches);

static struct attribute *gpio_keys_attrs[] = {
	&dev_attr_keys.attr,
	&dev_attr_switches.attr,
	&dev_attr_disabled_keys.attr,
	&dev_attr_disabled_switches.attr,
	NULL,
};

static struct attribute_group gpio_keys_attr_group = {
	.attrs = gpio_keys_attrs,
};

static int gpio_keys_gpio_report_event(struct gpio_button_data *bdata)
{
	const struct gpio_keys_button *button = bdata->button;
	struct input_dev *input = bdata->input;
	unsigned int type = button->type ?: EV_KEY;
	int state;
	//state = gpiod_get_value_cansleep(bdata->gpiod);
	state = gpiod_get_value(bdata->gpiod);

	if (state < 0) {
		dev_err(input->dev.parent,
			"failed to get gpio state: %d\n", state);
		return -1;
	}
#if 0
	if( (0!=bdata->report_jiffies) && 
		time_is_after_jiffies(bdata->report_jiffies+
			msecs_to_jiffies(bdata->software_debounce)) )
	{
		dev_info(input->dev.parent, "resetting irq=%d ... j=%lu\n",bdata->irq,bdata->report_jiffies);
		disable_irq(bdata->irq);
		udelay(1000);
		enable_irq(bdata->irq);
	}
#endif
	
	bdata->report_jiffies = jiffies;


	dev_info(input->dev.parent, "In gpio_keys_gpio_report_event,j=%lu\n",bdata->report_jiffies);
	dev_info(input->dev.parent, "active_low: %d\n", button->active_low);
	dev_info(input->dev.parent, "linux code = %d,irq=%d\n", button->code,(int)bdata->irq);
	dev_info(input->dev.parent, "LINE = %d, state: %d,gpioval=%d\n",
		   	__LINE__, state,gpiod_get_value(bdata->gpiod));

	if(state==bdata->key_pressed) {
		dev_info(input->dev.parent, "skipped report the same irq=%d,state=%d\n",bdata->irq,state);
	}
	else { 
		if (type == EV_ABS) {
			if (state)
				input_event(input, type, button->code, button->value);
		} else {
			input_event(input, type, button->code, state);
		}
		input_sync(input);
	}
	bdata->key_pressed = state;
	return state;
}

static void gpio_keys_gpio_work_func(struct work_struct *work)
{
	struct gpio_button_data *bdata =
		container_of(work, struct gpio_button_data, work.work);
	int iReportedState;
	
	disable_irq(bdata->irq);

	pr_debug("%s() irq=%d\n",__func__,bdata->irq);

	iReportedState = gpio_keys_gpio_report_event(bdata);

	if(1==iReportedState) {
		if(!delayed_work_pending(&bdata->work)) {
			pr_debug("%s() key pressing irq=%d delayed work queue again \n",__func__,bdata->irq);
			queue_delayed_work(system_wq,
				&bdata->work,
				msecs_to_jiffies(100));
		}
	}

#if 0
	if(iReportedState!=bdata->interrupt_state) {
		pr_debug("%s() resetting irq=%d, int state!=work state\n",__func__,bdata->irq);
		udelay(500);
		disable_irq(bdata->irq);
		udelay(500);
		enable_irq(bdata->irq);
	}
#endif

	enable_irq(bdata->irq);

	if (bdata->button->wakeup)
		pm_relax(bdata->input->dev.parent);
}

static irqreturn_t gpio_keys_gpio_isr(int irq, void *dev_id)
{
	struct gpio_button_data *bdata = dev_id;
	BUG_ON(irq != bdata->irq);




	bdata->interrupt_state = gpiod_get_value(bdata->gpiod);
	pr_debug("%s() irq=%d , val=%d\n",__func__,irq,bdata->interrupt_state);


	if (bdata->button->wakeup)
		pm_stay_awake(bdata->input->dev.parent);
#if 1
	mod_delayed_work(system_wq,
			 &bdata->work,
			 msecs_to_jiffies(bdata->software_debounce));
#else 
	queue_delayed_work(system_wq,
			 &bdata->work,
			 msecs_to_jiffies(bdata->software_debounce));
#endif

	return IRQ_HANDLED;
}

static void gpio_keys_irq_timer(unsigned long _data)
{
	struct gpio_button_data *bdata = (struct gpio_button_data *)_data;
	struct input_dev *input = bdata->input;
	unsigned long flags;

	spin_lock_irqsave(&bdata->lock, flags);
	if (1==bdata->key_pressed) {
		dev_dbg(&input->dev, "%s() release key code=%d\n",__func__,bdata->button->code);
		input_event(input, EV_KEY, bdata->button->code, 0);
		input_sync(input);
		bdata->key_pressed = false;
	}
	spin_unlock_irqrestore(&bdata->lock, flags);
}

static irqreturn_t gpio_keys_irq_isr(int irq, void *dev_id)
{
	struct gpio_button_data *bdata = dev_id;
	const struct gpio_keys_button *button = bdata->button;
	struct input_dev *input = bdata->input;
	unsigned long flags;

	BUG_ON(irq != bdata->irq);

	spin_lock_irqsave(&bdata->lock, flags);

	if (0==bdata->key_pressed) {
		if (bdata->button->wakeup)
			pm_wakeup_event(bdata->input->dev.parent, 0);

		dev_dbg(&input->dev, "%s() key code=%d down\n",__func__,bdata->button->code);
		input_event(input, EV_KEY, button->code, 1);
		input_sync(input);

		if (!bdata->release_delay) {
			dev_dbg(&input->dev, "%s() key code=%d up\n",__func__,bdata->button->code);
			input_event(input, EV_KEY, button->code, 0);
			input_sync(input);
			goto out;
		}

		bdata->key_pressed = true;
	}

	if (bdata->release_delay)
		mod_timer(&bdata->release_timer,
			jiffies + msecs_to_jiffies(bdata->release_delay));
out:
	spin_unlock_irqrestore(&bdata->lock, flags);
	return IRQ_HANDLED;
}

static void gpio_keys_quiesce_key(void *data)
{
	struct gpio_button_data *bdata = data;

	pr_info("%s() irq=%d\n",__func__,bdata->irq);
	if (bdata->gpiod)
		cancel_delayed_work_sync(&bdata->work);
	else
		del_timer_sync(&bdata->release_timer);
}

static int gpio_keys_setup_key(struct platform_device *pdev,
				struct input_dev *input,
				struct gpio_button_data *bdata,
				const struct gpio_keys_button *button)
{
	const char *desc = button->desc ? button->desc : "gpio_keys";
	struct device *dev = &pdev->dev;
	irq_handler_t isr;
	unsigned long irqflags;
	int irq;
	int error;

	bdata->deep_sleep_wakeup = 0;
	bdata->key_pressed = -1;
	bdata->input = input;
	bdata->button = button;
	spin_lock_init(&bdata->lock);

	bdata->report_jiffies = 0;

	/*
	 * Legacy GPIO number, so request the GPIO here and
	 * convert it to descriptor.
	 */
	if (gpio_is_valid(button->gpio)) {
		unsigned flags = GPIOF_IN;

		if (button->active_low)
			flags |= GPIOF_ACTIVE_LOW;

		error = devm_gpio_request_one(&pdev->dev, button->gpio, flags,
					      desc);
		if (error < 0) {
			dev_err(dev, "Failed to request GPIO %d, error %d\n",
				button->gpio, error);
			return error;
		}

		bdata->gpiod = gpio_to_desc(button->gpio);
		if (!bdata->gpiod)
			return -EINVAL;

		if (button->debounce_interval) {
			error = gpiod_set_debounce(bdata->gpiod,
					button->debounce_interval * 1000);
			/* use timer if gpiolib doesn't provide debounce */
			if (error < 0)
				bdata->software_debounce =
						button->debounce_interval;
		}

		if (button->irq) {
			bdata->irq = button->irq;
		} else {
			irq = gpiod_to_irq(bdata->gpiod);
			if (irq < 0) {
				error = irq;
				dev_err(dev,
					"Unable to get irq number for GPIO %d, error %d\n",
					button->gpio, error);
				return error;
			}
			bdata->irq = irq;
		}

		INIT_DELAYED_WORK(&bdata->work, gpio_keys_gpio_work_func);

		isr = gpio_keys_gpio_isr;
#if 1 //[
		if (button->active_low) {
			irqflags = IRQF_TRIGGER_FALLING;
		}
		else {
			irqflags = IRQF_TRIGGER_RISING;
		}
		//irqflags = IRQF_TRIGGER_HIGH|IRQF_TRIGGER_LOW;
#else 
		irqflags = IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;
#endif 

	} else {
		if (!button->irq) {
			dev_err(dev, "No IRQ specified\n");
			return -EINVAL;
		}
		bdata->irq = button->irq;

		if (button->type && button->type != EV_KEY) {
			dev_err(dev, "Only EV_KEY allowed for IRQ buttons.\n");
			return -EINVAL;
		}

		bdata->release_delay = button->debounce_interval;
		setup_timer(&bdata->release_timer,
			    gpio_keys_irq_timer, (unsigned long)bdata);

		isr = gpio_keys_irq_isr;
		irqflags = 0;
	}

	input_set_capability(input, button->type ?: EV_KEY, button->code);

	/*
	 * Install custom action to cancel release timer and
	 * workqueue item.
	 */
	error = devm_add_action(&pdev->dev, gpio_keys_quiesce_key, bdata);
	if (error) {
		dev_err(&pdev->dev,
			"failed to register quiesce action, error: %d\n",
			error);
		return error;
	}

	/*
	 * If platform has specified that the button can be disabled,
	 * we don't want it to share the interrupt line.
	 */
	if (!button->can_disable)
		irqflags |= IRQF_SHARED;
	bdata->isr = isr;
	bdata->irqflags = irqflags;
	error = devm_request_any_context_irq(&pdev->dev, bdata->irq,
					     isr, irqflags, desc, bdata);
	if (error < 0) {
		dev_err(dev, "Unable to claim irq %d; error %d\n",
			bdata->irq, error);
		return error;
	}

	return 0;
}

static void gpio_keys_report_state(struct gpio_keys_drvdata *ddata)
{
	struct input_dev *input = ddata->input;
	int i;

	for (i = 0; i < ddata->pdata->nbuttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];
		if (bdata->gpiod)
			gpio_keys_gpio_report_event(bdata);
	}
	input_sync(input);
}

static int gpio_keys_open(struct input_dev *input)
{
	struct gpio_keys_drvdata *ddata = input_get_drvdata(input);
	const struct gpio_keys_platform_data *pdata = ddata->pdata;
	int error;

	if (pdata->enable) {
		error = pdata->enable(input->dev.parent);
		if (error)
			return error;
	}

	/* Report current state of buttons that are connected to GPIOs */
	gpio_keys_report_state(ddata);

	return 0;
}

static void gpio_keys_close(struct input_dev *input)
{
	struct gpio_keys_drvdata *ddata = input_get_drvdata(input);
	const struct gpio_keys_platform_data *pdata = ddata->pdata;

	if (pdata->disable)
		pdata->disable(input->dev.parent);
}

/*
 * Handlers for alternative sources of platform_data
 */

#ifdef CONFIG_OF
/*
 * Translate OpenFirmware node properties into platform_data
 */
static struct gpio_keys_platform_data *
gpio_keys_get_devtree_pdata(struct device *dev)
{
	struct device_node *node, *pp;
	struct gpio_keys_platform_data *pdata;
	struct gpio_keys_button *button;
	int error;
	int nbuttons;
	int i;

	node = dev->of_node;
	if (!node)
		return ERR_PTR(-ENODEV);

	nbuttons = of_get_available_child_count(node);
	if (nbuttons == 0)
		return ERR_PTR(-ENODEV);

	pdata = devm_kzalloc(dev,
			     sizeof(*pdata) + nbuttons * sizeof(*button),
			     GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->buttons = (struct gpio_keys_button *)(pdata + 1);
	pdata->nbuttons = nbuttons;

	pdata->rep = !!of_get_property(node, "autorepeat", NULL);

	of_property_read_string(node, "label", &pdata->name);

	i = 0;
	for_each_available_child_of_node(node, pp) {
		enum of_gpio_flags flags;

		button = &pdata->buttons[i++];

		button->gpio = of_get_gpio_flags(pp, 0, &flags);
		if (button->gpio < 0) {
			error = button->gpio;
			if (error != -ENOENT) {
				if (error != -EPROBE_DEFER)
					dev_err(dev,
						"Failed to get gpio flags, error: %d\n",
						error);
				return ERR_PTR(error);
			}
		} else {
			button->active_low = flags & OF_GPIO_ACTIVE_LOW;
		}

		button->irq = irq_of_parse_and_map(pp, 0);

		if (!gpio_is_valid(button->gpio) && !button->irq) {
			dev_err(dev, "Found button without gpios or irqs\n");
			return ERR_PTR(-EINVAL);
		}

		if (of_property_read_u32(pp, "linux,code", &button->code)) {
			dev_err(dev, "Button without keycode: 0x%x\n",
				button->gpio);
			return ERR_PTR(-EINVAL);
		}


		button->desc = of_get_property(pp, "label", NULL);

		if (of_property_read_u32(pp, "linux,input-type", &button->type))
			button->type = EV_KEY;


		button->wakeup = of_property_read_bool(pp, "wakeup-source") ||
				 /* legacy name */
				 of_property_read_bool(pp, "gpio-key,wakeup");

		button->can_disable = !!of_get_property(pp, "linux,can-disable", NULL);

		if (of_property_read_u32(pp, "debounce-interval",
					 &button->debounce_interval))
			button->debounce_interval = 5;
	}

	if (pdata->nbuttons == 0)
		return ERR_PTR(-EINVAL);

	return pdata;
}

static const struct of_device_id gpio_keys_of_match[] = {
	{ .compatible = "gpio-keys", },
	{ },
};
MODULE_DEVICE_TABLE(of, gpio_keys_of_match);

#else

static inline struct gpio_keys_platform_data *
gpio_keys_get_devtree_pdata(struct device *dev)
{
	return ERR_PTR(-ENODEV);
}

#endif

static int32_t gpio_key_pinctrl_init(struct device *dev)
{
	int32_t ret;

	if (dev == NULL) {
		dev_err(dev, " device is NULL!\n");
		ret = -1;
	} else {
		pin_state_current=pin_state_active;

		pinctrl = devm_pinctrl_get(dev);
		if (IS_ERR(pinctrl))
			dev_err(dev, "unable to select pin group\n");

		pin_state_active = pinctrl_lookup_state(pinctrl, "active");
		if (IS_ERR(pin_state_active))
			dev_err(dev,"pinctrl_lookup_state active error!\n");

		pin_state_sleep = pinctrl_lookup_state(pinctrl, "sleep");
		if (IS_ERR(pin_state_sleep))
			dev_err(dev,"pinctrl_lookup_state sleep error!\n");

		pin_state_deepsleep =pinctrl_lookup_state(pinctrl, "deepsleep");
		if (IS_ERR(pin_state_deepsleep))
			dev_err(dev,"pinctrl_lookup_state deepsleep error!\n");
	}
	return ret;
}

static void gpio_key_pinctrl_select(struct device *dev,struct pinctrl_state *pin_state_select){

	if (dev == NULL) {
		dev_err(dev, " device is NULL!\n");

	} else {
		if(IS_ERR(pin_state_select)) {
			dev_err(dev,"%s pin state not exist\n",__func__);
			return;
		}
		if(pin_state_current==pin_state_select) {
			//printk("%s set same state, ignoer!\n",__func__);
			return;
		} else {
			if(pinctrl_select_state(pinctrl, pin_state_select)<0) {
				dev_err(dev, "pinctrl_select_state failed \n");
			}
			pin_state_current = pin_state_select;
		}
	}
}

static ssize_t deepslp_wake_enable_read(struct device *dev, struct device_attribute *attr,char *buf);
static ssize_t deepslp_wake_enable_write(struct device *dev, struct device_attribute *attr,const char *buf, size_t count);
static DEVICE_ATTR(deepslp_wake_enable, S_IWUSR | S_IRUSR|S_IROTH|S_IRGRP,
	   	deepslp_wake_enable_read, deepslp_wake_enable_write);

static ssize_t deepslp_wake_enable_read(struct device *dev, struct device_attribute *attr,char *buf)
{
	const struct gpio_keys_platform_data *pdata ;
	struct gpio_keys_drvdata *ddata ;
	int i;
	int len;
	int total_len=0;
	char *buf_ptr = buf;
	char *pcTemp,cSep='.';


	ddata = gt_gpio_keys_ddata;
	pdata = ddata->pdata;

	//printk("%s() : pdata=%p,buttons=%d\n",__func__,pdata,pdata->nbuttons);
	
	if(!pdata) {
		return 0;
	}


	for (i = 0; i < pdata->nbuttons; i++) {
		const struct gpio_keys_button *button = &pdata->buttons[i];
		struct gpio_button_data *bdata = &ddata->data[i];
		
		pcTemp = strchr(attr->attr.name,cSep);
		if(pcTemp) {

			*pcTemp = '\0';

			//printk("%s() : attr.name=\"%s\",button desc=\"%s\"\n",__func__,attr->attr.name,button->desc);

			if( 0==strcmp(attr->attr.name,button->desc) ) {
				len = sprintf(buf_ptr,"%d\n",bdata->deep_sleep_wakeup);total_len+=len;
			}
			*pcTemp = cSep;

			if(total_len>0) {
				// button found . 
				break;
			}
		}

	}

	return total_len;
}
static ssize_t deepslp_wake_enable_write(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	const struct gpio_keys_platform_data *pdata ;
	struct gpio_keys_drvdata *ddata ;
	int i;
	char *pcTemp,cSep='.';
	int iGot = 0;
	int len=strlen(buf);


	ddata = gt_gpio_keys_ddata;
	pdata = ddata->pdata;

	//printk("%s() : pdata=%p,buttons=%d\n",__func__,pdata,pdata->nbuttons);
	
	if(!pdata) {
		return 0;
	}


	for (i = 0; i < pdata->nbuttons; i++) {
		const struct gpio_keys_button *button = &pdata->buttons[i];
		struct gpio_button_data *bdata = &ddata->data[i];
		
		pcTemp = strchr(attr->attr.name,cSep);
		if(pcTemp) {

			*pcTemp = '\0';

			//printk("%s() : attr.name=\"%s\",button desc=\"%s\"\n",__func__,attr->attr.name,button->desc);

			if( 0==strcmp(attr->attr.name,button->desc) ) {
				bdata->deep_sleep_wakeup = (simple_strtol(buf,0,10))>0?1:0;
				iGot = 1;
			}
			*pcTemp = cSep;

			if(iGot) {
				// button found . 
				break;
			}
		}

	}

	return len;
}


static int gpio_keys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct gpio_keys_platform_data *pdata = dev_get_platdata(dev);
	struct gpio_keys_drvdata *ddata;
	struct input_dev *input;
	size_t size;
	int i, error, err;
	int wakeup = 0;
	int iIsNTX_EVT0 = 0;

#ifdef CONFIG_KEYBOARD_NTXEVT0 //[	
	extern struct input_dev * ntx_get_event0_inputdev(void);
#endif //]CONFIG_KEYBOARD_NTXEVT0

	dev_info(dev, "Start gpio_keys_probe\n");

	if (!pdata) {
		pdata = gpio_keys_get_devtree_pdata(dev);
		if (IS_ERR(pdata))
			return PTR_ERR(pdata);
	}

	size = sizeof(struct gpio_keys_drvdata) +
			pdata->nbuttons * sizeof(struct gpio_button_data);
	ddata = devm_kzalloc(dev, size, GFP_KERNEL);
	if (!ddata) {
		dev_err(dev, "failed to allocate state\n");
		return -ENOMEM;
	}
	err = gpio_key_pinctrl_init(&pdev->dev);
	if (err != 0)
		dev_err(dev,"gpio pinctrl init failed\n");
	gpio_key_pinctrl_select(dev,pin_state_active);

#ifdef CONFIG_KEYBOARD_NTXEVT0 //[
	printk(KERN_INFO"[%s_%d] assign NTXEVENT0 \n",__FUNCTION__,__LINE__);	
	input  = ntx_get_event0_inputdev();
	if(!input) 
#endif //]CONFIG_KEYBOARD_NTXEVT0
	{
		printk(KERN_ERR"%s() : no ntx_event0 \n",__func__);
		input = devm_input_allocate_device(dev);
		if (!input) {
			dev_err(dev, "failed to allocate input device\n");
			return -ENOMEM;
		}
	}
#ifdef CONFIG_KEYBOARD_NTXEVT0 //[	
	else {
		iIsNTX_EVT0 = 1;
	}
#endif //]CONFIG_KEYBOARD_NTXEVT0


	ddata->pdata = pdata;
	ddata->input = input;
	mutex_init(&ddata->disable_lock);

	platform_set_drvdata(pdev, ddata);
	input_set_drvdata(input, ddata);

	input->name = pdata->name ? : pdev->name;
	input->phys = "gpio-keys/input0";
	input->dev.parent = &pdev->dev;
	input->open = gpio_keys_open;
	input->close = gpio_keys_close;

	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0100;

	/* Enable auto repeat feature of Linux input subsystem */
	if (pdata->rep)
		__set_bit(EV_REP, input->evbit);

	error = sysfs_create_group(&pdev->dev.kobj, &gpio_keys_attr_group);
	if (error) {
		dev_err(dev, "Unable to export keys/switches, error: %d\n",
			error);
		return error;
	}

	for (i = 0; i < pdata->nbuttons; i++) {
		const struct gpio_keys_button *button = &pdata->buttons[i];
		struct gpio_button_data *bdata = &ddata->data[i];

		error = gpio_keys_setup_key(pdev, input, bdata, button);
		if (error)
			return error;

		
		sprintf(bdata->cDeepSlp_wake_enable_name_bufA,"%s.%s",button->desc,"deepslp_wake_en");

		{

			bdata->attr_deepslp_wake_en.attr.name = bdata->cDeepSlp_wake_enable_name_bufA;
			bdata->attr_deepslp_wake_en.attr.mode=dev_attr_deepslp_wake_enable.attr.mode;
			bdata->attr_deepslp_wake_en.show=dev_attr_deepslp_wake_enable.show;
			bdata->attr_deepslp_wake_en.store=dev_attr_deepslp_wake_enable.store;
			if(device_create_file(&pdev->dev, &bdata->attr_deepslp_wake_en)) {
				dev_warn(dev, "Can't create %s's deepslp_wake_enable attr sysfs !\n",button->desc);
			}
		}
		
		if(0==strcmp(button->desc,"Hall")) {
			bdata->deep_sleep_wakeup = 1;
		}

		if (button->wakeup)
			wakeup = 1;
	}

	input_set_capability(input, EV_MSC, MSC_RAW);	// for si114x report event


	if(!iIsNTX_EVT0) 
	{
		error = input_register_device(input);
		if (error) {
			dev_err(dev, "Unable to register input device, error: %d\n",
				error);
			goto err_remove_group;
		}
	}

	device_init_wakeup(&pdev->dev, wakeup);
	printk(KERN_ERR"[%s_%d] Set Global data \n",__FUNCTION__,__LINE__);
	gt_gpio_keys_ddata = ddata;

	return 0;

err_remove_group:
	sysfs_remove_group(&pdev->dev.kobj, &gpio_keys_attr_group);
	return error;
}


int gpio_keys_report_event(unsigned int type,unsigned int code , int value )
{
	int iRet=-1;

	if( gt_gpio_keys_ddata && gt_gpio_keys_ddata->input )	{
		printk("[%s_%d] code=%d val=%d\n",__FUNCTION__,__LINE__,code,value);

		input_event(gt_gpio_keys_ddata->input,type,code,value);
		input_sync(gt_gpio_keys_ddata->input);
		iRet = 0;
	}
	else{
		printk(KERN_ERR"[%s_%d] Can't report ntx_event !\n",__FUNCTION__,__LINE__);
	}
	return iRet;
}

static int gpio_keys_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct gpio_keys_platform_data *pdata = dev_get_platdata(dev);
	struct gpio_keys_drvdata *ddata = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < pdata->nbuttons; i++) {
		//const struct gpio_keys_button *button = &pdata->buttons[i];
		struct gpio_button_data *bdata = &ddata->data[i];

	   device_remove_file(&pdev->dev,&bdata->attr_deepslp_wake_en);

	}
	
	sysfs_remove_group(&pdev->dev.kobj, &gpio_keys_attr_group);

	device_init_wakeup(&pdev->dev, 0);

	return 0;
}


#ifdef CONFIG_PM_SLEEP
static int gpio_keys_suspend(struct device *dev)
{
	struct gpio_keys_drvdata *ddata = dev_get_drvdata(dev);
	struct input_dev *input = ddata->input;
	int i;

	printk(KERN_INFO"[%s_%d] gSleep_Mode_Suspend = %d\n",__FUNCTION__,__LINE__,gSleep_Mode_Suspend);
	if (gSleep_Mode_Suspend)
		gpio_key_pinctrl_select(dev,pin_state_deepsleep);

	if (device_may_wakeup(dev)) {
		for (i = 0; i < ddata->pdata->nbuttons; i++) {
			struct gpio_button_data *bdata = &ddata->data[i];
			printk(KERN_INFO"[%s_%d] irq:%d \n",__FUNCTION__,__LINE__,bdata->irq);
			if (bdata->button->wakeup) {
				if(gSleep_Mode_Suspend && !bdata->deep_sleep_wakeup) {
					//do nothing
				} else {
					enable_irq_wake(bdata->irq);
				}
			}

#ifdef SUSPEND_DEDGE_TEST //[
			{
			int error ;
			devm_free_irq(dev,bdata->irq,bdata);
			error = devm_request_any_context_irq(dev, bdata->irq,
						 bdata->isr, 
						 bdata->irqflags|IRQF_TRIGGER_FALLING|IRQF_TRIGGER_RISING,
						 bdata->button->desc, bdata);
			if (error < 0) {
				dev_err(dev, "Unable to claim irq %d; error %d\n",
					bdata->irq, error);
				return error;
			}
			}
#endif //]SUSPEND_DEDGE_TEST
	   
		}
	} else {
		mutex_lock(&input->mutex);
		if (input->users)
			gpio_keys_close(input);
		mutex_unlock(&input->mutex);
	}




	return 0;
}

static int gpio_keys_resume(struct device *dev)
{
	struct gpio_keys_drvdata *ddata = dev_get_drvdata(dev);
	struct input_dev *input = ddata->input;
	int error = 0;
	int i;




	gpio_key_pinctrl_select(dev,pin_state_active);

	if (device_may_wakeup(dev)) {
		for (i = 0; i < ddata->pdata->nbuttons; i++) {
			struct gpio_button_data *bdata = &ddata->data[i];

#ifdef SUSPEND_DEDGE_TEST //[
			devm_free_irq(dev,bdata->irq,bdata);
			dev_dbg(dev, "resume request irq=%d, irqflags=0x%lx\n",bdata->irq,bdata->irqflags );
			error = devm_request_any_context_irq(dev, bdata->irq,
								 bdata->isr, 
								 bdata->irqflags,
								 bdata->button->desc, bdata);
			if (error < 0) {
				dev_err(dev, "Unable to claim irq %d; error %d\n",
					bdata->irq, error);
			}
#endif //] SUSPEND_DEDGE_TEST
			
			if (bdata->button->wakeup) {
				if(gSleep_Mode_Suspend && !bdata->deep_sleep_wakeup) {
					//do nothing
				} else {
					disable_irq_wake(bdata->irq);
				}
			}
		}
	} else {
		mutex_lock(&input->mutex);
		if (input->users)
			error = gpio_keys_open(input);
		mutex_unlock(&input->mutex);
	}

	if (error)
		return error;

	gpio_keys_report_state(ddata);
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(gpio_keys_pm_ops, gpio_keys_suspend, gpio_keys_resume);

static struct platform_driver gpio_keys_device_driver = {
	.probe		= gpio_keys_probe,
	.remove		= gpio_keys_remove,
	.driver		= {
		.name	= "gpio-keys",
		.pm	= &gpio_keys_pm_ops,
		.of_match_table = of_match_ptr(gpio_keys_of_match),
	}
};

static int __init gpio_keys_init(void)
{
	return platform_driver_register(&gpio_keys_device_driver);
}

static void __exit gpio_keys_exit(void)
{
	platform_driver_unregister(&gpio_keys_device_driver);
}

late_initcall(gpio_keys_init);
module_exit(gpio_keys_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Phil Blundell <pb@handhelds.org>");
MODULE_DESCRIPTION("Keyboard driver for GPIOs");
MODULE_ALIAS("platform:gpio-keys");

/*
 * linux/drivers/leds-pwm.c
 *
 * simple PWM based LED control
 *
 * Copyright 2009 Luotao Fu @ Pengutronix (l.fu@pengutronix.de)
 *
 * based on leds-gpio.c by Raphael Assenat <raph@8d.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/leds.h>
#include <linux/err.h>
#include <linux/pwm.h>
#include <linux/leds_pwm.h>
#include <linux/slab.h>

struct led_element_pwm {
	int			element_index;

	struct pwm_device	*pwm;
	unsigned int		active_low;
	unsigned int		period;
	int			duty;
};

struct led_pwm_data {
	struct led_classdev	cdev;

	unsigned int		num_elements;
	struct led_element_pwm	*elements;
};

struct led_pwm_priv {
	int num_leds;
	struct led_pwm_data leds[0];
};

static void __led_element_pwm_set(struct led_element_pwm *elem_pwm)
{
	int new_duty = elem_pwm->duty;

	pwm_config(elem_pwm->pwm, new_duty, elem_pwm->period);

	if (new_duty == 0)
		pwm_disable(elem_pwm->pwm);
	else
		pwm_enable(elem_pwm->pwm);
}

static int led_pwm_set(struct led_classdev *led_cdev,
		       enum led_brightness brightness)
{
	struct led_pwm_data *led_dat =
		container_of(led_cdev, struct led_pwm_data, cdev);
	unsigned int raw_value;
	unsigned int max_value;
	unsigned long long duty;
	unsigned int i;

	led_scale_color_elements(led_cdev, brightness);

	for (i = 0; i < led_dat->num_elements; i++) {
		struct led_element_pwm *elem_pwm = &led_dat->elements[i];
		struct led_color_element *color_element;
		int element_index = elem_pwm->element_index;

		if (element_index < 0)
			continue;

		color_element = &led_cdev->color_elements[element_index];

		raw_value = color_element->raw_value;
		max_value = color_element->max_value;

		duty = elem_pwm->period;

		duty *= raw_value;
		do_div(duty, max_value);

		if (elem_pwm->active_low)
			duty = elem_pwm->period - duty;

		elem_pwm->duty = duty;

		__led_element_pwm_set(elem_pwm);
	}

	return 0;
}

static inline size_t sizeof_pwm_leds_priv(int num_leds)
{
	return sizeof(struct led_pwm_priv) +
		      (sizeof(struct led_pwm_data) * num_leds);
}

static void led_pwm_data_cleanup(struct led_pwm_data *pwm_data)
{
	led_classdev_unregister(&pwm_data->cdev);
}

static void led_pwm_cleanup(struct led_pwm_priv *priv)
{
	while (priv->num_leds--)
		led_pwm_data_cleanup(&priv->leds[priv->num_leds]);
}

static int led_pwm_add_single(struct device *dev, struct led_pwm_priv *priv,
			      struct led_pwm *led, struct device_node *child)
{
	struct led_pwm_data *led_data = &priv->leds[priv->num_leds];
	struct led_color_element *color_element;
	struct pwm_args pargs;
	int ret;

	led_data->cdev.num_color_elements = 1;
	led_data->cdev.color_elements = devm_kzalloc(dev,
		sizeof(struct led_color_element) *
			led_data->cdev.num_color_elements,
		GFP_KERNEL);

	led_data->cdev.color_elements[0].name = devm_kstrdup_const(dev,
								   "single",
								   GFP_KERNEL);

	led_data->num_elements = led_data->cdev.num_color_elements;

	led_data->elements = devm_kzalloc(dev,
		sizeof(struct led_element_pwm) * led_data->num_elements,
		GFP_KERNEL);

	led_data->elements[0].element_index = 0;

	led_data->elements[0].active_low = led->active_low;

	led_data->cdev.name = led->name;
	led_data->cdev.default_trigger = led->default_trigger;
	led_data->cdev.brightness = LED_OFF;
	led_data->cdev.max_brightness = led->max_brightness;
	led_data->cdev.flags = LED_CORE_SUSPENDRESUME;

	if (child)
		led_data->elements[0].pwm = devm_of_pwm_get(dev, child, NULL);
	else
		led_data->elements[0].pwm = devm_pwm_get(dev, led->name);
	if (IS_ERR(led_data->elements[0].pwm)) {
		ret = PTR_ERR(led_data->elements[0].pwm);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "unable to request PWM for %s: %d\n",
				led->name, ret);
		return ret;
	}

	led_data->cdev.brightness_set_blocking = led_pwm_set;

	/*
	 * FIXME: pwm_apply_args() should be removed when switching to the
	 * atomic PWM API.
	 */
	pwm_apply_args(led_data->elements[0].pwm);

	pwm_get_args(led_data->elements[0].pwm, &pargs);

	led_data->elements[0].period = pargs.period;
	if (!led_data->elements[0].period && (led->pwm_period_ns > 0))
		led_data->elements[0].period = led->pwm_period_ns;

	ret = led_classdev_register(dev, &led_data->cdev);
	if (ret == 0) {
		priv->num_leds++;

		color_element = &led_data->cdev.color_elements[0];

		color_element->value = led_data->cdev.max_brightness;
		color_element->max_value = led_data->cdev.max_brightness;

		led_pwm_set(&led_data->cdev, led_data->cdev.brightness);
	} else {
		dev_err(dev, "failed to register PWM led for %s: %d\n",
			led->name, ret);
	}

	return ret;
}

static int led_pwm_add_multi(struct device *dev, struct led_pwm_priv *priv,
			     struct led_pwm *led, struct device_node *child,
			     unsigned int num_color_elements)
{
	struct led_pwm_data *led_data = &priv->leds[priv->num_leds];
	struct pwm_args pargs;
	unsigned int elem_index;
	struct device_node *elem_child;
	int ret;

	led_data->cdev.name = led->name;
	led_data->cdev.default_trigger = led->default_trigger;
	led_data->cdev.brightness = LED_OFF;
	led_data->cdev.max_brightness = led->max_brightness;
	led_data->cdev.flags = LED_CORE_SUSPENDRESUME | LED_MULTI_COLOR_LED;

	led_data->cdev.num_color_elements = num_color_elements;
	led_data->cdev.color_elements = devm_kzalloc(dev,
		sizeof(struct led_color_element) * num_color_elements,
		GFP_KERNEL);

	led_data->num_elements = num_color_elements;
	led_data->elements = devm_kzalloc(dev,
		sizeof(struct led_element_pwm) * num_color_elements,
		GFP_KERNEL);

	elem_index = 0;
	for_each_child_of_node(child, elem_child) {
		if (strncmp(elem_child->name, "element-", 8) == 0) {
			struct led_element_pwm *led_element;

			ret = led_color_element_setup_of(dev, &led_data->cdev,
							 elem_index,
							 elem_child);
			if (ret)
				return ret;

			led_element = &led_data->elements[elem_index];

			led_element->element_index = elem_index;

			led_element->active_low = of_property_read_bool(
				elem_child, "active-low");

			led_element->pwm = devm_of_pwm_get(dev, elem_child,
							   NULL);

			if (IS_ERR(led_element->pwm)) {
				ret = PTR_ERR(led_element->pwm);
				if (ret != -EPROBE_DEFER)
					dev_err(dev, "unable to request PWM for %s: %d\n",
						led->name, ret);
				return ret;
			}

			/*
			 * FIXME: pwm_apply_args() should be removed when
			 * switching to the atomic PWM API.
			 */
			pwm_apply_args(led_element->pwm);

			pwm_get_args(led_element->pwm, &pargs);

			led_element->period = pargs.period;

			elem_index++;
		}
	}

	led_data->cdev.brightness_set_blocking = led_pwm_set;

	ret = led_classdev_register(dev, &led_data->cdev);
	if (ret == 0) {
		priv->num_leds++;
		led_pwm_set(&led_data->cdev, led_data->cdev.brightness);
	} else {
		dev_err(dev, "failed to register PWM led for %s: %d\n",
			led->name, ret);
	}

	return ret;
}

static int led_pwm_create_of(struct device *dev, struct led_pwm_priv *priv)
{
	struct device_node *child;
	struct device_node *elem_child;
	struct led_pwm led;
	int num_color_elements;
	int ret = 0;

	memset(&led, 0, sizeof(led));

	for_each_child_of_node(dev->of_node, child) {
		led.name = of_get_property(child, "label", NULL) ? :
			   child->name;

		led.default_trigger = of_get_property(child,
						"linux,default-trigger", NULL);
		of_property_read_u32(child, "max-brightness",
				     &led.max_brightness);

		num_color_elements = 0;

		for_each_child_of_node(child, elem_child) {
			if (strncmp(elem_child->name, "element-", 8) == 0)
				num_color_elements++;
		}

		if (num_color_elements) {
			ret = led_pwm_add_multi(dev, priv, &led, child,
						num_color_elements);
			if (ret) {
				of_node_put(child);
				break;
			}
		} else {
			led.active_low = of_property_read_bool(child,
							       "active-low");

			ret = led_pwm_add_single(dev, priv, &led, child);
			if (ret) {
				of_node_put(child);
				break;
			}
		}
	}

	return ret;
}

static int led_pwm_probe(struct platform_device *pdev)
{
	struct led_pwm_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct led_pwm_priv *priv;
	int count, i;
	int ret = 0;

	if (pdata)
		count = pdata->num_leds;
	else
		count = of_get_child_count(pdev->dev.of_node);

	if (!count)
		return -EINVAL;

	priv = devm_kzalloc(&pdev->dev, sizeof_pwm_leds_priv(count),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	if (pdata) {
		for (i = 0; i < count; i++) {
			ret = led_pwm_add_single(&pdev->dev, priv,
						 &pdata->leds[i], NULL);
			if (ret)
				break;
		}
	} else {
		ret = led_pwm_create_of(&pdev->dev, priv);
	}

	if (ret) {
		led_pwm_cleanup(priv);
		return ret;
	}

	platform_set_drvdata(pdev, priv);

	return 0;
}

static int led_pwm_remove(struct platform_device *pdev)
{
	struct led_pwm_priv *priv = platform_get_drvdata(pdev);

	led_pwm_cleanup(priv);

	return 0;
}

static const struct of_device_id of_pwm_leds_match[] = {
	{ .compatible = "pwm-leds", },
	{},
};
MODULE_DEVICE_TABLE(of, of_pwm_leds_match);

static struct platform_driver led_pwm_driver = {
	.probe		= led_pwm_probe,
	.remove		= led_pwm_remove,
	.driver		= {
		.name	= "leds_pwm",
		.of_match_table = of_pwm_leds_match,
	},
};

module_platform_driver(led_pwm_driver);

MODULE_AUTHOR("Luotao Fu <l.fu@pengutronix.de>");
MODULE_DESCRIPTION("generic PWM LED driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:leds-pwm");

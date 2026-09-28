// SPDX-License-Identifier: GPL-2.0-only
/* Chipsemi CHSC5448: protocol derived from the chsc5448_p785 vendor driver. */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>

#define CHSC_BOOT_HEADER	0x20000014
#define CHSC_BOOT_STATUS	0x20000018
#define CHSC_TOUCH_DATA		0x2000002c
#define CHSC_SIGNATURE		0x43534843
#define CHSC_BOOT_ERROR		0x4f525245
#define CHSC_MAX_TOUCHES		10
#define CHSC_REPORT_SIZE	56
#define CHSC_POINT_SIZE		5

struct chsc5448 {
	struct i2c_client *client;
	struct regmap *regmap;
	struct gpio_desc *reset;
	struct regulator_bulk_data supplies[2];
	struct input_dev *input;
	struct touchscreen_properties props;
};

static const struct regmap_config chsc5448_regmap_config = {
	.reg_bits = 32,
	.val_bits = 8,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.cache_type = REGCACHE_NONE,
};

static int chsc5448_boot(struct chsc5448 *ts)
{
	u8 header[12];
	u8 zero[4] = { 0 };
	u32 signature;
	int error, i;

	/* GPIO values are logical: DT must declare an active-low reset. */
	gpiod_set_value_cansleep(ts->reset, 1);
	msleep(30);
	gpiod_set_value_cansleep(ts->reset, 0);
	usleep_range(5000, 6000);

	/* Vendor reset sequence clears the boot/self-check word. Not flash. */
	error = regmap_bulk_write(ts->regmap, CHSC_BOOT_STATUS, zero, sizeof(zero));
	if (error)
		return error;

	for (i = 0; i < 20; i++) {
		usleep_range(10000, 12000);
		error = regmap_bulk_read(ts->regmap, CHSC_BOOT_HEADER,
					 header, sizeof(header));
		if (error)
			continue;

		signature = get_unaligned_le32(header + 4);
		if (signature == CHSC_SIGNATURE) {
			dev_dbg(&ts->client->dev, "firmware %04x, VID/PID %08x\n",
				get_unaligned_le16(header),
				get_unaligned_le32(header + 8));
			return 0;
		}
		if (signature == CHSC_BOOT_ERROR) {
			dev_err(&ts->client->dev,
				"firmware self-check failed; refusing to flash\n");
			return -EIO;
		}
	}

	return error ? error : -ETIMEDOUT;
}

static void chsc5448_release_all(struct chsc5448 *ts)
{
	int i;

	for (i = 0; i < CHSC_MAX_TOUCHES; i++) {
		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
	}
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

static irqreturn_t chsc5448_irq(int irq, void *data)
{
	struct chsc5448 *ts = data;
	u8 report[CHSC_REPORT_SIZE];
	unsigned int seen = 0;
	int error, i;

	error = regmap_bulk_read(ts->regmap, CHSC_TOUCH_DATA,
				 report, sizeof(report));
	if (error) {
		dev_err_ratelimited(&ts->client->dev, "report read failed: %d\n", error);
		return IRQ_HANDLED;
	}

	/* Proximity, heartbeat and gesture packets are not touch frames. */
	if (report[0] >= 0xfc)
		return IRQ_HANDLED;
	if ((report[1] & 0x0f) > CHSC_MAX_TOUCHES)
		goto malformed;
	if (!(report[1] & 0x0f)) {
		chsc5448_release_all(ts);
		return IRQ_HANDLED;
	}

	/* Validate the entire frame before changing any input state. */
	for (i = 0; i < CHSC_MAX_TOUCHES; i++) {
		const u8 *p = report + 2 + i * CHSC_POINT_SIZE;
		unsigned int id = p[4] & 0x0f;
		unsigned int event = p[4] >> 4;

		if (id == 0x0f)
			continue;
		if (id >= CHSC_MAX_TOUCHES || (seen & BIT(id)) ||
		    (event != 0 && event != 8 && event != 4))
			goto malformed;
		seen |= BIT(id);
	}

	for (i = 0; i < CHSC_MAX_TOUCHES; i++) {
		const u8 *p = report + 2 + i * CHSC_POINT_SIZE;
		unsigned int id = p[4] & 0x0f;
		bool active = (p[4] >> 4) != 4;

		if (id == 0x0f)
			continue;
		input_mt_slot(ts->input, id);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, active);
		if (active) {
			touchscreen_report_pos(ts->input, &ts->props,
					       p[0] | ((p[3] & 0x0f) << 8),
					       p[1] | ((p[3] >> 4) << 8), true);
			input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, p[2]);
		}
	}
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
	return IRQ_HANDLED;

malformed:
	dev_warn_ratelimited(&ts->client->dev, "malformed touch frame\n");
	return IRQ_HANDLED;
}

static void chsc5448_power_off(void *data)
{
	struct chsc5448 *ts = data;

	gpiod_set_value_cansleep(ts->reset, 1);
	regulator_bulk_disable(ARRAY_SIZE(ts->supplies), ts->supplies);
}

static int chsc5448_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct chsc5448 *ts;
	int error;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	if (client->irq <= 0)
		return dev_err_probe(dev, -EINVAL, "missing interrupt\n");

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->client = client;
	i2c_set_clientdata(client, ts);
	ts->regmap = devm_regmap_init_i2c(client, &chsc5448_regmap_config);
	if (IS_ERR(ts->regmap))
		return dev_err_probe(dev, PTR_ERR(ts->regmap), "regmap init failed\n");

	ts->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset))
		return dev_err_probe(dev, PTR_ERR(ts->reset), "reset GPIO unavailable\n");
	ts->supplies[0].supply = "vdd";
	ts->supplies[1].supply = "vccio";
	error = devm_regulator_bulk_get(dev, ARRAY_SIZE(ts->supplies), ts->supplies);
	if (error)
		return dev_err_probe(dev, error, "supplies unavailable\n");

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;
	ts->input->name = "Chipsemi CHSC5448 Touchscreen";
	ts->input->id.bustype = BUS_I2C;
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, 0, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, 0, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	touchscreen_parse_properties(ts->input, true, &ts->props);
	if (!input_abs_get_max(ts->input, ABS_MT_POSITION_X) ||
	    !input_abs_get_max(ts->input, ABS_MT_POSITION_Y))
		return dev_err_probe(dev, -EINVAL, "touchscreen-size-x/y required\n");

	/* Vendor reports all ten records, with 0xf marking unused slots. */
	error = input_mt_init_slots(ts->input, CHSC_MAX_TOUCHES,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error)
		return error;

	error = regulator_bulk_enable(ARRAY_SIZE(ts->supplies), ts->supplies);
	if (error)
		return dev_err_probe(dev, error, "power-on failed\n");
	error = devm_add_action_or_reset(dev, chsc5448_power_off, ts);
	if (error)
		return error;
	error = chsc5448_boot(ts);
	if (error)
		return dev_err_probe(dev, error, "firmware did not boot\n");

	error = input_register_device(ts->input);
	if (error)
		return error;
	return devm_request_threaded_irq(dev, client->irq, NULL, chsc5448_irq,
					IRQF_ONESHOT, dev_name(dev), ts);
}

static int chsc5448_suspend(struct device *dev)
{
	struct chsc5448 *ts = dev_get_drvdata(dev);

	disable_irq(ts->client->irq);
	chsc5448_release_all(ts);
	/* No wake gestures: hold reset rather than issuing undocumented commands. */
	gpiod_set_value_cansleep(ts->reset, 1);
	return 0;
}

static int chsc5448_resume(struct device *dev)
{
	struct chsc5448 *ts = dev_get_drvdata(dev);
	int error;

	error = chsc5448_boot(ts);
	if (error)
		dev_err(dev, "resume boot failed: %d\n", error);
	/* Balance suspend even on failure; preserve the error for the PM core. */
	enable_irq(ts->client->irq);
	return error;
}

static DEFINE_SIMPLE_DEV_PM_OPS(chsc5448_pm_ops, chsc5448_suspend, chsc5448_resume);

static const struct of_device_id chsc5448_of_match[] = {
	{ .compatible = "chipsemi,chsc5448" },
	{ }
};
MODULE_DEVICE_TABLE(of, chsc5448_of_match);

static const struct i2c_device_id chsc5448_ids[] = {
	{ "chsc5448" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, chsc5448_ids);

static struct i2c_driver chsc5448_driver = {
	.driver = {
		.name = "chsc5448",
		.of_match_table = chsc5448_of_match,
		.pm = pm_sleep_ptr(&chsc5448_pm_ops),
	},
	.probe = chsc5448_probe,
	.id_table = chsc5448_ids,
};
module_i2c_driver(chsc5448_driver);

MODULE_DESCRIPTION("Chipsemi CHSC5448 touchscreen driver");
MODULE_LICENSE("GPL");

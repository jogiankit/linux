/*
 * Thermal sensor driver for Allwinner H6
 *
 * Copyright (C) 2018 Icenowy Zheng
 *
 * Based on the work of Ondřej Jirman
 * Based on the work of Josef Gajdusek <atx@atx.name>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/printk.h>

#define THS_H6_MAX_SENSOR_NUM	4

#define THS_H6_CTRL0		0x00
#define THS_H6_CTRL2		0x04
#define THS_H6_PER		0x08
#define THS_H6_DATA_INT_CTRL	0x10
#define THS_H6_DATA_INT_STAT	0x20
#define THS_H6_FILTER		0x30
#define THS_H6_CDATA(n)		(0xa0 + 4 * (n))
#define THS_H6_DATA(n)		(0xc0 + 4 * (n))

#define THS_H6_CTRL0_SENSOR_ACQ0(x)	((x) << 16)
#define THS_H6_CTRL2_SENSE_EN(n)	BIT(0 + (n))
#define THS_H6_PER_THERMAL_PER(x)	((x) << 12)
#define THS_H6_INT_CTRL_DATA_IRQ_EN(n)	BIT(0 + (n))
#define THS_H6_STAT_DATA_IRQ_STS(n)	BIT(0 + (n))
#define THS_H6_FILTER_TYPE(x)		((x) << 0)
#define THS_H6_FILTER_EN		BIT(2)

#define THS_H6_CLK_IN		240000000 /* Hz */
#define THS_H6_DATA_PERIOD	10 /* ms */

#define THS_H6_FILTER_TYPE_VALUE	2 /* average over 2^(n+1) samples */
#define THS_H6_FILTER_DIV		(1 << (THS_H6_FILTER_TYPE_VALUE + 1))
#define THS_H6_INT_CTRL_THERMAL_PER_VALUE \
	(THS_H6_DATA_PERIOD * (THS_H6_CLK_IN / 1000) / THS_H6_FILTER_DIV / 4096 - 1)
#define THS_H6_CTRL0_SENSOR_ACQ0_VALUE	0x1df /* 20us */
#define THS_H6_CTRL0_UNK		0x0000002f

#define THS_H6_CAL_FT_TEMP_MASK		0x0fff
#define THS_H6_CAL_FT_TEMP_DEVIATION_EN	0x3000
#define THS_H6_CAL_DEFAULT		0x800
#define THS_H6_CAL_VAL_MASK		0xfff

struct sun50i_h6_ths_data;

struct sun50i_h6_ths_sensor {
	struct sun50i_h6_ths_data *data;
	int id;
	struct thermal_zone_device *tzd;
	u32 val;
};

struct sun50i_h6_ths_cfg {
	int sensor_num;
	int (*calc_temp)(u32 val);
};

struct sun50i_h6_ths_data {
	struct reset_control *reset;
	struct clk *busclk;
	void __iomem *regs;
	const struct sun50i_h6_ths_cfg *cfg;
	struct nvmem_cell *calcell;
	struct sun50i_h6_ths_sensor sensors[THS_H6_MAX_SENSOR_NUM];
};

static int sun50i_h6_ths_calc_temp(u32 val)
{
	return (187744 - (int)((val * 1000000) / 14882));
}

static u16 sun50i_h6_ths_recalc_reg(u32 temp)
{
	return (u16)(2794 - temp * 14882 / 1000000);
}

static int sun50i_h6_ths_get_temp(void *_data, int *out)
{
	struct sun50i_h6_ths_sensor *sensor = _data;

	if (sensor->val == 0)
		return -EBUSY;

	/* Formula and parameters from the Allwinner 3.4 kernel */
	*out = sensor->data->cfg->calc_temp(sensor->val);
	return 0;
}

static irqreturn_t sun50i_h6_ths_irq_thread(int irq, void *_data)
{
	struct sun50i_h6_ths_data *data = _data;
	int i;

	for (i = 0; i < data->cfg->sensor_num; i++) {
		if (!(readl(data->regs + THS_H6_DATA_INT_STAT) &
		      THS_H6_STAT_DATA_IRQ_STS(i)))
			continue;

		writel(THS_H6_STAT_DATA_IRQ_STS(i),
		       data->regs + THS_H6_DATA_INT_STAT);

		data->sensors[i].val = readl(data->regs + THS_H6_DATA(i));
		if (data->sensors[i].val)
			thermal_zone_device_update(data->sensors[i].tzd,
						   THERMAL_EVENT_TEMP_SAMPLE);
	}

	return IRQ_HANDLED;
}

static void sun50i_h6_ths_init(struct sun50i_h6_ths_data *data)
{
	u32 val;
	int i;

	writel(THS_H6_CTRL0_SENSOR_ACQ0(THS_H6_CTRL0_SENSOR_ACQ0_VALUE) |
	       THS_H6_CTRL0_UNK, data->regs + THS_H6_CTRL0);
	writel(THS_H6_FILTER_EN | THS_H6_FILTER_TYPE(THS_H6_FILTER_TYPE_VALUE),
	       data->regs + THS_H6_FILTER);

	val = 0;
	for (i = 0; i < data->cfg->sensor_num; i++)
		val |= THS_H6_CTRL2_SENSE_EN(i);
	writel(val, data->regs + THS_H6_CTRL2);

	val = THS_H6_PER_THERMAL_PER(THS_H6_INT_CTRL_THERMAL_PER_VALUE);
	writel(val, data->regs + THS_H6_PER);

	val = 0;
	for (i = 0; i < data->cfg->sensor_num; i++)
		val |= THS_H6_INT_CTRL_DATA_IRQ_EN(i);
	writel(val, data->regs + THS_H6_DATA_INT_CTRL);
}

static const struct thermal_zone_of_device_ops sun50i_h6_ths_thermal_ops = {
	.get_temp = sun50i_h6_ths_get_temp,
};

static int sun50i_h6_ths_calibrate(struct sun50i_h6_ths_data *data)
{
	u16 *caldata;
	size_t callen;
	int i;
	int ft_temp;
	s16 ft_temp_orig_reg, diff, cal_val;
	u32 reg_val;

	caldata = nvmem_cell_read(data->calcell, &callen);
	if (IS_ERR(caldata))
		return PTR_ERR(caldata);

	if (callen < 2 + 2 * data->cfg->sensor_num) 
		return -EINVAL;

	if (!caldata[0])
		return -EINVAL;

	/*
	 * The calbration data on H6 is stored as temperature-value
	 * pair when being filled at factory test stage.
	 * The unit of stored FT temperature is 0.1 degreee celusis.
	 */
	ft_temp = (caldata[0] & THS_H6_CAL_FT_TEMP_MASK) * 100;
	ft_temp_orig_reg = sun50i_h6_ths_recalc_reg(ft_temp);

	for (i = 0; i < data->cfg->sensor_num; i++)
	{
		diff = (ft_temp_orig_reg - (s16)caldata[1 + i]);
		cal_val = THS_H6_CAL_DEFAULT - diff;

		if (cal_val & ~THS_H6_CAL_VAL_MASK) {
			pr_warn("Faulty thermal sensor %d calibration value, beyond the valid range.\n", i);
			continue;
		}

		if (i % 2) {
			reg_val = readl(data->regs + THS_H6_CDATA(i / 2));
			reg_val &= 0xffff;
			reg_val |= cal_val << 16;
			writel(reg_val, data->regs + THS_H6_CDATA(i / 2));
		} else {
			writel(cal_val, data->regs + THS_H6_CDATA(i / 2));
		}
	}
	
	kfree(caldata);
	return 0;
}

static int sun50i_h6_ths_probe(struct platform_device *pdev)
{
	struct sun50i_h6_ths_data *data;
	struct resource *res;
	int ret, irq, i;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->cfg = of_device_get_match_data(&pdev->dev);
	if (!data->cfg)
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "no memory resources defined\n");
		return -EINVAL;
	}

	data->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(data->regs)) {
		ret = PTR_ERR(data->regs);
		dev_err(&pdev->dev, "failed to ioremap THS registers: %d\n", ret);
		return ret;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "failed to get IRQ: %d\n", irq);
		return irq;
	}

	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					sun50i_h6_ths_irq_thread, IRQF_ONESHOT,
					dev_name(&pdev->dev), data);
	if (ret)
		return ret;

	data->busclk = devm_clk_get(&pdev->dev, "bus");
	if (IS_ERR(data->busclk)) {
		ret = PTR_ERR(data->busclk);
		dev_err(&pdev->dev, "failed to get ahb clk: %d\n", ret);
		return ret;
	}

	data->reset = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(data->reset)) {
		ret = PTR_ERR(data->reset);
		dev_err(&pdev->dev, "failed to get reset: %d\n", ret);
		return ret;
	}

	ret = reset_control_deassert(data->reset);
	if (ret) {
		dev_err(&pdev->dev, "reset deassert failed: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(data->busclk);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable bus clk: %d\n", ret);
		goto err_assert_reset;
	}

	data->calcell = devm_nvmem_cell_get(&pdev->dev, "calibration");
	if (IS_ERR(data->calcell)) {
		if (PTR_ERR(data->calcell) == -EPROBE_DEFER) {
			ret = PTR_ERR(data->calcell);
			goto err_disable_bus;
		}
		/*
		* Even if the external calibration data stored in eFUSE is
		* not accessible, the THS hardware can still work, although
		* the data won't be so accurate.
		* The default value of calibration register is 0x800 for
		* every sensor, and the calibration value is usually 0x7xx
		* or 0x8xx, so they won't be away from the default value
		* for a lot.
		* So here we do not return if the calibartion data is not
		* available, except the probe needs deferring.
		*/
	} else {
		ret = sun50i_h6_ths_calibrate(data);
		if (ret) {
			/* Revert calibrating */
			for (i = 0; i < data->cfg->sensor_num; i += 2) {
				writew(THS_H6_CAL_DEFAULT,
				       data->regs + THS_H6_CDATA(i / 2));
			}
		}
	}

	for (i = 0; i < data->cfg->sensor_num; i++) {
		data->sensors[i].data = data;
		data->sensors[i].id = i;
		data->sensors[i].tzd =
			devm_thermal_zone_of_sensor_register(&pdev->dev,
				i, &data->sensors[i], &sun50i_h6_ths_thermal_ops);
		if (IS_ERR(data->sensors[i].tzd)) {
			ret = PTR_ERR(data->sensors[i].tzd);
			dev_err(&pdev->dev,
				"failed to register thermal zone %d: %d\n",
				i, ret);
			goto err_disable_bus;
		}
	}

	sun50i_h6_ths_init(data);

	platform_set_drvdata(pdev, data);
	return 0;

err_disable_bus:
	clk_disable_unprepare(data->busclk);
err_assert_reset:
	reset_control_assert(data->reset);
	return ret;
}

static int sun50i_h6_ths_remove(struct platform_device *pdev)
{
	struct sun50i_h6_ths_data *data = platform_get_drvdata(pdev);

	reset_control_assert(data->reset);
	clk_disable_unprepare(data->busclk);
	return 0;
}

static const struct sun50i_h6_ths_cfg sun50i_h6_ths_cfg = {
	.sensor_num = 2,
	.calc_temp = sun50i_h6_ths_calc_temp,
};

static const struct of_device_id sun50i_h6_ths_id_table[] = {
	{ .compatible = "allwinner,sun50i-h6-ths", .data = &sun50i_h6_ths_cfg },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, sun50i_h6_ths_id_table);

static struct platform_driver sun50i_h6_ths_driver = {
	.probe = sun50i_h6_ths_probe,
	.remove = sun50i_h6_ths_remove,
	.driver = {
		.name = "sun50i_h6_ths",
		.of_match_table = sun50i_h6_ths_id_table,
	},
};

module_platform_driver(sun50i_h6_ths_driver);

MODULE_AUTHOR("Icenowy Zheng <icenowy@aosc.io>");
MODULE_DESCRIPTION("Thermal sensor driver for Allwinner H6");
MODULE_LICENSE("GPL v2");

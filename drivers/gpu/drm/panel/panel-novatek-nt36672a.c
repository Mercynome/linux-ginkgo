// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2020 Linaro Ltd
 * Author: Sumit Semwal <sumit.semwal@linaro.org>
 *
 * This driver is for the DSI interface to panels using the NT36672A display driver IC
 * from Novatek.
 * Currently supported are the Tianma FHD+ panels found in some Xiaomi phones, including
 * some variants of the Poco F1 phone.
 *
 * Panels using the Novatek NT37762A IC should add appropriate configuration per-panel and
 * use this driver.
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>

#include <linux/gpio/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

struct nt36672a_panel_cmd {
	const char data[2];
};

static const char * const nt36672a_regulator_names[] = {
	"vddio",
	"vddpos",
	"vddneg",
};

static unsigned long const nt36672a_regulator_enable_loads[] = {
	62000,
	100000,
	100000
};

struct nt36672a_panel_desc {
	const struct drm_display_mode *display_mode;
	const char *panel_name;

	unsigned int width_mm;
	unsigned int height_mm;

	unsigned long mode_flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;

	unsigned int num_on_cmds_1;
	const struct nt36672a_panel_cmd *on_cmds_1;
	unsigned int num_on_cmds_2;
	const struct nt36672a_panel_cmd *on_cmds_2;

	unsigned int num_off_cmds;
	const struct nt36672a_panel_cmd *off_cmds;
};

struct nt36672a_panel {
	struct drm_panel base;
	struct mipi_dsi_device *link;
	const struct nt36672a_panel_desc *desc;

	struct regulator_bulk_data supplies[ARRAY_SIZE(nt36672a_regulator_names)];

	struct gpio_desc *reset_gpio;
};

static inline struct nt36672a_panel *to_nt36672a_panel(struct drm_panel *panel)
{
	return container_of(panel, struct nt36672a_panel, base);
}

static void nt36672a_send_cmds(struct mipi_dsi_multi_context *dsi_ctx,
			       const struct nt36672a_panel_cmd *cmds, int num)
{
	unsigned int i;

	for (i = 0; i < num; i++) {
		const struct nt36672a_panel_cmd *cmd = &cmds[i];

		/* cmd->data[0] is the DCS command, cmd->data[1] is the parameter */
		mipi_dsi_dcs_write_buffer_multi(dsi_ctx, cmd->data, sizeof(cmd->data));
	}
}

static void nt36672a_panel_power_off(struct drm_panel *panel)
{
	struct nt36672a_panel *pinfo = to_nt36672a_panel(panel);
	int ret;

	gpiod_set_value(pinfo->reset_gpio, 1);

	ret = regulator_bulk_disable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret)
		dev_err(panel->dev, "regulator_bulk_disable failed %d\n", ret);
}

static int nt36672a_panel_unprepare(struct drm_panel *panel)
{
	struct nt36672a_panel *pinfo = to_nt36672a_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = pinfo->link };

	/* send off cmds */
	nt36672a_send_cmds(&dsi_ctx, pinfo->desc->off_cmds,
			   pinfo->desc->num_off_cmds);

	/* Reset error to continue with display off even if send_cmds failed */
	dsi_ctx.accum_err = 0;
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	/* Reset error to continue power-down even if display off failed */
	dsi_ctx.accum_err = 0;

	/* 120ms delay required here as per DCS spec */
	msleep(120);

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);

	/* 0x3C = 60ms delay */
	msleep(60);

	nt36672a_panel_power_off(panel);

	return 0;
}

static int nt36672a_panel_power_on(struct nt36672a_panel *pinfo)
{
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret < 0)
		return ret;

	/*
	 * As per downstream kernel, Reset sequence of Tianma FHD panel requires the panel to
	 * be out of reset for 10ms, followed by being held in reset for 10ms. But for Android
	 * AOSP, we needed to bump it upto 200ms otherwise we get white screen sometimes.
	 * FIXME: Try to reduce this 200ms to a lesser value.
	 */
	gpiod_set_value(pinfo->reset_gpio, 1);
	msleep(200);
	gpiod_set_value(pinfo->reset_gpio, 0);
	msleep(200);

	return 0;
}

static int nt36672a_panel_prepare(struct drm_panel *panel)
{
	struct nt36672a_panel *pinfo = to_nt36672a_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = pinfo->link };

	dsi_ctx.accum_err = nt36672a_panel_power_on(pinfo);

	/* send first part of init cmds */
	nt36672a_send_cmds(&dsi_ctx, pinfo->desc->on_cmds_1,
			   pinfo->desc->num_on_cmds_1);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);

	/* 0x50 = 80 ms delay (stock ginkgo) */
	mipi_dsi_msleep(&dsi_ctx, 80);

	/* Send rest of the init cmds */
	nt36672a_send_cmds(&dsi_ctx, pinfo->desc->on_cmds_2,
			   pinfo->desc->num_on_cmds_2);
	/* CABC/dimming config: 3-byte write from stock ginkgo DTBO */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x68, 0x04, 0x03);

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	mipi_dsi_msleep(&dsi_ctx, 120);

	if (dsi_ctx.accum_err < 0)
		gpiod_set_value(pinfo->reset_gpio, 0);

	return dsi_ctx.accum_err;
}

static int nt36672a_panel_get_modes(struct drm_panel *panel,
				    struct drm_connector *connector)
{
	struct nt36672a_panel *pinfo = to_nt36672a_panel(panel);
	const struct drm_display_mode *m = pinfo->desc->display_mode;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, m);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%u@%u\n", m->hdisplay,
			m->vdisplay, drm_mode_vrefresh(m));
		return -ENOMEM;
	}

	connector->display_info.width_mm = pinfo->desc->width_mm;
	connector->display_info.height_mm = pinfo->desc->height_mm;

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs panel_funcs = {
	.unprepare = nt36672a_panel_unprepare,
	.prepare = nt36672a_panel_prepare,
	.get_modes = nt36672a_panel_get_modes,
};

/* Stock ginkgo (Redmi Note 8) DTBO on-command, part before sleep-out */
static const struct nt36672a_panel_cmd tianma_fhd_video_on_cmds_1[] = {
	{ .data = {0xFF, 0x25} },
	{ .data = {0xFB, 0x01} },
	{ .data = {0x18, 0x96} },
	{ .data = {0x05, 0x04} },
	{ .data = {0xFF, 0x20} },
	{ .data = {0xFB, 0x01} },
	{ .data = {0x78, 0x01} },
	{ .data = {0xFF, 0x24} },
	{ .data = {0xFB, 0x01} },
	{ .data = {0x82, 0x13} },
	{ .data = {0x84, 0x31} },
	{ .data = {0x88, 0x13} },
	{ .data = {0x8A, 0x31} },
	{ .data = {0x8E, 0xE4} },
	{ .data = {0x8F, 0x01} },
	{ .data = {0x90, 0x80} },
	{ .data = {0xFF, 0x26} },
	{ .data = {0xFB, 0x01} },
	{ .data = {0xA9, 0x12} },
	{ .data = {0xAA, 0x10} },
	{ .data = {0xAE, 0x8A} },
	{ .data = {0xFF, 0x10} },
};

/* Stock ginkgo DTBO on-command, part after sleep-out (before 68 04 03 and display-on) */
static const struct nt36672a_panel_cmd tianma_fhd_video_on_cmds_2[] = {
	{ .data = {0xB0, 0x01} },
	{ .data = {0x35, 0x00} },
	{ .data = {0x51, 0xB8} },
	{ .data = {0x53, 0x2C} },
	{ .data = {0x55, 0x00} },
};

static const struct nt36672a_panel_cmd tianma_fhd_video_off_cmds[] = {
	{ .data = {0xFF, 0x10} },
};

static const struct drm_display_mode tianma_fhd_video_panel_default_mode = {
	/* Stock ginkgo DTBO dsi_nt36672a_tianma_video: 1080x2340@60 */
	.clock		= 183025,

	.hdisplay	= 1080,
	.hsync_start	= 1080 + 90,
	.hsync_end	= 1080 + 90 + 2,
	.htotal		= 1080 + 90 + 2 + 120,

	.vdisplay	= 2340,
	.vsync_start	= 2340 + 10,
	.vsync_end	= 2340 + 10 + 3,
	.vtotal		= 2340 + 10 + 3 + 8,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct nt36672a_panel_desc tianma_fhd_video_panel_desc = {
	.display_mode = &tianma_fhd_video_panel_default_mode,

	.width_mm = 67,
	.height_mm = 145,

	.mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_VIDEO
			| MIPI_DSI_MODE_VIDEO_HSE
			| MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
	.on_cmds_1 = tianma_fhd_video_on_cmds_1,
	.num_on_cmds_1 = ARRAY_SIZE(tianma_fhd_video_on_cmds_1),
	.on_cmds_2 = tianma_fhd_video_on_cmds_2,
	.num_on_cmds_2 = ARRAY_SIZE(tianma_fhd_video_on_cmds_2),
	.off_cmds = tianma_fhd_video_off_cmds,
	.num_off_cmds = ARRAY_SIZE(tianma_fhd_video_off_cmds),
};

static int nt36672a_panel_add(struct nt36672a_panel *pinfo)
{
	struct device *dev = &pinfo->link->dev;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++) {
		pinfo->supplies[i].supply = nt36672a_regulator_names[i];
		pinfo->supplies[i].init_load_uA = nt36672a_regulator_enable_loads[i];
	}

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(pinfo->supplies),
				      pinfo->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	pinfo->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(pinfo->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(pinfo->reset_gpio),
				     "failed to get reset gpio from DT\n");

	ret = drm_panel_of_backlight(&pinfo->base);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	pinfo->base.prepare_prev_first = true;
	drm_panel_add(&pinfo->base);

	return 0;
}

static int nt36672a_panel_probe(struct mipi_dsi_device *dsi)
{
	struct nt36672a_panel *pinfo;
	const struct nt36672a_panel_desc *desc;
	int err;

	pinfo = devm_drm_panel_alloc(&dsi->dev, __typeof(*pinfo), base,
				     &panel_funcs, DRM_MODE_CONNECTOR_DSI);

	if (IS_ERR(pinfo))
		return PTR_ERR(pinfo);

	desc = of_device_get_match_data(&dsi->dev);
	dsi->mode_flags = desc->mode_flags;
	dsi->format = desc->format;
	dsi->lanes = desc->lanes;
	pinfo->desc = desc;
	pinfo->link = dsi;

	mipi_dsi_set_drvdata(dsi, pinfo);

	err = nt36672a_panel_add(pinfo);
	if (err < 0)
		return err;

	err = mipi_dsi_attach(dsi);
	if (err < 0) {
		drm_panel_remove(&pinfo->base);
		return err;
	}

	return 0;
}

static void nt36672a_panel_remove(struct mipi_dsi_device *dsi)
{
	struct nt36672a_panel *pinfo = mipi_dsi_get_drvdata(dsi);
	int err;

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", err);

	drm_panel_remove(&pinfo->base);
}

static const struct of_device_id tianma_fhd_video_of_match[] = {
	{ .compatible = "tianma,fhd-video", .data = &tianma_fhd_video_panel_desc },
	{ },
};
MODULE_DEVICE_TABLE(of, tianma_fhd_video_of_match);

static struct mipi_dsi_driver nt36672a_panel_driver = {
	.driver = {
		.name = "panel-tianma-nt36672a",
		.of_match_table = tianma_fhd_video_of_match,
	},
	.probe = nt36672a_panel_probe,
	.remove = nt36672a_panel_remove,
};
module_mipi_dsi_driver(nt36672a_panel_driver);

MODULE_AUTHOR("Sumit Semwal <sumit.semwal@linaro.org>");
MODULE_DESCRIPTION("NOVATEK NT36672A based MIPI-DSI LCD panel driver");
MODULE_LICENSE("GPL");

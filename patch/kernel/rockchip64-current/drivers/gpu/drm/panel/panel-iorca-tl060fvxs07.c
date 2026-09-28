#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mipi-dsi/driver.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct tl060_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset;
	struct backlight_device *backlight;
};

static inline struct tl060_panel *to_tl060_panel(struct drm_panel *panel)
{
	return container_of(panel, struct tl060_panel, panel);
}

static const struct drm_display_mode tl060_mode = {
	.clock = 135000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 115,
	.hsync_end = 1080 + 115 + 5,
	.htotal = 1080 + 115 + 5 + 15,
	.vdisplay = 2160,
	.vsync_start = 2160 + 10,
	.vsync_end = 2160 + 10 + 5,
	.vtotal = 2160 + 10 + 5 + 10,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int tl060_prepare(struct drm_panel *panel)
{
	struct tl060_panel *ctx = to_tl060_panel(panel);
	int ret;

	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(120);
	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(120);

	ret = mipi_dsi_dcs_exit_sleep_mode(ctx->dsi);
	if (ret < 0)
		return ret;

	msleep(120);
	return 0;
}

static int tl060_enable(struct drm_panel *panel)
{
	struct tl060_panel *ctx = to_tl060_panel(panel);
	int ret;

	ret = mipi_dsi_dcs_set_display_on(ctx->dsi);
	if (ret < 0)
		return ret;

	msleep(20);
	return 0;
}

static int tl060_disable(struct drm_panel *panel)
{
	struct tl060_panel *ctx = to_tl060_panel(panel);
	int ret;

	ret = mipi_dsi_dcs_set_display_off(ctx->dsi);
	if (ret < 0)
		return ret;

	msleep(10);
	return mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
}

static int tl060_unprepare(struct drm_panel *panel)
{
	struct tl060_panel *ctx = to_tl060_panel(panel);

	msleep(120);
	gpiod_set_value_cansleep(ctx->reset, 1);
	return 0;
}

static int tl060_get_modes(struct drm_panel *panel,
				   struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &tl060_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	mode->width_mm = 68;
	mode->height_mm = 136;
	drm_mode_probed_add(connector, mode);

	connector->display_info.bpc = 8;
	connector->display_info.bus_flags = DRM_BUS_FLAG_DE_HIGH;
	connector->display_info.width_mm = 68;
	connector->display_info.height_mm = 136;
	return 1;
}

static const struct drm_panel_funcs tl060_panel_funcs = {
	.prepare = tl060_prepare,
	.enable = tl060_enable,
	.disable = tl060_disable,
	.unprepare = tl060_unprepare,
	.get_modes = tl060_get_modes,
};

static int tl060_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct tl060_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct tl060_panel, panel,
				  &tl060_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->dsi = dsi;
	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset),
				     "failed to get reset GPIO\n");

	ctx->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(ctx->backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->backlight),
				     "failed to get backlight\n");

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO |
			  MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM |
			  MIPI_DSI_MODE_EOT_PACKET;

	mipi_dsi_set_drvdata(dsi, ctx);
	drm_panel_init(&ctx->panel, dev, &tl060_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	ret = drm_panel_add(&ctx->panel);
	if (ret)
		return ret;

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void tl060_remove(struct mipi_dsi_device *dsi)
{
	struct tl060_panel *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id tl060_of_match[] = {
	{ .compatible = "iorca,tl060fvxs07" },
	{ }
};
MODULE_DEVICE_TABLE(of, tl060_of_match);

static struct mipi_dsi_driver tl060_driver = {
	.probe = tl060_probe,
	.remove = tl060_remove,
	.driver = {
		.name = "panel-iorca-tl060fvxs07",
		.of_match_table = tl060_of_match,
	},
};
module_mipi_dsi_driver(tl060_driver);

MODULE_AUTHOR("iorca");
MODULE_DESCRIPTION("Tianma TL060FVXS07 MIPI DSI panel");
MODULE_LICENSE("GPL");

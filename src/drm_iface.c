// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for 2.7" Sharp Memory LCD
 *
 * Copyright 2023 Andrew D'Angelo
 */

#include <linux/version.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/sched/clock.h>
#include <linux/spi/spi.h>
#include <linux/mutex.h>
#include <linux/list.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_rect.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#include <drm/drm_fbdev_generic.h>
#endif

#include "params_iface.h"
#include "ioctl_iface.h"
#include "drm_iface.h"

#define CMD_CLEAR_SCREEN 0b00100000

// Globals

struct overlay_storage_t
{
	struct list_head list;
	struct sharp_overlay_t overlay;
};

struct overlay_display_t
{
	struct list_head list;
	struct overlay_storage_t *storage;
};

static LIST_HEAD(g_overlays);
static LIST_HEAD(g_visible_overlays);

struct sharp_memory_panel
{
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	const struct drm_display_mode *mode;
	struct drm_connector connector;
	struct spi_device *spi;
	struct drm_framebuffer *fb;

	struct timer_list vcom_timer;

	unsigned int height;
	unsigned int width;

	unsigned char *buf;
	struct spi_transfer *spi_3_xfers;
	unsigned char *cmd_buf;
	unsigned char *trailer_buf;

	struct gpio_desc *gpio_disp;
	struct gpio_desc *gpio_vcom;
	struct gpio_desc *gpio_backlit;
};

static inline struct sharp_memory_panel *drm_to_panel(struct drm_device *drm)
{
	return container_of(drm, struct sharp_memory_panel, drm);
}

bool backlit_on;

static void vcom_timer_callback(struct timer_list *t)
{
	static u8 vcom_setting = 0;

	struct sharp_memory_panel *panel = from_timer(panel, t, vcom_timer);

	// Toggle the GPIO pin
	vcom_setting = (vcom_setting) ? 0 : 1;
	gpiod_set_value(panel->gpio_vcom, vcom_setting);

	if (backlit_on != g_param_backlit) {
		backlit_on = g_param_backlit;
		gpiod_set_value(panel->gpio_backlit, backlit_on ? 1 : 0);
	}

	// Reschedule the timer
	mod_timer(&panel->vcom_timer, jiffies + msecs_to_jiffies(1000));
}

static int sharp_memory_spi_clear_screen(struct sharp_memory_panel *panel)
{
	int rc;

	// Create screen clear command SPI transfer
	panel->cmd_buf[0] = CMD_CLEAR_SCREEN;
	panel->spi_3_xfers[0].tx_buf = panel->cmd_buf;
	panel->spi_3_xfers[0].len = 1;
	panel->trailer_buf[0] = 0;
	panel->spi_3_xfers[1].tx_buf = panel->trailer_buf;
	panel->spi_3_xfers[1].len = 1;

	// Write clear screen command
	ndelay(80);

	rc = spi_sync_transfer(panel->spi, panel->spi_3_xfers, 2);

	return rc;
}

static int sharp_memory_spi_write_tagged_lines(struct sharp_memory_panel *panel,
	void *line_data, size_t len)
{
	int rc;

	// Write line command
	panel->cmd_buf[0] = g_param_color ? 0b10000000 : 0b10001000;
	panel->spi_3_xfers[0].tx_buf = panel->cmd_buf;
	panel->spi_3_xfers[0].len = 1;

	// Line data
	panel->spi_3_xfers[1].tx_buf = line_data;
	panel->spi_3_xfers[1].len = len;

	// Trailer
	panel->trailer_buf[0] = 0;
	panel->spi_3_xfers[2].tx_buf = panel->trailer_buf;
	panel->spi_3_xfers[2].len = 1;

	ndelay(80);

	rc = spi_sync_transfer(panel->spi, panel->spi_3_xfers, 3);

	return rc;
}

static void draw_overlays(struct sharp_memory_panel *panel, u8* buf, int width,
	struct drm_rect const* clip)
{
	int x, y, dx, dy, sx, sy;
	struct overlay_display_t *p;
	struct sharp_overlay_t const *ov;

	list_for_each_entry(p, &g_visible_overlays, list) {
		ov = &p->storage->overlay;
		x = (ov->x < 0) ? (panel->width + ov->x) : ov->x;
		y = (ov->y < 0) ? (panel->height + ov->y) : ov->y;

		// Any overlap?
		if (((y + ov->height) < clip->y1) || (clip->y2 < y)) {
			continue;
		}

		// Draw overlay pixels
		for (sy = 0; sy < ov->height; sy++) {

			// Skip lines before clip
			if ((y + sy) < clip->y1) {
				continue;
			// Exit if reached end of clip
			} else if (clip->y2 <= (y + sy)) {
				break;
			}

			dy = sy - clip->y1;

			for (sx = 0; sx < ov->width; sx++) {

				if (sx < clip->x1) {
					continue;
				} else if (clip->x2 <= sx) {
					break;
				}

				dx = (x + sx) - clip->x1;
				if (g_param_color) {
					int i = 3 * ((y + dy) * (clip->x2 - clip->x1) + dx);
					int p = ov->pixels[(sy * ov->width) + sx];
					buf[i + 2] = p;
				} else {
					buf[(y + dy) * (clip->x2 - clip->x1) + dx]
						= ov->pixels[(sy * ov->width) + sx];
				}
			}
		}
	}
}

// Based on sharp_memory_gray8_to_mono_tagged.
static size_t sharp_memory_to_color_tagged(u8 *buf, int width, int height, int y0) {
	int line, b8, b1;
	unsigned int d;
	int const tagged_line_len = 2 + width / 8;
	for (line = 0; line < height; line++) {
		// Look at 8 3 byte sections.
		for (b8 = 0; b8 < width; b8 += 24) {
			d = 0;
			for (b1 = 0; b1 < 24; b1++) {
				if (buf[(line * width) + b8 + b1] >= g_param_color_cutoff) {
					d |= (0b10000000 << 16) >> b1;
				}
			}
			int i = (line * tagged_line_len) + 1 + (b8 / 8);
			// Flip red and blue.
			int mask = 0b1001001001001001001001;
			d = (d & mask << 2) >> 2 | (d & mask << 1) | (d & mask) << 2;
			buf[i] = d >> 16;
			buf[i+1] = d >> 8;
			buf[i+2] = d;
		}
		buf[line * tagged_line_len] = (u8)(y0 + 1); // Indexed from 1
		buf[(line * tagged_line_len) + tagged_line_len - 1] = 0;
		y0++;
	}
	return height * tagged_line_len;
}

static size_t sharp_memory_gray8_to_mono_tagged(u8 *buf, int width, int height, int y0)
{
	int line, b8, b1;
	unsigned char d;
	int const tagged_line_len = 2 + width / 8;

	// Iterate over lines from [0, height)
	for (line = 0; line < height; line++) {

		// Iterate over chunks of 8 source grayscale bytes
		// Each 8-byte source chunk will map to one destination mono byte
		for (b8 = 0; b8 < width; b8 += 8) {
			d = 0;

			// Iterate over each of the 8 grayscale bytes in the chunk
			// Build up the destination mono byte
			for (b1 = 0; b1 < 8; b1++) {

				// Change at what gray level the mono pixel is active here
				if (buf[(line * width) + b8 + b1] >= g_param_mono_cutoff) {
					d |= 0b10000000 >> b1;
				}
			}

			// Apply inversion
			if (g_param_mono_invert) {
				d = ~d;
			}

			// Without the line number and trailer tags, each destination
			// mono line would have a length `width / 8`. However, we are
			// inserting the line number at the beginning of the line and
			// the zero-byte trailer at the end.
			// So the destination mono line is at index
			// `line * tagged_line_len = line * (2 + width / 8)`
			// The destination mono byte is offset by 1 to make room for
			// the line tag, written at the end of converting the current
			// line.
			buf[(line * tagged_line_len) + 1 + (b8 / 8)] = d;
		}

		// Write the line number and trailer tags
		buf[line * tagged_line_len] = (u8)(y0 + 1); // Indexed from 1
		buf[(line * tagged_line_len) + tagged_line_len - 1] = 0;
		y0++;
	}

	return height * tagged_line_len;
}

// Use DMA to get grayscale representation, then convert to mono
// with line number and trailer tags suitable for multi-line write
// Output is stored in `buf`, which must be at least W*H bytes
static int sharp_memory_clip_mono_tagged(struct sharp_memory_panel* panel, size_t* result_len,
	u8* buf, struct drm_framebuffer *fb, struct drm_rect const* clip)
{
	int rc;
	struct drm_gem_dma_object *dma_obj;
	struct iosys_map dst, vmap;

	// Get GEM memory manager
	dma_obj = drm_fb_dma_get_gem_obj(fb, 0);

	// Start DMA area
	rc = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (rc) {
		return rc;
	}

	// Initialize destination (buf) and source (video)
	iosys_map_set_vaddr(&dst, buf);
	iosys_map_set_vaddr(&vmap, dma_obj->vaddr);
	if (g_param_color) {
		drm_fb_xrgb8888_to_rgb888(&dst, NULL, &vmap, fb, clip);
		drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
		if (g_param_overlays) {
			draw_overlays(panel, buf, fb->width, clip);
		}
		*result_len = sharp_memory_to_color_tagged(buf,
			(clip->x2 - clip->x1) * 3, (clip->y2 - clip->y1), clip->y1);
	} else {
		// DMA `clip` into `buf` and convert to 8-bit grayscale
		drm_fb_xrgb8888_to_gray8(&dst, NULL, &vmap, fb, clip);

		// End DMA area
		drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);

		// Add overlays
		if (g_param_overlays) {

			// TODO: track overlay draw region
			draw_overlays(panel, buf, fb->width, clip);
		}

		// Convert in-place from 8-bit grayscale to mono
		*result_len = sharp_memory_gray8_to_mono_tagged(buf,
			(clip->x2 - clip->x1), (clip->y2 - clip->y1), clip->y1);
	}

	// Success
	return 0;
}

static int sharp_memory_fb_dirty(struct drm_framebuffer *fb,
	struct drm_rect const* dirty_rect)
{
	int rc;
	struct drm_rect clip;
	struct sharp_memory_panel *panel;
	int drm_idx;
	size_t buf_len;

	// Clip dirty region rows
	clip.x1 = 0;
	clip.x2 = fb->width;
	clip.y1 = dirty_rect->y1;
	clip.y2 = dirty_rect->y2;

	// Get panel info from DRM struct
	panel = drm_to_panel(fb->dev);

	// Enter DRM device resource area
	if (!drm_dev_enter(fb->dev, &drm_idx)) {
		return -ENODEV;
	}

	// Convert `clip` from framebuffer to mono with line number tags
	rc = sharp_memory_clip_mono_tagged(panel, &buf_len, panel->buf, fb, &clip);
	if (rc) {
		goto out_exit;
	}

	// Write mono data to display
	rc = sharp_memory_spi_write_tagged_lines(panel, panel->buf, buf_len);

out_exit:
	// Exit DRM device resource area
	drm_dev_exit(drm_idx);

	return rc;
}


static void power_off(struct sharp_memory_panel *panel)
{
	printk(KERN_INFO "sharp_memory: powering off\n");

	// Clear display if auto clear is set
	if (g_param_auto_clear) {
		(void)sharp_memory_spi_clear_screen(panel);
	}

	/* Turn off power and all signals */
	if (panel->gpio_disp) {
		gpiod_set_value(panel->gpio_disp, 0);
	}
	gpiod_set_value(panel->gpio_vcom, 0);
	gpiod_set_value(panel->gpio_backlit, 0);
}

static void sharp_memory_pipe_enable(struct drm_simple_display_pipe *pipe,
	struct drm_crtc_state *crtc_state, struct drm_plane_state *plane_state)
{
	struct sharp_memory_panel *panel;
	struct spi_device *spi;
	int drm_idx;

	printk(KERN_INFO "sharp_memory: entering sharp_memory_pipe_enable\n");

	// Get panel and SPI device structs
	panel = drm_to_panel(pipe->crtc.dev);
	spi = panel->spi;

	// Enter DRM resource area
	if (!drm_dev_enter(pipe->crtc.dev, &drm_idx)) {
		return;
	}

	// Power up sequence
	if (panel->gpio_disp) {
		gpiod_set_value(panel->gpio_disp, 1);
	}
	gpiod_set_value(panel->gpio_vcom, 0);
	usleep_range(5000, 10000);

	// Clear display
	if (sharp_memory_spi_clear_screen(panel)) {
		if (panel->gpio_disp) {
			gpiod_set_value(panel->gpio_disp, 0); // Power down display, VCOM is not running
		}
		goto out_exit;
	}

	// Initialize and schedule the VCOM timer
	timer_setup(&panel->vcom_timer, vcom_timer_callback, 0);
	mod_timer(&panel->vcom_timer, jiffies + msecs_to_jiffies(500));

	printk(KERN_INFO "sharp_memory: completed sharp_memory_pipe_enable\n");

out_exit:
	drm_dev_exit(drm_idx);
}

static void sharp_memory_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct sharp_memory_panel *panel;
	struct spi_device *spi;

	printk(KERN_INFO "sharp_memory: sharp_memory_pipe_disable\n");

	// Get panel and SPI device structs
	panel = drm_to_panel(pipe->crtc.dev);
	spi = panel->spi;

	// Cancel the timer
	del_timer_sync(&panel->vcom_timer);

	power_off(panel);
}

static void sharp_memory_pipe_update(struct drm_simple_display_pipe *pipe,
				struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_rect rect;

	if (!pipe->crtc.state->active) {
		return;
	}

	if (drm_atomic_helper_damage_merged(old_state, state, &rect)) {
		sharp_memory_fb_dirty(state->fb, &rect);
	}
}

static const struct drm_simple_display_pipe_funcs sharp_memory_pipe_funcs = {
	.enable = sharp_memory_pipe_enable,
	.disable = sharp_memory_pipe_disable,
	.update = sharp_memory_pipe_update
	// .prepare_fb and .cleanup_fb are handled automatically when not set
};

static int sharp_memory_connector_get_modes(struct drm_connector *connector)
{
	struct sharp_memory_panel *panel = drm_to_panel(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, panel->mode);
}

static struct drm_framebuffer* create_and_store_fb(struct drm_device *dev,
	struct drm_file *file, const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct sharp_memory_panel *panel;

	// Initialize framebuffer
	panel = drm_to_panel(dev);
	panel->fb = drm_gem_fb_create_with_dirty(dev, file, mode_cmd);
	return panel->fb;
}

static const struct drm_connector_helper_funcs sharp_memory_connector_hfuncs = {
	.get_modes = sharp_memory_connector_get_modes,
};

static const struct drm_connector_funcs sharp_memory_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs sharp_memory_mode_config_funcs = {
	.fb_create = create_and_store_fb,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const uint32_t sharp_memory_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static const struct drm_display_mode sharp_memory_ls027b7dh01_mode = {
	DRM_SIMPLE_MODE(400, 240, 59, 35),
};

DEFINE_DRM_GEM_DMA_FOPS(sharp_memory_fops);

static const struct drm_ioctl_desc sharp_memory_ioctls[] = {
	DRM_IOCTL_DEF_DRV_REDRAW,
	DRM_IOCTL_DEF_DRV_OV_ADD,
	DRM_IOCTL_DEF_DRV_OV_REM,
	DRM_IOCTL_DEF_DRV_OV_SHOW,
	DRM_IOCTL_DEF_DRV_OV_HIDE,
	DRM_IOCTL_DEF_DRV_OV_CLEAR
};

static const struct drm_driver sharp_memory_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &sharp_memory_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	.name = "sharp_drm",
	.desc = "Sharp Memory LCD panel",
	.date = "20230713",
	.major = 1,
	.minor = 1,

	.ioctls = sharp_memory_ioctls,
	.num_ioctls = ARRAY_SIZE(sharp_memory_ioctls)
};

int drm_probe(struct spi_device *spi)
{
	const struct drm_display_mode *mode;
	struct device *dev;
	struct sharp_memory_panel *panel;
	struct drm_device *drm;
	int ret;

	printk(KERN_INFO "sharp_memory: entering drm_probe\n");

	// Get DRM device from SPI struct
	dev = &spi->dev;

	// The SPI device is used to allocate DMA memory
	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_warn(dev, "Failed to set dma mask %d\n", ret);
			return ret;
		}
	}

	// Allocate panel storage
	panel = devm_drm_dev_alloc(dev, &sharp_memory_driver,
		struct sharp_memory_panel, drm);
	if (IS_ERR(panel)) {
		printk(KERN_ERR "sharp_memory: failed to allocate panel\n");
		return PTR_ERR(panel);
	}

	// Initialize GPIO
	panel->gpio_disp = devm_gpiod_get_optional(dev, "disp", GPIOD_OUT_HIGH);
	if (IS_ERR(panel->gpio_disp))
		return dev_err_probe(dev, PTR_ERR(panel->gpio_disp), "Failed to get GPIO 'disp'\n");

	panel->gpio_vcom = devm_gpiod_get(dev, "vcom", GPIOD_OUT_LOW);
	if (IS_ERR(panel->gpio_vcom))
		return dev_err_probe(dev, PTR_ERR(panel->gpio_vcom), "Failed to get GPIO 'vcom'\n");

	panel->gpio_backlit = devm_gpiod_get(dev, "backlit", GPIOD_OUT_LOW);
	if (IS_ERR(panel->gpio_backlit))
		return dev_err_probe(dev, PTR_ERR(panel->gpio_backlit), "Failed to get GPIO 'backlit'\n");

	// Initalize DRM mode
	drm = &panel->drm;
	ret = drmm_mode_config_init(drm);
	if (ret) {
		return ret;
	}
	drm->mode_config.funcs = &sharp_memory_mode_config_funcs;

	// Initialize panel contents
	panel->spi = spi;
	panel->fb = NULL;
	mode = &sharp_memory_ls027b7dh01_mode;
	panel->mode = mode;
	panel->width = mode->hdisplay;
	panel->height = mode->vdisplay;

	// Allocate reused heap buffers suitable for SPI source
	panel->buf = devm_kzalloc(dev, panel->width * panel->height * 3, GFP_KERNEL);
	panel->spi_3_xfers = devm_kzalloc(dev, sizeof(struct spi_transfer) * 3, GFP_KERNEL);
	panel->cmd_buf = devm_kzalloc(dev, 1, GFP_KERNEL);
	panel->trailer_buf = devm_kzalloc(dev, 1, GFP_KERNEL);

	// DRM mode settings
	drm->mode_config.min_width = mode->hdisplay;
	drm->mode_config.max_width = mode->hdisplay;
	drm->mode_config.min_height = mode->vdisplay;
	drm->mode_config.max_height = mode->vdisplay;

	// Configure DRM connector
	ret = drm_connector_init(drm, &panel->connector, &sharp_memory_connector_funcs,
		DRM_MODE_CONNECTOR_SPI);
	if (ret) {
		return ret;
	}
	drm_connector_helper_add(&panel->connector, &sharp_memory_connector_hfuncs);

	// Initialize DRM pipe
	ret = drm_simple_display_pipe_init(drm, &panel->pipe, &sharp_memory_pipe_funcs,
		sharp_memory_formats, ARRAY_SIZE(sharp_memory_formats),
		NULL, &panel->connector);
	if (ret) {
		return ret;
	}

	// Enable damaged screen area clips
	drm_plane_enable_fb_damage_clips(&panel->pipe.plane);

	drm_mode_config_reset(drm);

	printk(KERN_INFO "sharp_memory: registering DRM device\n");
	ret = drm_dev_register(drm, 0);
	if (ret) {
		return ret;
	}

	// fbdev setup
	spi_set_drvdata(spi, drm);
	drm_fbdev_generic_setup(drm, 0);

	printk(KERN_INFO "sharp_memory: successful probe\n");

	return 0;
}

void drm_remove(struct spi_device *spi)
{
	struct drm_device *drm;
	struct device *dev;
	struct sharp_memory_panel *panel;

	printk(KERN_INFO "sharp_memory: drm_remove\n");

	// Remove all overlays
	drm_clear_overlays();

	// Get DRM and panel device from SPI
	drm = spi_get_drvdata(spi);

	// Clean up the GPIO descriptors
	dev = &spi->dev;
	panel = drm_to_panel(drm);

	if (panel->gpio_disp) {
		devm_gpiod_put(dev, panel->gpio_disp);
	}
	devm_gpiod_put(dev, panel->gpio_vcom);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

int drm_redraw_fb(struct drm_device *drm, int height)
{
	struct sharp_memory_panel *panel;
	struct drm_framebuffer *fb;
	struct drm_clip_rect dirty_rect;

	if (!drm
	 || ((panel = drm_to_panel(drm)) == NULL)
	 || ((fb = panel->fb) == NULL)
	 || !fb->funcs || !fb->funcs->dirty) {
		return 0;
	}

	// Create dirty region
	dirty_rect.x1 = 0;
	dirty_rect.x2 = fb->width;
	dirty_rect.y1 = 0;
	dirty_rect.y2 = (height > 0)
		? height
		: fb->height;

	// Call framebuffer region update handler
	return fb->funcs->dirty(fb, NULL, 0, 0, &dirty_rect, 1);
}

void* drm_add_overlay(int x, int y, int width, int height,
	unsigned char const* pixels)
{
	void *chunk = kmalloc(sizeof(struct overlay_storage_t), GFP_KERNEL);

	struct overlay_storage_t *entry = (struct overlay_storage_t *)chunk;
	entry->overlay.x = x;
	entry->overlay.y = y;
	entry->overlay.width = width;
	entry->overlay.height = height;
	entry->overlay.pixels = kmemdup(pixels, width * height, GFP_KERNEL);

	INIT_LIST_HEAD(&entry->list);
	list_add_tail(&entry->list, &g_overlays);

	return entry;
}

void drm_remove_overlay(void* entry_)
{
	struct overlay_storage_t *entry = (struct overlay_storage_t *)entry_;

	list_del(&entry->list);
	kfree(entry->overlay.pixels);
	kfree(entry);
}

void drm_clear_overlays(void)
{
	{
		struct overlay_display_t *ptr, *next;
		list_for_each_entry_safe(ptr, next, &g_visible_overlays, list) {
			drm_hide_overlay(ptr);
		}
	}

	{
		struct overlay_storage_t *ptr, *next;
		list_for_each_entry_safe(ptr, next, &g_overlays, list) {
			drm_remove_overlay(ptr);
		}
	}
}

void* drm_show_overlay(void* storage_)
{
	void *chunk = kmalloc(sizeof(struct overlay_display_t), GFP_KERNEL);
	struct overlay_display_t *entry = (struct overlay_display_t *)chunk;

	entry->storage = (struct overlay_storage_t *)storage_;

	INIT_LIST_HEAD(&entry->list);
	list_add_tail(&entry->list, &g_visible_overlays);

	return entry;
}

void drm_hide_overlay(void* entry_)
{
	struct overlay_display_t *entry = (struct overlay_display_t *)entry_;

	list_del(&entry->list);
	kfree(entry);
}


/*
 * Copyright © 2008 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Keith Packard <keithp@keithp.com>
 *
 */

/*
 * Notes regarding the adaptation to OpenBSD Xenocara's Intel Xorg driver:
 *
 * - Removed audio support.
 * - Removed hotplug event support.
 * - Disabled PCH panel fitting.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <unistd.h>

#include "xf86.h"
#include "i830.h"
#include "xf86Modes.h"
#include "i830_display.h"

#include "i830_dp.h"

static __inline void
msleep(unsigned int msecs)
{
	if (usleep(1000*msecs) == EINVAL)
		FatalError("%s: invalid argument: %u\n", __FUNCTION__, msecs);
}

/*
 * Wait for COND to become true, timeout of MS milliseconds,
 * retry every 1 millisecond.
 */
#define wait_for(COND, MS) ({						\
	unsigned int i;							\
	int ret__ = 0;							\
	for (i = 0; ! (COND); i++) {					\
		if (i >= MS) {						\
			ret__ = ETIMEDOUT;				\
			break;						\
		}							\
		msleep(1);						\
	}								\
	ret__;								\
})

#define HAS_PCH_SPLIT(x)	IS_IGDNG(x)
#define HAS_PCH_CPT(x)		FALSE
#define IS_GEN6(x)		FALSE

#define I915_WRITE(reg, val)	OUTREG(reg, val)
#define I915_READ(reg)		INREG(reg)
#define DRM_ERROR(fmt, arg...)		ErrorF(fmt, ## arg)
#define DRM_DEBUG_KMS(fmt, arg...)	DPRINTF(PFX, fmt, ## arg)


#define DP_LINK_STATUS_SIZE	6
#define DP_LINK_CHECK_TIMEOUT	(10 * 1000)

#define DP_LINK_CONFIGURATION_SIZE	9

struct i830_dp_priv {
	uint32_t output_reg;
	uint32_t DP;
	uint8_t  link_configuration[DP_LINK_CONFIGURATION_SIZE];
	int dpms_mode;
	uint8_t link_bw;
	uint8_t lane_count;
	uint8_t dpcd[4];
	struct {
		Bool running;
		uint16_t address;
		Bool reading;
	} i2c_state;
	Bool is_pch_edp;
	uint8_t	train_set[4];
	uint8_t link_status[DP_LINK_STATUS_SIZE];
};

static I2CBusPtr i830_dp_i2c_init(xf86OutputPtr, struct i830_dp_priv *, const char *);

/**
 * is_edp - is the given port attached to an eDP panel (either CPU or PCH)
 * @dev_priv: DP struct
 *
 * If a CPU or PCH DP output is attached to an eDP panel, this function
 * will return true, and false otherwise.
 */
static Bool is_edp(xf86OutputPtr output)
{
	return ((I830OutputPrivatePtr)output->driver_private)->type == I830_OUTPUT_EDP;
}

/**
 * is_pch_edp - is the port on the PCH and attached to an eDP panel?
 * @dev_priv: DP struct
 *
 * Returns true if the given DP struct corresponds to a PCH DP port attached
 * to an eDP panel, false otherwise.  Helpful for determining whether we
 * may need FDI resources for a given DP output or not.
 */
static Bool is_pch_edp(struct i830_dp_priv *dev_priv)
{
	return dev_priv->is_pch_edp;
}

static struct i830_dp_priv *output_to_i830_dp(xf86OutputPtr output)
{
	return ((I830OutputPrivatePtr)output->driver_private)->dev_priv;
}

/**
 * i830_encoder_is_pch_edp - is the given output a PCH attached eDP?
 * @output: DRM encoder
 *
 * Return true if @output corresponds to a PCH attached eDP panel.  Needed
 * by intel_display.c.
 */
Bool i830_encoder_is_pch_edp(xf86OutputPtr output)
{
	struct i830_dp_priv *dev_priv;

	if (!output)
		return FALSE;

	dev_priv = output_to_i830_dp(output);

	return is_pch_edp(dev_priv);
}

static void i830_dp_start_link_train(xf86OutputPtr output);
static void i830_dp_complete_link_train(xf86OutputPtr output);
static void i830_dp_link_down(xf86OutputPtr output);

void
i830_edp_link_config (I830OutputPrivatePtr intel_output,
		       int *lane_num, int *link_bw)
{
	struct i830_dp_priv *dev_priv = intel_output->dev_priv;

	*lane_num = dev_priv->lane_count;
	if (dev_priv->link_bw == DP_LINK_BW_1_62)
		*link_bw = 162000;
	else if (dev_priv->link_bw == DP_LINK_BW_2_7)
		*link_bw = 270000;
}

static int
i830_dp_max_lane_count(struct i830_dp_priv *dev_priv)
{
	int max_lane_count = 4;

	if (dev_priv->dpcd[0] >= 0x11) {
		max_lane_count = dev_priv->dpcd[2] & 0x1f;
		switch (max_lane_count) {
		case 1: case 2: case 4:
			break;
		default:
			max_lane_count = 4;
		}
	}
	return max_lane_count;
}

static int
i830_dp_max_link_bw(struct i830_dp_priv *dev_priv)
{
	int max_link_bw = dev_priv->dpcd[1];

	switch (max_link_bw) {
	case DP_LINK_BW_1_62:
	case DP_LINK_BW_2_7:
		break;
	default:
		max_link_bw = DP_LINK_BW_1_62;
		break;
	}
	return max_link_bw;
}

static int
i830_dp_link_clock(uint8_t link_bw)
{
	if (link_bw == DP_LINK_BW_2_7)
		return 270000;
	else
		return 162000;
}

/* I think this is a fiction */
static int
i830_dp_link_required(xf86OutputPtr output, struct i830_dp_priv *dev_priv, int pixel_clock)
{
	intel_screen_private *intel = intel_get_screen_private(output->scrn);

	if (is_edp(output))
		return (pixel_clock * intel->edp.bpp + 7) / 8;
	else
		return pixel_clock * 3;
}

static int
i830_dp_max_data_rate(int max_link_clock, int max_lanes)
{
	return (max_link_clock * max_lanes * 8) / 10;
}

static int
i830_dp_mode_valid(xf86OutputPtr output,
		    DisplayModePtr mode)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	ScrnInfoPtr scrn = output->scrn;
	intel_screen_private *intel = intel_get_screen_private(scrn);
	int max_link_clock = i830_dp_link_clock(i830_dp_max_link_bw(dev_priv));
	int max_lanes = i830_dp_max_lane_count(dev_priv);

	if (is_edp(output) && intel->panel_fixed_mode) {
		if (mode->HDisplay > intel->panel_fixed_mode->HDisplay)
			return MODE_PANEL;

		if (mode->VDisplay > intel->panel_fixed_mode->VDisplay)
			return MODE_PANEL;
	}

	/* only refuse the mode on non eDP since we have seen some wierd eDP panels
	   which are outside spec tolerances but somehow work by magic */
	if (!is_edp(output) &&
	    (i830_dp_link_required(output, dev_priv, mode->Clock)
	     > i830_dp_max_data_rate(max_link_clock, max_lanes)))
		return MODE_CLOCK_HIGH;

	if (mode->Clock < 10000)
		return MODE_CLOCK_LOW;

	return MODE_OK;
}

static uint32_t
pack_aux(uint8_t *src, int src_bytes)
{
	int	i;
	uint32_t v = 0;

	if (src_bytes > 4)
		src_bytes = 4;
	for (i = 0; i < src_bytes; i++)
		v |= ((uint32_t) src[i]) << ((3-i) * 8);
	return v;
}

static void
unpack_aux(uint32_t src, uint8_t *dst, int dst_bytes)
{
	int i;
	if (dst_bytes > 4)
		dst_bytes = 4;
	for (i = 0; i < dst_bytes; i++)
		dst[i] = src >> ((3-i) * 8);
}

/* hrawclock is 1/4 the FSB frequency */
static int
intel_hrawclk(ScrnInfoPtr scrn)
{
	intel_screen_private *intel = intel_get_screen_private(scrn);
	uint32_t clkcfg;

	clkcfg = I915_READ(CLKCFG);
	switch (clkcfg & CLKCFG_FSB_MASK) {
	case CLKCFG_FSB_400:
		return 100;
	case CLKCFG_FSB_533:
		return 133;
	case CLKCFG_FSB_667:
		return 166;
	case CLKCFG_FSB_800:
		return 200;
	case CLKCFG_FSB_1067:
		return 266;
	case CLKCFG_FSB_1333:
		return 333;
	/* these two are just a guess; one of them might be right */
	case CLKCFG_FSB_1600:
	case CLKCFG_FSB_1600_ALT:
		return 400;
	default:
		return 133;
	}
}

static int
i830_dp_aux_ch(xf86OutputPtr output,
		uint8_t *send, int send_bytes,
		uint8_t *recv, int recv_size)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	uint32_t output_reg = dev_priv->output_reg;
	ScrnInfoPtr scrn = output->scrn;
	intel_screen_private *intel = intel_get_screen_private(scrn);
	uint32_t ch_ctl = output_reg + 0x10;
	uint32_t ch_data = ch_ctl + 4;
	int i;
	int recv_bytes;
	uint32_t status;
	uint32_t aux_clock_divider;
	int try, precharge;

	/* The clock divider is based off the hrawclk,
	 * and would like to run at 2MHz. So, take the
	 * hrawclk value and divide by 2 and use that
	 *
	 * Note that PCH attached eDP panels should use a 125MHz input
	 * clock divider.
	 */
	if (is_edp(output) && !is_pch_edp(dev_priv)) {
		if (IS_GEN6(intel))
			aux_clock_divider = 200; /* SNB eDP input clock at 400Mhz */
		else
			aux_clock_divider = 225; /* eDP input clock at 450Mhz */
	} else if (HAS_PCH_SPLIT(intel))
		aux_clock_divider = 62; /* IRL input clock fixed at 125Mhz */
	else
		aux_clock_divider = intel_hrawclk(scrn) / 2;

	if (IS_GEN6(intel))
		precharge = 3;
	else
		precharge = 5;

	if (I915_READ(ch_ctl) & DP_AUX_CH_CTL_SEND_BUSY) {
		DRM_ERROR("dp_aux_ch not started status 0x%08x\n",
			  I915_READ(ch_ctl));
		return -EBUSY;
	}

	/* Must try at least 3 times according to DP spec */
	for (try = 0; try < 5; try++) {
		/* Load the send data into the aux channel data registers */
		for (i = 0; i < send_bytes; i += 4)
			I915_WRITE(ch_data + i,
				   pack_aux(send + i, send_bytes - i));
	
		/* Send the command and wait for it to complete */
		I915_WRITE(ch_ctl,
			   DP_AUX_CH_CTL_SEND_BUSY |
			   DP_AUX_CH_CTL_TIME_OUT_400us |
			   (send_bytes << DP_AUX_CH_CTL_MESSAGE_SIZE_SHIFT) |
			   (precharge << DP_AUX_CH_CTL_PRECHARGE_2US_SHIFT) |
			   (aux_clock_divider << DP_AUX_CH_CTL_BIT_CLOCK_2X_SHIFT) |
			   DP_AUX_CH_CTL_DONE |
			   DP_AUX_CH_CTL_TIME_OUT_ERROR |
			   DP_AUX_CH_CTL_RECEIVE_ERROR);
		for (;;) {
			status = I915_READ(ch_ctl);
			if ((status & DP_AUX_CH_CTL_SEND_BUSY) == 0)
				break;
			usleep(100);
		}
	
		/* Clear done status and any errors */
		I915_WRITE(ch_ctl,
			   status |
			   DP_AUX_CH_CTL_DONE |
			   DP_AUX_CH_CTL_TIME_OUT_ERROR |
			   DP_AUX_CH_CTL_RECEIVE_ERROR);
		if (status & DP_AUX_CH_CTL_DONE)
			break;
	}

	if ((status & DP_AUX_CH_CTL_DONE) == 0) {
		DRM_ERROR("dp_aux_ch not done status 0x%08x\n", status);
		return -EBUSY;
	}

	/* Check for timeout or receive error.
	 * Timeouts occur when the sink is not connected
	 */
	if (status & DP_AUX_CH_CTL_RECEIVE_ERROR) {
		DRM_ERROR("dp_aux_ch receive error status 0x%08x\n", status);
		return -EIO;
	}

	/* Timeouts occur when the device isn't connected, so they're
	 * "normal" -- don't fill the kernel log with these */
	if (status & DP_AUX_CH_CTL_TIME_OUT_ERROR) {
		DRM_DEBUG_KMS("dp_aux_ch timeout status 0x%08x\n", status);
		return -ETIMEDOUT;
	}

	/* Unload any bytes sent back from the other side */
	recv_bytes = ((status & DP_AUX_CH_CTL_MESSAGE_SIZE_MASK) >>
		      DP_AUX_CH_CTL_MESSAGE_SIZE_SHIFT);
	if (recv_bytes > recv_size)
		recv_bytes = recv_size;
	
	for (i = 0; i < recv_bytes; i += 4)
		unpack_aux(I915_READ(ch_data + i),
			   recv + i, recv_bytes - i);

	return recv_bytes;
}

/* Write data to the aux channel in native mode */
static int
i830_dp_aux_native_write(xf86OutputPtr output,
			  uint16_t address, uint8_t *send, int send_bytes)
{
	int ret;
	uint8_t	msg[20];
	int msg_bytes;
	uint8_t	ack;

	if (send_bytes > 16)
		return -1;
	msg[0] = AUX_NATIVE_WRITE << 4;
	msg[1] = address >> 8;
	msg[2] = address & 0xff;
	msg[3] = send_bytes - 1;
	memcpy(&msg[4], send, send_bytes);
	msg_bytes = send_bytes + 4;
	for (;;) {
		ret = i830_dp_aux_ch(output, msg, msg_bytes, &ack, 1);
		if (ret < 0)
			return ret;
		if ((ack & AUX_NATIVE_REPLY_MASK) == AUX_NATIVE_REPLY_ACK)
			break;
		else if ((ack & AUX_NATIVE_REPLY_MASK) == AUX_NATIVE_REPLY_DEFER)
			usleep(100);
		else
			return -EIO;
	}
	return send_bytes;
}

/* Write a single byte to the aux channel in native mode */
static int
i830_dp_aux_native_write_1(xf86OutputPtr output,
			    uint16_t address, uint8_t byte)
{
	return i830_dp_aux_native_write(output, address, &byte, 1);
}

/* read bytes from a native aux channel */
static int
i830_dp_aux_native_read(xf86OutputPtr output,
			 uint16_t address, uint8_t *recv, int recv_bytes)
{
	uint8_t msg[4];
	int msg_bytes;
	uint8_t reply[20];
	int reply_bytes;
	uint8_t ack;
	int ret;

	msg[0] = AUX_NATIVE_READ << 4;
	msg[1] = address >> 8;
	msg[2] = address & 0xff;
	msg[3] = recv_bytes - 1;

	msg_bytes = 4;
	reply_bytes = recv_bytes + 1;

	for (;;) {
		ret = i830_dp_aux_ch(output, msg, msg_bytes,
				      reply, reply_bytes);
		if (ret == 0)
			return -EPROTO;
		if (ret < 0)
			return ret;
		ack = reply[0];
		if ((ack & AUX_NATIVE_REPLY_MASK) == AUX_NATIVE_REPLY_ACK) {
			memcpy(recv, reply + 1, ret - 1);
			return ret - 1;
		}
		else if ((ack & AUX_NATIVE_REPLY_MASK) == AUX_NATIVE_REPLY_DEFER)
			usleep(100);
		else
			return -EIO;
	}
}

static Bool
i830_dp_i2c_aux_ch(xf86OutputPtr output, int mode,
		    uint8_t write_byte, uint8_t *read_byte)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	uint16_t address = dev_priv->i2c_state.address;
	uint8_t msg[5];
	uint8_t reply[2];
	unsigned retry;
	int msg_bytes;
	int reply_bytes;
	int ret;

	/* Set up the command byte */
	if (mode & MODE_I2C_READ)
		msg[0] = AUX_I2C_READ << 4;
	else
		msg[0] = AUX_I2C_WRITE << 4;

	if (!(mode & MODE_I2C_STOP))
		msg[0] |= AUX_I2C_MOT << 4;

	msg[1] = address >> 8;
	msg[2] = address;

	switch (mode) {
	case MODE_I2C_WRITE:
		msg[3] = 0;
		msg[4] = write_byte;
		msg_bytes = 5;
		reply_bytes = 1;
		break;
	case MODE_I2C_READ:
		msg[3] = 0;
		msg_bytes = 4;
		reply_bytes = 2;
		break;
	default:
		msg_bytes = 3;
		reply_bytes = 1;
		break;
	}

	for (retry = 0; retry < 5; retry++) {
		ret = i830_dp_aux_ch(output,
				      msg, msg_bytes,
				      reply, reply_bytes);
		if (ret < 0) {
			DRM_DEBUG_KMS("aux_ch failed %d\n", ret);
			return FALSE;
		}

		switch (reply[0] & AUX_NATIVE_REPLY_MASK) {
		case AUX_NATIVE_REPLY_ACK:
			/* I2C-over-AUX Reply field is only valid
			 * when paired with AUX ACK.
			 */
			break;
		case AUX_NATIVE_REPLY_NACK:
			DRM_DEBUG_KMS("aux_ch native nack\n");
			return FALSE;
		case AUX_NATIVE_REPLY_DEFER:
			usleep(100);
			continue;
		default:
			DRM_ERROR("aux_ch invalid native reply 0x%02x\n",
				  reply[0]);
			return FALSE;
		}

		switch (reply[0] & AUX_I2C_REPLY_MASK) {
		case AUX_I2C_REPLY_ACK:
			if (mode == MODE_I2C_READ) {
				*read_byte = reply[1];
			}
			return TRUE;
		case AUX_I2C_REPLY_NACK:
			DRM_DEBUG_KMS("aux_i2c nack\n");
			return FALSE;
		case AUX_I2C_REPLY_DEFER:
			DRM_DEBUG_KMS("aux_i2c defer\n");
			usleep(100);
			break;
		default:
			DRM_ERROR("aux_i2c invalid reply 0x%02x\n", reply[0]);
			return FALSE;
		}
	}

	DRM_ERROR("too many retries, giving up\n");
	return FALSE;
}

/* Taken from Linux intel_panel.c. */
static void
intel_fixed_panel_mode(DisplayModePtr fixed_mode,
		       DisplayModePtr adjusted_mode)
{
	adjusted_mode->HDisplay = fixed_mode->HDisplay;
	adjusted_mode->HSyncStart = fixed_mode->HSyncStart;
	adjusted_mode->HSyncEnd = fixed_mode->HSyncEnd;
	adjusted_mode->HTotal = fixed_mode->HTotal;

	adjusted_mode->VDisplay = fixed_mode->VDisplay;
	adjusted_mode->VSyncStart = fixed_mode->VSyncStart;
	adjusted_mode->VSyncEnd = fixed_mode->VSyncEnd;
	adjusted_mode->VTotal = fixed_mode->VTotal;

	adjusted_mode->Clock = fixed_mode->Clock;

	xf86SetModeCrtc(adjusted_mode, INTERLACE_HALVE_V);
}

static Bool
i830_dp_mode_fixup(xf86OutputPtr output, DisplayModePtr mode,
		    DisplayModePtr adjusted_mode)
{
	ScrnInfoPtr scrn = output->scrn;
	intel_screen_private *intel = intel_get_screen_private(scrn);
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	int lane_count, clock;
	int max_lane_count = i830_dp_max_lane_count(dev_priv);
	int max_clock = i830_dp_max_link_bw(dev_priv) == DP_LINK_BW_2_7 ? 1 : 0;
	static int bws[2] = { DP_LINK_BW_1_62, DP_LINK_BW_2_7 };

	if (is_edp(output) && intel->panel_fixed_mode) {
		intel_fixed_panel_mode(intel->panel_fixed_mode, adjusted_mode);
#if 0	/* To properly support this, the LVDS driver would need some work. */
		intel_pch_panel_fitting(scrn, DRM_MODE_SCALE_FULLSCREEN,
					mode, adjusted_mode);
#endif
		/*
		 * the mode->Clock is used to calculate the Data&Link M/N
		 * of the pipe. For the eDP the fixed clock should be used.
		 */
		mode->Clock = intel->panel_fixed_mode->Clock;
	}

	for (lane_count = 1; lane_count <= max_lane_count; lane_count <<= 1) {
		for (clock = 0; clock <= max_clock; clock++) {
			int link_avail = i830_dp_max_data_rate(i830_dp_link_clock(bws[clock]), lane_count);

			if (i830_dp_link_required(output, dev_priv, mode->Clock)
					<= link_avail) {
				dev_priv->link_bw = bws[clock];
				dev_priv->lane_count = lane_count;
				adjusted_mode->Clock = i830_dp_link_clock(dev_priv->link_bw);
				DRM_DEBUG_KMS("Display port link bw %02x lane "
						"count %d clock %d\n",
				       dev_priv->link_bw, dev_priv->lane_count,
				       adjusted_mode->Clock);
				return TRUE;
			}
		}
	}

	if (is_edp(output)) {
		/* okay we failed just pick the highest */
		dev_priv->lane_count = max_lane_count;
		dev_priv->link_bw = bws[max_clock];
		adjusted_mode->Clock = i830_dp_link_clock(dev_priv->link_bw);
		DRM_DEBUG_KMS("Force picking display port link bw %02x lane "
			      "count %d clock %d\n",
			      dev_priv->link_bw, dev_priv->lane_count,
			      adjusted_mode->Clock);

		return TRUE;
	}

	return FALSE;
}

struct i830_dp_priv_m_n {
	uint32_t	tu;
	uint32_t	gmch_m;
	uint32_t	gmch_n;
	uint32_t	link_m;
	uint32_t	link_n;
};

static void
intel_reduce_ratio(uint32_t *num, uint32_t *den)
{
	while (*num > 0xffffff || *den > 0xffffff) {
		*num >>= 1;
		*den >>= 1;
	}
}

static void
i830_dp_compute_m_n(int bpp,
		     int nlanes,
		     int pixel_clock,
		     int link_clock,
		     struct i830_dp_priv_m_n *m_n)
{
	m_n->tu = 64;
	m_n->gmch_m = (pixel_clock * bpp) >> 3;
	m_n->gmch_n = link_clock * nlanes;
	intel_reduce_ratio(&m_n->gmch_m, &m_n->gmch_n);
	m_n->link_m = pixel_clock;
	m_n->link_n = link_clock;
	intel_reduce_ratio(&m_n->link_m, &m_n->link_n);
}

void
i830_dp_set_m_n(xf86CrtcPtr crtc, DisplayModePtr mode,
		 DisplayModePtr adjusted_mode)
{
	ScrnInfoPtr scrn = crtc->scrn;
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
	xf86OutputPtr output;
	intel_screen_private *intel = intel_get_screen_private(scrn);
	I830CrtcPrivatePtr intel_crtc = crtc->driver_private;
	int lane_count = 4, bpp = 24;
	struct i830_dp_priv_m_n m_n;
	int i;

	/*
	 * Find the lane count in the intel_output private
	 */
	for (i = 0; i < xf86_config->num_output; i++) {
		xf86OutputPtr output = xf86_config->output[i];
		struct i830_dp_priv *dev_priv;

		if (output->crtc != crtc)
			continue;

		dev_priv = output_to_i830_dp(output);
		if (((I830OutputPrivatePtr)output->driver_private)->type == I830_OUTPUT_DISPLAYPORT) {
			lane_count = dev_priv->lane_count;
			break;
		} else if (is_edp(output)) {
			lane_count = intel->edp.lanes;
			bpp = intel->edp.bpp;
			break;
		}
	}

	/*
	 * Compute the GMCH and Link ratios. The '3' here is
	 * the number of bytes_per_pixel post-LUT, which we always
	 * set up for 8-bits of R/G/B, or 3 bytes total.
	 */
	i830_dp_compute_m_n(bpp, lane_count,
			     mode->Clock, adjusted_mode->Clock, &m_n);

	if (HAS_PCH_SPLIT(intel)) {
		if (intel_crtc->pipe == 0) {
			I915_WRITE(TRANSA_DATA_M1,
				   ((m_n.tu - 1) << PIPE_GMCH_DATA_M_TU_SIZE_SHIFT) |
				   m_n.gmch_m);
			I915_WRITE(TRANSA_DATA_N1, m_n.gmch_n);
			I915_WRITE(TRANSA_DP_LINK_M1, m_n.link_m);
			I915_WRITE(TRANSA_DP_LINK_N1, m_n.link_n);
		} else {
			I915_WRITE(TRANSB_DATA_M1,
				   ((m_n.tu - 1) << PIPE_GMCH_DATA_M_TU_SIZE_SHIFT) |
				   m_n.gmch_m);
			I915_WRITE(TRANSB_DATA_N1, m_n.gmch_n);
			I915_WRITE(TRANSB_DP_LINK_M1, m_n.link_m);
			I915_WRITE(TRANSB_DP_LINK_N1, m_n.link_n);
		}
	} else {
		if (intel_crtc->pipe == 0) {
			I915_WRITE(PIPEA_GMCH_DATA_M,
				   ((m_n.tu - 1) << PIPE_GMCH_DATA_M_TU_SIZE_SHIFT) |
				   m_n.gmch_m);
			I915_WRITE(PIPEA_GMCH_DATA_N,
				   m_n.gmch_n);
			I915_WRITE(PIPEA_DP_LINK_M, m_n.link_m);
			I915_WRITE(PIPEA_DP_LINK_N, m_n.link_n);
		} else {
			I915_WRITE(PIPEB_GMCH_DATA_M,
				   ((m_n.tu - 1) << PIPE_GMCH_DATA_M_TU_SIZE_SHIFT) |
				   m_n.gmch_m);
			I915_WRITE(PIPEB_GMCH_DATA_N,
					m_n.gmch_n);
			I915_WRITE(PIPEB_DP_LINK_M, m_n.link_m);
			I915_WRITE(PIPEB_DP_LINK_N, m_n.link_n);
		}
	}
}

static void
i830_dp_mode_set(xf86OutputPtr output, DisplayModePtr mode,
		  DisplayModePtr adjusted_mode)
{
	ScrnInfoPtr scrn = output->scrn;
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	xf86CrtcPtr crtc = output->crtc;
	I830CrtcPrivatePtr intel_crtc = crtc->driver_private;

	dev_priv->DP = (DP_VOLTAGE_0_4 |
		       DP_PRE_EMPHASIS_0);

	if (adjusted_mode->Flags & DRM_MODE_FLAG_PHSYNC)
		dev_priv->DP |= DP_SYNC_HS_HIGH;
	if (adjusted_mode->Flags & DRM_MODE_FLAG_PVSYNC)
		dev_priv->DP |= DP_SYNC_VS_HIGH;

	if (HAS_PCH_CPT(intel) && !is_edp(output))
		dev_priv->DP |= DP_LINK_TRAIN_OFF_CPT;
	else
		dev_priv->DP |= DP_LINK_TRAIN_OFF;

	switch (dev_priv->lane_count) {
	case 1:
		dev_priv->DP |= DP_PORT_WIDTH_1;
		break;
	case 2:
		dev_priv->DP |= DP_PORT_WIDTH_2;
		break;
	case 4:
		dev_priv->DP |= DP_PORT_WIDTH_4;
		break;
	}

	memset(dev_priv->link_configuration, 0, DP_LINK_CONFIGURATION_SIZE);
	dev_priv->link_configuration[0] = dev_priv->link_bw;
	dev_priv->link_configuration[1] = dev_priv->lane_count;

	/*
	 * Check for DPCD version > 1.1 and enhanced framing support
	 */
	if (dev_priv->dpcd[0] >= 0x11 && (dev_priv->dpcd[2] & DP_ENHANCED_FRAME_CAP)) {
		dev_priv->link_configuration[1] |= DP_LANE_COUNT_ENHANCED_FRAME_EN;
		dev_priv->DP |= DP_ENHANCED_FRAMING;
	}

	/* CPT DP's pipe select is decided in TRANS_DP_CTL */
	if (intel_crtc->pipe == 1 && !HAS_PCH_CPT(intel))
		dev_priv->DP |= DP_PIPEB_SELECT;

	if (is_edp(output) && !is_pch_edp(dev_priv)) {
		/* don't miss out required setting for eDP */
		dev_priv->DP |= DP_PLL_ENABLE;
		if (adjusted_mode->Clock < 200000)
			dev_priv->DP |= DP_PLL_FREQ_160MHZ;
		else
			dev_priv->DP |= DP_PLL_FREQ_270MHZ;
	}
}

/* Returns true if the panel was already on when called */
static Bool ironlake_edp_panel_on (ScrnInfoPtr scrn)
{
	intel_screen_private *intel = intel_get_screen_private(scrn);
	uint32_t pp, idle_on_mask = PP_ON | PP_SEQUENCE_STATE_ON_IDLE;

	if (I915_READ(PCH_PP_STATUS) & PP_ON)
		return TRUE;

	pp = I915_READ(PCH_PP_CONTROL);

	/* ILK workaround: disable reset around power sequence */
	pp &= ~PANEL_POWER_RESET;
	I915_WRITE(PCH_PP_CONTROL, pp);
	POSTING_READ(PCH_PP_CONTROL);

	pp |= PANEL_UNLOCK_REGS | POWER_TARGET_ON;
	I915_WRITE(PCH_PP_CONTROL, pp);
	POSTING_READ(PCH_PP_CONTROL);

	/* Ouch. We need to wait here for some panels, like Dell e6510
	 * https://bugs.freedesktop.org/show_bug.cgi?id=29278i
	 */
	msleep(300);

	if (wait_for((I915_READ(PCH_PP_STATUS) & idle_on_mask) == idle_on_mask,
		     5000))
		DRM_ERROR("panel on wait timed out: 0x%08x\n",
			  I915_READ(PCH_PP_STATUS));

	pp |= PANEL_POWER_RESET; /* restore panel reset bit */
	I915_WRITE(PCH_PP_CONTROL, pp);
	POSTING_READ(PCH_PP_CONTROL);

	return FALSE;
}

static void ironlake_edp_panel_off (ScrnInfoPtr scrn)
{
	intel_screen_private *intel = intel_get_screen_private(scrn);
	uint32_t pp, idle_off_mask = PP_ON | PP_SEQUENCE_MASK |
		PP_CYCLE_DELAY_ACTIVE | PP_SEQUENCE_STATE_MASK;

	pp = I915_READ(PCH_PP_CONTROL);

	/* ILK workaround: disable reset around power sequence */
	pp &= ~PANEL_POWER_RESET;
	I915_WRITE(PCH_PP_CONTROL, pp);
	POSTING_READ(PCH_PP_CONTROL);

	pp &= ~POWER_TARGET_ON;
	I915_WRITE(PCH_PP_CONTROL, pp);
	POSTING_READ(PCH_PP_CONTROL);

	if (wait_for((I915_READ(PCH_PP_STATUS) & idle_off_mask) == 0, 5000))
		DRM_ERROR("panel off wait timed out: 0x%08x\n",
			  I915_READ(PCH_PP_STATUS));

	pp |= PANEL_POWER_RESET; /* restore panel reset bit */
	I915_WRITE(PCH_PP_CONTROL, pp);
	POSTING_READ(PCH_PP_CONTROL);

	/* Ouch. We need to wait here for some panels, like Dell e6510
	 * https://bugs.freedesktop.org/show_bug.cgi?id=29278i
	 */
	msleep(300);
}

static void ironlake_edp_backlight_on (ScrnInfoPtr scrn)
{
	intel_screen_private *intel = intel_get_screen_private(scrn);
	uint32_t pp;

	DRM_DEBUG_KMS("\n");
	/*
	 * If we enable the backlight right away following a panel power
	 * on, we may see slight flicker as the panel syncs with the eDP
	 * link.  So delay a bit to make sure the image is solid before
	 * allowing it to appear.
	 */
	msleep(300);
	pp = I915_READ(PCH_PP_CONTROL);
	pp |= EDP_BLC_ENABLE;
	I915_WRITE(PCH_PP_CONTROL, pp);
}

static void ironlake_edp_backlight_off (ScrnInfoPtr scrn)
{
	intel_screen_private *intel = intel_get_screen_private(scrn);
	uint32_t pp;

	DRM_DEBUG_KMS("\n");
	pp = I915_READ(PCH_PP_CONTROL);
	pp &= ~EDP_BLC_ENABLE;
	I915_WRITE(PCH_PP_CONTROL, pp);
}

static void ironlake_edp_pll_on(xf86OutputPtr output)
{
	ScrnInfoPtr scrn = output->scrn;
	intel_screen_private *intel = intel_get_screen_private(scrn);
	uint32_t dpa_ctl;

	DRM_DEBUG_KMS("\n");
	dpa_ctl = I915_READ(DP_A);
	dpa_ctl |= DP_PLL_ENABLE;
	I915_WRITE(DP_A, dpa_ctl);
	POSTING_READ(DP_A);
	usleep(200);
}

static void ironlake_edp_pll_off(xf86OutputPtr output)
{
	ScrnInfoPtr scrn = output->scrn;
	intel_screen_private *intel = intel_get_screen_private(scrn);
	uint32_t dpa_ctl;

	dpa_ctl = I915_READ(DP_A);
	dpa_ctl &= ~DP_PLL_ENABLE;
	I915_WRITE(DP_A, dpa_ctl);
	POSTING_READ(DP_A);
	usleep(200);
}

static void i830_dp_prepare(xf86OutputPtr output)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	ScrnInfoPtr scrn = output->scrn;

	if (is_edp(output)) {
		ironlake_edp_backlight_off(scrn);
		ironlake_edp_panel_on(scrn);
		if (!is_pch_edp(dev_priv))
			ironlake_edp_pll_on(output);
		else
			ironlake_edp_pll_off(output);
	}
	i830_dp_link_down(output);
}

static void i830_dp_commit(xf86OutputPtr output)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	ScrnInfoPtr scrn = output->scrn;

	i830_dp_start_link_train(output);

	if (is_edp(output))
		ironlake_edp_panel_on(scrn);

	i830_dp_complete_link_train(output);

	if (is_edp(output))
		ironlake_edp_backlight_on(scrn);
}

static void
i830_dp_dpms(xf86OutputPtr output, int mode)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	ScrnInfoPtr scrn = output->scrn;
	intel_screen_private *intel = intel_get_screen_private(scrn);
	uint32_t dp_reg = I915_READ(dev_priv->output_reg);

	if (mode != DRM_MODE_DPMS_ON) {
		if (is_edp(output))
			ironlake_edp_backlight_off(scrn);
		i830_dp_link_down(output);
		if (is_edp(output))
			ironlake_edp_panel_off(scrn);
		if (is_edp(output) && !is_pch_edp(dev_priv))
			ironlake_edp_pll_off(output);
	} else {
		if (is_edp(output))
			ironlake_edp_panel_on(scrn);
		if (!(dp_reg & DP_PORT_EN)) {
			i830_dp_start_link_train(output);
			i830_dp_complete_link_train(output);
		}
		if (is_edp(output))
			ironlake_edp_backlight_on(scrn);
	}
	dev_priv->dpms_mode = mode;
}

/*
 * Fetch AUX CH registers 0x202 - 0x207 which contain
 * link status information
 */
static Bool
i830_dp_get_link_status(xf86OutputPtr output)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	int ret;

	ret = i830_dp_aux_native_read(output,
				       DP_LANE0_1_STATUS,
				       dev_priv->link_status, DP_LINK_STATUS_SIZE);
	if (ret != DP_LINK_STATUS_SIZE)
		return FALSE;
	return TRUE;
}

static uint8_t
i830_dp_link_status(uint8_t link_status[DP_LINK_STATUS_SIZE],
		     int r)
{
	return link_status[r - DP_LANE0_1_STATUS];
}

static uint8_t
intel_get_adjust_request_voltage(uint8_t link_status[DP_LINK_STATUS_SIZE],
				 int lane)
{
	int	    i = DP_ADJUST_REQUEST_LANE0_1 + (lane >> 1);
	int	    s = ((lane & 1) ?
			 DP_ADJUST_VOLTAGE_SWING_LANE1_SHIFT :
			 DP_ADJUST_VOLTAGE_SWING_LANE0_SHIFT);
	uint8_t l = i830_dp_link_status(link_status, i);

	return ((l >> s) & 3) << DP_TRAIN_VOLTAGE_SWING_SHIFT;
}

static uint8_t
intel_get_adjust_request_pre_emphasis(uint8_t link_status[DP_LINK_STATUS_SIZE],
				      int lane)
{
	int	    i = DP_ADJUST_REQUEST_LANE0_1 + (lane >> 1);
	int	    s = ((lane & 1) ?
			 DP_ADJUST_PRE_EMPHASIS_LANE1_SHIFT :
			 DP_ADJUST_PRE_EMPHASIS_LANE0_SHIFT);
	uint8_t l = i830_dp_link_status(link_status, i);

	return ((l >> s) & 3) << DP_TRAIN_PRE_EMPHASIS_SHIFT;
}


#if 0
static char	*voltage_names[] = {
	"0.4V", "0.6V", "0.8V", "1.2V"
};
static char	*pre_emph_names[] = {
	"0dB", "3.5dB", "6dB", "9.5dB"
};
static char	*link_train_names[] = {
	"pattern 1", "pattern 2", "idle", "off"
};
#endif

/*
 * These are source-specific values; current Intel hardware supports
 * a maximum voltage of 800mV and a maximum pre-emphasis of 6dB
 */
#define I830_DP_VOLTAGE_MAX	    DP_TRAIN_VOLTAGE_SWING_800

static uint8_t
i830_dp_pre_emphasis_max(uint8_t voltage_swing)
{
	switch (voltage_swing & DP_TRAIN_VOLTAGE_SWING_MASK) {
	case DP_TRAIN_VOLTAGE_SWING_400:
		return DP_TRAIN_PRE_EMPHASIS_6;
	case DP_TRAIN_VOLTAGE_SWING_600:
		return DP_TRAIN_PRE_EMPHASIS_6;
	case DP_TRAIN_VOLTAGE_SWING_800:
		return DP_TRAIN_PRE_EMPHASIS_3_5;
	case DP_TRAIN_VOLTAGE_SWING_1200:
	default:
		return DP_TRAIN_PRE_EMPHASIS_0;
	}
}

static void
intel_get_adjust_train(struct i830_dp_priv *dev_priv)
{
	uint8_t v = 0;
	uint8_t p = 0;
	int lane;

	for (lane = 0; lane < dev_priv->lane_count; lane++) {
		uint8_t this_v = intel_get_adjust_request_voltage(dev_priv->link_status, lane);
		uint8_t this_p = intel_get_adjust_request_pre_emphasis(dev_priv->link_status, lane);

		if (this_v > v)
			v = this_v;
		if (this_p > p)
			p = this_p;
	}

	if (v >= I830_DP_VOLTAGE_MAX)
		v = I830_DP_VOLTAGE_MAX | DP_TRAIN_MAX_SWING_REACHED;

	if (p >= i830_dp_pre_emphasis_max(v))
		p = i830_dp_pre_emphasis_max(v) | DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;

	for (lane = 0; lane < 4; lane++)
		dev_priv->train_set[lane] = v | p;
}

static uint32_t
i830_dp_signal_levels(uint8_t train_set, int lane_count)
{
	uint32_t	signal_levels = 0;

	switch (train_set & DP_TRAIN_VOLTAGE_SWING_MASK) {
	case DP_TRAIN_VOLTAGE_SWING_400:
	default:
		signal_levels |= DP_VOLTAGE_0_4;
		break;
	case DP_TRAIN_VOLTAGE_SWING_600:
		signal_levels |= DP_VOLTAGE_0_6;
		break;
	case DP_TRAIN_VOLTAGE_SWING_800:
		signal_levels |= DP_VOLTAGE_0_8;
		break;
	case DP_TRAIN_VOLTAGE_SWING_1200:
		signal_levels |= DP_VOLTAGE_1_2;
		break;
	}
	switch (train_set & DP_TRAIN_PRE_EMPHASIS_MASK) {
	case DP_TRAIN_PRE_EMPHASIS_0:
	default:
		signal_levels |= DP_PRE_EMPHASIS_0;
		break;
	case DP_TRAIN_PRE_EMPHASIS_3_5:
		signal_levels |= DP_PRE_EMPHASIS_3_5;
		break;
	case DP_TRAIN_PRE_EMPHASIS_6:
		signal_levels |= DP_PRE_EMPHASIS_6;
		break;
	case DP_TRAIN_PRE_EMPHASIS_9_5:
		signal_levels |= DP_PRE_EMPHASIS_9_5;
		break;
	}
	return signal_levels;
}

/* Gen6's DP voltage swing and pre-emphasis control */
static uint32_t
intel_gen6_edp_signal_levels(uint8_t train_set)
{
	switch (train_set & (DP_TRAIN_VOLTAGE_SWING_MASK|DP_TRAIN_PRE_EMPHASIS_MASK)) {
	case DP_TRAIN_VOLTAGE_SWING_400 | DP_TRAIN_PRE_EMPHASIS_0:
		return EDP_LINK_TRAIN_400MV_0DB_SNB_B;
	case DP_TRAIN_VOLTAGE_SWING_400 | DP_TRAIN_PRE_EMPHASIS_6:
		return EDP_LINK_TRAIN_400MV_6DB_SNB_B;
	case DP_TRAIN_VOLTAGE_SWING_600 | DP_TRAIN_PRE_EMPHASIS_3_5:
		return EDP_LINK_TRAIN_600MV_3_5DB_SNB_B;
	case DP_TRAIN_VOLTAGE_SWING_800 | DP_TRAIN_PRE_EMPHASIS_0:
		return EDP_LINK_TRAIN_800MV_0DB_SNB_B;
	default:
		DRM_DEBUG_KMS("Unsupported voltage swing/pre-emphasis level\n");
		return EDP_LINK_TRAIN_400MV_0DB_SNB_B;
	}
}

static uint8_t
intel_get_lane_status(uint8_t link_status[DP_LINK_STATUS_SIZE],
		      int lane)
{
	int i = DP_LANE0_1_STATUS + (lane >> 1);
	int s = (lane & 1) * 4;
	uint8_t l = i830_dp_link_status(link_status, i);

	return (l >> s) & 0xf;
}

/* Check for clock recovery is done on all channels */
static Bool
intel_clock_recovery_ok(uint8_t link_status[DP_LINK_STATUS_SIZE], int lane_count)
{
	int lane;
	uint8_t lane_status;

	for (lane = 0; lane < lane_count; lane++) {
		lane_status = intel_get_lane_status(link_status, lane);
		if ((lane_status & DP_LANE_CR_DONE) == 0)
			return FALSE;
	}
	return TRUE;
}

/* Check to see if channel eq is done on all channels */
#define CHANNEL_EQ_BITS (DP_LANE_CR_DONE|\
			 DP_LANE_CHANNEL_EQ_DONE|\
			 DP_LANE_SYMBOL_LOCKED)
static Bool
intel_channel_eq_ok(struct i830_dp_priv *dev_priv)
{
	uint8_t lane_align;
	uint8_t lane_status;
	int lane;

	lane_align = i830_dp_link_status(dev_priv->link_status,
					  DP_LANE_ALIGN_STATUS_UPDATED);
	if ((lane_align & DP_INTERLANE_ALIGN_DONE) == 0)
		return FALSE;
	for (lane = 0; lane < dev_priv->lane_count; lane++) {
		lane_status = intel_get_lane_status(dev_priv->link_status, lane);
		if ((lane_status & CHANNEL_EQ_BITS) != CHANNEL_EQ_BITS)
			return FALSE;
	}
	return TRUE;
}

static Bool
i830_dp_set_link_train(xf86OutputPtr output,
			uint32_t dp_reg_value,
			uint8_t dp_train_pat)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	ScrnInfoPtr scrn = output->scrn;
	intel_screen_private *intel = intel_get_screen_private(scrn);
	int ret;

	I915_WRITE(dev_priv->output_reg, dp_reg_value);
	POSTING_READ(dev_priv->output_reg);

	i830_dp_aux_native_write_1(output,
				    DP_TRAINING_PATTERN_SET,
				    dp_train_pat);

	ret = i830_dp_aux_native_write(output,
					DP_TRAINING_LANE0_SET,
					dev_priv->train_set, 4);
	if (ret != 4)
		return FALSE;

	return TRUE;
}

/* Enable corresponding port and start training pattern 1 */
static void
i830_dp_start_link_train(xf86OutputPtr output)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	ScrnInfoPtr scrn = output->scrn;
	intel_screen_private *intel = intel_get_screen_private(scrn);
	I830CrtcPrivatePtr intel_crtc = output->crtc->driver_private;
	int i;
	uint8_t voltage;
	Bool clock_recovery = FALSE;
	int tries;
	uint32_t reg;
	uint32_t DP = dev_priv->DP;

	/* Enable output, wait for it to become active */
	I915_WRITE(dev_priv->output_reg, dev_priv->DP);
	POSTING_READ(dev_priv->output_reg);
	i830WaitForVblank(scrn);

	/* Write the link configuration data */
	i830_dp_aux_native_write(output, DP_LINK_BW_SET,
				  dev_priv->link_configuration,
				  DP_LINK_CONFIGURATION_SIZE);

	DP |= DP_PORT_EN;
	if (HAS_PCH_CPT(intel) && !is_edp(output))
		DP &= ~DP_LINK_TRAIN_MASK_CPT;
	else
		DP &= ~DP_LINK_TRAIN_MASK;
	memset(dev_priv->train_set, 0, 4);
	voltage = 0xff;
	tries = 0;
	clock_recovery = FALSE;
	for (;;) {
		/* Use dev_priv->train_set[0] to set the voltage and pre emphasis values */
		uint32_t    signal_levels;
		if (IS_GEN6(intel) && is_edp(output)) {
			signal_levels = intel_gen6_edp_signal_levels(dev_priv->train_set[0]);
			DP = (DP & ~EDP_LINK_TRAIN_VOL_EMP_MASK_SNB) | signal_levels;
		} else {
			signal_levels = i830_dp_signal_levels(dev_priv->train_set[0], dev_priv->lane_count);
			DP = (DP & ~(DP_VOLTAGE_MASK|DP_PRE_EMPHASIS_MASK)) | signal_levels;
		}

		if (HAS_PCH_CPT(intel) && !is_edp(output))
			reg = DP | DP_LINK_TRAIN_PAT_1_CPT;
		else
			reg = DP | DP_LINK_TRAIN_PAT_1;

		if (!i830_dp_set_link_train(output, reg,
					     DP_TRAINING_PATTERN_1))
			break;
		/* Set training pattern 1 */

		usleep(100);
		if (!i830_dp_get_link_status(output))
			break;

		if (intel_clock_recovery_ok(dev_priv->link_status, dev_priv->lane_count)) {
			clock_recovery = TRUE;
			break;
		}

		/* Check to see if we've tried the max voltage */
		for (i = 0; i < dev_priv->lane_count; i++)
			if ((dev_priv->train_set[i] & DP_TRAIN_MAX_SWING_REACHED) == 0)
				break;
		if (i == dev_priv->lane_count)
			break;

		/* Check to see if we've tried the same voltage 5 times */
		if ((dev_priv->train_set[0] & DP_TRAIN_VOLTAGE_SWING_MASK) == voltage) {
			++tries;
			if (tries == 5)
				break;
		} else
			tries = 0;
		voltage = dev_priv->train_set[0] & DP_TRAIN_VOLTAGE_SWING_MASK;

		/* Compute new dev_priv->train_set as requested by target */
		intel_get_adjust_train(dev_priv);
	}

	dev_priv->DP = DP;
}

static void
i830_dp_complete_link_train(xf86OutputPtr output)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	ScrnInfoPtr scrn = output->scrn;
	intel_screen_private *intel = intel_get_screen_private(scrn);
	Bool channel_eq = FALSE;
	int tries;
	uint32_t reg;
	uint32_t DP = dev_priv->DP;

	/* channel equalization */
	tries = 0;
	channel_eq = FALSE;
	for (;;) {
		/* Use dev_priv->train_set[0] to set the voltage and pre emphasis values */
		uint32_t    signal_levels;

		if (IS_GEN6(intel) && is_edp(output)) {
			signal_levels = intel_gen6_edp_signal_levels(dev_priv->train_set[0]);
			DP = (DP & ~EDP_LINK_TRAIN_VOL_EMP_MASK_SNB) | signal_levels;
		} else {
			signal_levels = i830_dp_signal_levels(dev_priv->train_set[0], dev_priv->lane_count);
			DP = (DP & ~(DP_VOLTAGE_MASK|DP_PRE_EMPHASIS_MASK)) | signal_levels;
		}

		if (HAS_PCH_CPT(intel) && !is_edp(output))
			reg = DP | DP_LINK_TRAIN_PAT_2_CPT;
		else
			reg = DP | DP_LINK_TRAIN_PAT_2;

		/* channel eq pattern */
		if (!i830_dp_set_link_train(output, reg,
					     DP_TRAINING_PATTERN_2))
			break;

		usleep(400);
		if (!i830_dp_get_link_status(output))
			break;

		if (intel_channel_eq_ok(dev_priv)) {
			channel_eq = TRUE;
			break;
		}

		/* Try 5 times */
		if (tries > 5)
			break;

		/* Compute new dev_priv->train_set as requested by target */
		intel_get_adjust_train(dev_priv);
		++tries;
	}

	if (HAS_PCH_CPT(intel) && !is_edp(output))
		reg = DP | DP_LINK_TRAIN_OFF_CPT;
	else
		reg = DP | DP_LINK_TRAIN_OFF;

	I915_WRITE(dev_priv->output_reg, reg);
	POSTING_READ(dev_priv->output_reg);
	i830_dp_aux_native_write_1(output,
				    DP_TRAINING_PATTERN_SET, DP_TRAINING_PATTERN_DISABLE);
}

static void
i830_dp_link_down(xf86OutputPtr output)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	ScrnInfoPtr scrn = output->scrn;
	intel_screen_private *intel = intel_get_screen_private(scrn);
	uint32_t DP = dev_priv->DP;

	if ((I915_READ(dev_priv->output_reg) & DP_PORT_EN) == 0)
		return;

	DRM_DEBUG_KMS("\n");

	if (is_edp(output)) {
		DP &= ~DP_PLL_ENABLE;
		I915_WRITE(dev_priv->output_reg, DP);
		POSTING_READ(dev_priv->output_reg);
		usleep(100);
	}

	if (HAS_PCH_CPT(intel) && !is_edp(output)) {
		DP &= ~DP_LINK_TRAIN_MASK_CPT;
		I915_WRITE(dev_priv->output_reg, DP | DP_LINK_TRAIN_PAT_IDLE_CPT);
	} else {
		DP &= ~DP_LINK_TRAIN_MASK;
		I915_WRITE(dev_priv->output_reg, DP | DP_LINK_TRAIN_PAT_IDLE);
	}
	POSTING_READ(dev_priv->output_reg);

	msleep(17);

	if (is_edp(output))
		DP |= DP_LINK_TRAIN_OFF;

	if (!HAS_PCH_CPT(intel) &&
	    I915_READ(dev_priv->output_reg) & DP_PIPEB_SELECT) {
		I830CrtcPrivatePtr intel_crtc = output->crtc->driver_private;
		/* Hardware workaround: leaving our transcoder select
		 * set to transcoder B while it's off will prevent the
		 * corresponding HDMI output on transcoder A.
		 *
		 * Combine this with another hardware workaround:
		 * transcoder select bit can only be cleared while the
		 * port is enabled.
		 */
		DP &= ~DP_PIPEB_SELECT;
		I915_WRITE(dev_priv->output_reg, DP);

		/* Changes to enable or select take place the vblank
		 * after being written.
		 */
		i830WaitForVblank(scrn);
	}

	I915_WRITE(dev_priv->output_reg, DP & ~DP_PORT_EN);
	POSTING_READ(dev_priv->output_reg);
}

/*
 * According to DP spec
 * 5.1.2:
 *  1. Read DPCD
 *  2. Configure link according to Receiver Capabilities
 *  3. Use Link Training from 2.5.3.3 and 3.5.1.3
 *  4. Check link status on receipt of hot-plug interrupt
 */

static void
i830_dp_check_link_status(xf86OutputPtr output)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);

	if (!output->crtc)
		return;

	if (!i830_dp_get_link_status(output)) {
		i830_dp_link_down(output);
		return;
	}

	if (!intel_channel_eq_ok(dev_priv)) {
		i830_dp_start_link_train(output);
		i830_dp_complete_link_train(output);
	}
}

static xf86OutputStatus
ironlake_dp_detect(xf86OutputPtr output)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	xf86OutputStatus status;

	/* Can't disconnect eDP */
	if (is_edp(output))
		return XF86OutputStatusConnected;

	status = XF86OutputStatusDisconnected;
	if (i830_dp_aux_native_read(output,
				     0x000, dev_priv->dpcd,
				     sizeof (dev_priv->dpcd))
	    == sizeof(dev_priv->dpcd)) {
		if (dev_priv->dpcd[0] != 0)
			status = XF86OutputStatusConnected;
	}
	DRM_DEBUG_KMS("DPCD: %hx%hx%hx%hx\n", dev_priv->dpcd[0],
		      dev_priv->dpcd[1], dev_priv->dpcd[2], dev_priv->dpcd[3]);
	return status;
}

static xf86OutputStatus
g4x_dp_detect(xf86OutputPtr output)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	ScrnInfoPtr scrn = output->scrn;
	intel_screen_private *intel = intel_get_screen_private(scrn);
	xf86OutputStatus status;
	uint32_t temp, bit;

	switch (dev_priv->output_reg) {
	case DP_B:
		bit = DPB_HOTPLUG_INT_STATUS;
		break;
	case DP_C:
		bit = DPC_HOTPLUG_INT_STATUS;
		break;
	case DP_D:
		bit = DPD_HOTPLUG_INT_STATUS;
		break;
	default:
		return XF86OutputStatusUnknown;
	}

	temp = I915_READ(PORT_HOTPLUG_STAT);

	if ((temp & bit) == 0)
		return XF86OutputStatusDisconnected;

	status = XF86OutputStatusDisconnected;
	if (i830_dp_aux_native_read(output, 0x000, dev_priv->dpcd,
				     sizeof (dev_priv->dpcd)) == sizeof (dev_priv->dpcd))
	{
		if (dev_priv->dpcd[0] != 0)
			status = XF86OutputStatusConnected;
	}

	return status;
}

/**
 * Uses CRT_HOTPLUG_EN and CRT_HOTPLUG_STAT to detect DP connection.
 *
 * \return true if DP port is connected.
 * \return false if DP port is disconnected.
 */
static xf86OutputStatus
i830_dp_detect(xf86OutputPtr output)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	ScrnInfoPtr scrn = output->scrn;
	intel_screen_private *intel = intel_get_screen_private(scrn);
	xf86OutputStatus status;
	struct edid *edid = NULL;

	if (HAS_PCH_SPLIT(intel))
		status = ironlake_dp_detect(output);
	else
		status = g4x_dp_detect(output);
	if (status != XF86OutputStatusConnected)
		return status;

	return XF86OutputStatusConnected;
}

static DisplayModePtr
i830_dp_get_modes(xf86OutputPtr output)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	ScrnInfoPtr scrn = output->scrn;
	intel_screen_private *intel = intel_get_screen_private(scrn);
	DisplayModePtr modes;

	modes = i830_ddc_get_modes(output);
	if (modes != NULL) {
		if (is_edp(output) && !intel->panel_fixed_mode) {
			DisplayModePtr newmode;
			for (newmode = modes; newmode != NULL; newmode = newmode->next) {
				if (newmode->type & M_T_PREFERRED) {
					intel->panel_fixed_mode =
						xf86DuplicateMode(newmode);
					break;
				}
			}
		}

		return modes;
	}

	/* if eDP has no EDID, try to use fixed panel mode from VBT */
	if (is_edp(output)) {
		if (intel->panel_fixed_mode != NULL) {
			modes = xf86DuplicateMode(intel->panel_fixed_mode);
			return modes;
		}
	}
	return NULL;
}

static void
i830_dp_destroy (xf86OutputPtr output)
{
	ScrnInfoPtr scrn = output->scrn;
	intel_screen_private *intel = intel_get_screen_private(scrn);
	I830OutputPrivatePtr intel_output = output->driver_private;

	if (intel_output) {
		if (intel_output->pDDCBus)
			xf86DestroyI2CBusRec(intel_output->pDDCBus, FALSE, FALSE);

		if (intel->panel_fixed_mode)
			xf86DeleteMode(&intel->panel_fixed_mode,
			    intel->panel_fixed_mode);

		free (intel_output);
	}
}

static void i830_dp_encoder_destroy(xf86OutputPtr output)
{
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);

	i2c_del_adapter(&dev_priv->adapter);
	drm_encoder_cleanup(output);
	kfree(dev_priv);
}

static const struct drm_encoder_helper_funcs i830_dp_helper_funcs = {
	.dpms = i830_dp_dpms,
	.mode_fixup = i830_dp_mode_fixup,
	.prepare = i830_dp_prepare,
	.mode_set = i830_dp_mode_set,
	.commit = i830_dp_commit,
};

static const struct drm_connector_funcs i830_dp_connector_funcs = {
	.dpms = drm_helper_connector_dpms,
	.detect = i830_dp_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.set_property = i830_dp_set_property,
	.destroy = i830_dp_destroy,
};

static const struct drm_connector_helper_funcs i830_dp_connector_helper_funcs = {
	.get_modes = i830_dp_get_modes,
	.mode_valid = i830_dp_mode_valid,
	.best_encoder = intel_best_encoder,
};

static const struct drm_encoder_funcs i830_dp_enc_funcs = {
	.destroy = i830_dp_encoder_destroy,
};

/* Return which DP Port should be selected for Transcoder DP control */
int
i830_trans_dp_port_sel (xf86CrtcPtr crtc)
{
	ScrnInfoPtr scrn = crtc->scrn;
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
	xf86OutputPtr output;
	int i;

	for (i = 0; i < xf86_config->num_output; i++) {
		xf86OutputPtr output = xf86_config->output[i];
		struct i830_dp_priv *dev_priv;

		if (output->crtc != crtc)
			continue;

		dev_priv = output_to_i830_dp(output);
		if (((I830OutputPrivatePtr)output->driver_private)->type == I830_OUTPUT_DISPLAYPORT)
			return dev_priv->output_reg;
	}

	return -1;
}

/* check the VBT to see whether the eDP is on DP-D port */
Bool i830_dpd_is_edp(ScrnInfoPtr scrn)
{
	intel_screen_private *intel = intel_get_screen_private(scrn);
	struct child_device_config *p_child;
	int i;

	if (!intel->child_dev_num)
		return FALSE;

	for (i = 0; i < intel->child_dev_num; i++) {
		p_child = intel->child_dev + i;

		if (p_child->dvo_port == PORT_IDPD &&
		    p_child->device_type == DEVICE_TYPE_eDP)
			return TRUE;
	}
	return FALSE;
}

void
i830_dp_init(ScrnInfoPtr scrn, int output_reg)
{
	intel_screen_private *intel = intel_get_screen_private(scrn);
	struct drm_connector *connector;
	struct i830_dp_priv *dev_priv;
	I830OutputPrivatePtr intel_output;
	struct intel_connector *intel_connector;
	const char *name = NULL;
	int type;

	dev_priv = kzalloc(sizeof(struct i830_dp_priv), GFP_KERNEL);
	if (!dev_priv)
		return;

	intel_connector = kzalloc(sizeof(struct intel_connector), GFP_KERNEL);
	if (!intel_connector) {
		kfree(dev_priv);
		return;
	}
	intel_output = &dev_priv->base;

	if (HAS_PCH_SPLIT(intel) && output_reg == PCH_DP_D)
		if (i830_dpd_is_edp(scrn))
			dev_priv->is_pch_edp = TRUE;

	if (output_reg == DP_A || is_pch_edp(dev_priv)) {
		type = DRM_MODE_CONNECTOR_eDP;
		intel_output->type = I830_OUTPUT_EDP;
	} else {
		type = DRM_MODE_CONNECTOR_DisplayPort;
		intel_output->type = I830_OUTPUT_DISPLAYPORT;
	}

	connector = &intel_connector->base;
	drm_connector_init(scrn, connector, &i830_dp_connector_funcs, type);
	drm_connector_helper_add(connector, &i830_dp_connector_helper_funcs);

	connector->polled = DRM_CONNECTOR_POLL_HPD;

	if (output_reg == DP_B || output_reg == PCH_DP_B)
		intel_output->clone_mask = (1 << INTEL_DP_B_CLONE_BIT);
	else if (output_reg == DP_C || output_reg == PCH_DP_C)
		intel_output->clone_mask = (1 << INTEL_DP_C_CLONE_BIT);
	else if (output_reg == DP_D || output_reg == PCH_DP_D)
		intel_output->clone_mask = (1 << INTEL_DP_D_CLONE_BIT);

	if (is_edp(output))
		intel_output->clone_mask = (1 << INTEL_EDP_CLONE_BIT);

	intel_output->crtc_mask = (1 << 0) | (1 << 1);
	connector->interlace_allowed = TRUE;
	connector->doublescan_allowed = 0;

	dev_priv->output_reg = output_reg;
	dev_priv->dpms_mode = DRM_MODE_DPMS_ON;

	drm_encoder_init(scrn, &intel_output->base, &i830_dp_enc_funcs,
			 DRM_MODE_ENCODER_TMDS);
	drm_encoder_helper_add(&intel_output->base, &i830_dp_helper_funcs);

	intel_connector_attach_encoder(intel_connector, intel_output);
	drm_sysfs_connector_add(connector);

	/* Set up the DDC bus. */
	switch (output_reg) {
		case DP_A:
			name = "DPDDC-A";
			break;
		case DP_B:
		case PCH_DP_B:
			name = "DPDDC-B";
			break;
		case DP_C:
		case PCH_DP_C:
			name = "DPDDC-C";
			break;
		case DP_D:
		case PCH_DP_D:
			name = "DPDDC-D";
			break;
	}

	intel_output->pDDCBus = i830_dp_i2c_init(output, dev_priv, name);

	/* Cache some DPCD data in the eDP case */
	if (is_edp(output)) {
		int ret;
		Bool was_on;

		was_on = ironlake_edp_panel_on(scrn);
		ret = i830_dp_aux_native_read(output, DP_DPCD_REV,
					       dev_priv->dpcd,
					       sizeof(dev_priv->dpcd));
		if (ret == sizeof(dev_priv->dpcd)) {
			if (dev_priv->dpcd[0] >= 0x11)
				intel->no_aux_handshake = dev_priv->dpcd[3] &
					DP_NO_AUX_HANDSHAKE_LINK_TRAINING;
		} else {
			DRM_ERROR("failed to retrieve link info\n");
		}
		if (!was_on)
			ironlake_edp_panel_off(scrn);
	}

	if (is_edp(output)) {
		/* initialize panel mode from VBT if available for eDP */
		if (intel->lfp_lvds_vbt_mode) {
			intel->panel_fixed_mode =
				xf86DuplicateMode(intel->lfp_lvds_vbt_mode);
			if (intel->panel_fixed_mode) {
				intel->panel_fixed_mode->type |=
					M_T_PREFERRED;
			}
		}
	}

	i830_dp_add_properties(dev_priv, connector);

	/* For G4X desktop chip, PEG_BAND_GAP_DATA 3:0 must first be written
	 * 0xd.  Failure to do so will result in spurious interrupts being
	 * generated on the port when a cable is not attached.
	 */
	if (IS_G4X(intel) && !IS_GM45(intel)) {
		uint32_t temp = I915_READ(PEG_BAND_GAP_DATA);
		I915_WRITE(PEG_BAND_GAP_DATA, (temp & ~0xf) | 0xd);
	}
}

/*
 * I2C over AUX CH
 */

static Bool
i830_dp_i2c_get_byte(I2CDevPtr d, I2CByte *byte, Bool last)
{
	xf86OutputPtr output = d->pI2CBus->DriverPrivate.ptr;
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);

	if (!dev_priv->i2c_state.running)
		return FALSE;

	dev_priv->i2c_state.reading = TRUE;

	return i830_dp_i2c_aux_ch(output, MODE_I2C_READ, 0, byte);
}

static Bool
i830_dp_i2c_put_byte(I2CDevPtr d, I2CByte byte)
{
	xf86OutputPtr output = d->pI2CBus->DriverPrivate.ptr;
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);

	if (!dev_priv->i2c_state.running)
		return FALSE;

	dev_priv->i2c_state.reading = FALSE;

	return i830_dp_i2c_aux_ch(output, MODE_I2C_WRITE, byte, NULL);
}

static Bool
i830_dp_i2c_start(I2CBusPtr b, int timeout)
{
	/* XXX We probably should do something here, but what? */
	return TRUE;
}

static void
i830_dp_i2c_stop(I2CDevPtr d)
{
	xf86OutputPtr output = d->pI2CBus->DriverPrivate.ptr;
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	int mode = MODE_I2C_STOP;

	if (dev_priv->i2c_state.reading)
		mode |= MODE_I2C_READ;
	else
		mode |= MODE_I2C_WRITE;

	if (dev_priv->i2c_state.running) {
		(void)i830_dp_i2c_aux_ch(output, mode, 0, NULL);
		dev_priv->i2c_state.running = FALSE;
	}
}

static Bool
i830_dp_i2c_address(I2CDevPtr d, I2CSlaveAddr address)
{
	xf86OutputPtr output = d->pI2CBus->DriverPrivate.ptr;
	struct i830_dp_priv *dev_priv = output_to_i830_dp(output);
	int mode = MODE_I2C_START;

	dev_priv->i2c_state.reading = (address & 0x1) ? TRUE : FALSE;

	if (dev_priv->i2c_state.reading)
		mode |= MODE_I2C_READ;
	else
		mode |= MODE_I2C_WRITE;

	dev_priv->i2c_state.address = address;
	dev_priv->i2c_state.running = TRUE;

	return i830_dp_i2c_aux_ch(output, mode, 0, NULL);
}

/*
 * Adaptation of I830I2CInit().
 */
static I2CBusPtr
i830_dp_i2c_init(xf86OutputPtr output, struct i830_dp_priv *dev_priv, const char *name)
{
	I2CBusPtr pI2CBus;
	int mode;

	DRM_DEBUG_KMS("%s %s\n", __FUNCTION__, name);

	pI2CBus = xf86CreateI2CBusRec();

	if (!pI2CBus)
		return NULL;

	pI2CBus->BusName = __UNCONST(name);
	pI2CBus->scrnIndex = output->scrn->scrnIndex;
	pI2CBus->I2CGetByte = i830_dp_i2c_get_byte;
	pI2CBus->I2CPutByte = i830_dp_i2c_put_byte;
	pI2CBus->I2CStart = i830_dp_i2c_start;
	pI2CBus->I2CStop = i830_dp_i2c_stop;
	pI2CBus->I2CAddress = i830_dp_i2c_address;
	pI2CBus->DriverPrivate.ptr = output;

	pI2CBus->ByteTimeout = 2200;	/* VESA DDC spec 3 p. 43 (+10 %) */
	pI2CBus->StartTimeout = 550;
	pI2CBus->BitTimeout = 40;
	pI2CBus->AcknTimeout = 40;
	pI2CBus->RiseFallTime = 20;

	if (!xf86I2CBusInit(pI2CBus))
		return NULL;

	dev_priv->i2c_state.running = FALSE;
	dev_priv->i2c_state.address = 0;
	dev_priv->i2c_state.reading = FALSE;

	/* Simulate Linux i2c_dp_aux_reset_bus. */
	mode = MODE_I2C_START | MODE_I2C_WRITE;
	(void)i830_dp_i2c_aux_ch(output, mode, 0, NULL);
	mode = MODE_I2C_STOP | MODE_I2C_WRITE;
	(void)i830_dp_i2c_aux_ch(output, mode, 0, NULL);

	return pI2CBus;
}

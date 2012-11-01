s/\([^a-zA-Z0-9_]\)bool\([^a-zA-Z0-9_]\)/\1Bool\2/g
s/^bool\([^a-zA-Z0-9_]\)/Bool\1/g
s/\([^a-zA-Z0-9_]\)bool$/\1Bool/g
s/ true;/ TRUE;/g
s/ false;/ FALSE;/g

s/\([^a-zA-Z0-9_]\)u32\([^a-zA-Z0-9_]\)/\1uint32_t\2/g

s/\([^a-zA-Z0-9_]\)udelay(/\1usleep(/g

s/struct intel_dp/struct i830_dp_priv/g
s/\([^a-zA-Z0-9_]\)intel_dp_/\1i830_dp_/g
s/^intel_dp_/i830_dp_/g
s/\([^a-zA-Z0-9_]\)intel_dpd_/\1i830_dpd_/g
s/^intel_dpd_/i830_dpd_/g

s/struct drm_i915_private/intel_screen_private/g
s/\([^a-zA-Z0-9_]\)dev_priv\([^a-zA-Z0-9_]\)/\1intel\2/g
s/\([^a-zA-Z0-9_]\)intel_dp\([^a-zA-Z0-9_]\)/\1dev_priv\2/g

s/\(HAS_[A-Z0-9_]*\)(dev)/\1(intel)/g
s/\(IS_[A-Z0-9_]*\)(dev)/\1(intel)/g

s/struct intel_encoder \*/I830OutputPrivatePtr /g
s/\([^a-zA-Z0-9_]\)intel_encoder\([^a-zA-Z0-9_]\)/\1intel_output\2/g
s/enc_to_intel_dp(/output_to_i830_dp(/g

s/struct drm_encoder \*/xf86OutputPtr /g
s/\([^a-zA-Z0-9_]\)encoder\([^a-zA-Z0-9_]\)/\1output\2/g

s/struct drm_device \*dev/ScrnInfoPtr scrn/g
s/\([^a-zA-Z0-9_]\)dev->dev_private/\1intel_get_screen_private(scrn)/g
s/&dev->mode_config/XF86_CRTC_CONFIG_PTR(scrn)/g
s/\([^a-zA-Z0-9_]\)output->dev;/\1output->scrn;/g
s/\([^a-zA-Z0-9_]\)crtc->dev;/\1crtc->scrn;/g
s/\([^a-zA-Z0-9_]intel_[a-z_]*\)(dev\([,)]\)/\1(scrn\2/g
s/\([^a-zA-Z0-9_]ironlake_edp_[a-z_]*\)(dev)/\1(scrn)/g
s/\([^a-zA-Z0-9_]drm_[a-z_]*\)(dev,/\1(scrn,/g
s/\([^a-zA-Z0-9_]i830_dpd_is_edp\)(dev)/\1(scrn)/g

s/struct drm_crtc \*/xf86CrtcPtr /g
s/struct intel_crtc \*/I830CrtcPrivatePtr /g
s/\([^a-zA-Z0-9_]\)to_intel_crtc(\([^)]*\))/\1\2->driver_private/g

s/struct drm_display_mode \*/DisplayModePtr /g
s/mode->clock/mode->Clock/g
s/mode->flags/mode->Flags/g
s/mode->hdisplay/mode->HDisplay/g
s/mode->hsync_start/mode->HSyncStart/g
s/mode->hsync_end/mode->HSyncEnd/g
s/mode->htotal/mode->HTotal/g
s/mode->vdisplay/mode->VDisplay/g
s/mode->vsync_start/mode->VSyncStart/g
s/mode->vsync_end/mode->VSyncEnd/g
s/mode->vtotal/mode->VTotal/g

s/enum drm_connector_status/xf86OutputStatus/g
s/connector_status_connected/XF86OutputStatusConnected/g
s/connector_status_disconnected/XF86OutputStatusDisconnected/g
s/connector_status_unknown/XF86OutputStatusUnknown/g

s/struct drm_mode_config \*/xf86CrtcConfigPtr /g
s/\([^a-zA-Z0-9_]\)mode_config\([^a-zA-Z0-9_]\)/\1xf86_config\2/g

s/drm_mode_duplicate(scrn,/xf86DuplicateModes(scrn,/g
s/DRM_MODE_TYPE_PREFERRED/M_T_PREFERRED/g
s/drm_mode_set_crtcinfo(/xf86SetModeCrtc(/g
s/CRTC_INTERLACE_HALVE_V/INTERLACE_HALVE_V/g

s/\([^a-zA-Z0-9_]\)INTEL_OUTPUT_/\1I830_OUTPUT_/g

s/\([^a-zA-Z0-9_]\)intel_\(encoder_is_pch_edp\)/\1i830_\2/g
s/^intel_\(encoder_is_pch_edp\)/i830_\1/g
s/\([^a-zA-Z0-9_]\)intel_\(edp_link_config\)/\1i830_\2/g
s/^intel_\(edp_link_config\)/i830_\1/g
s/\([^a-zA-Z0-9_]\)intel_\(trans_dp_port_sel\)/\1i830_\2/g
s/^intel_\(trans_dp_port_sel\)/i830_\1/g

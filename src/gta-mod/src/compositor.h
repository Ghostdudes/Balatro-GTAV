// compositor.h
// DirectX 11 ReShade add-on compositor for Balatro <-> GTA V passthrough.
#pragma once

#include "../../protocol/balatro_protocol.h"
#include <windows.h>

namespace compositor
{
	bool try_register(void *module);
	void unregister(void *module);
	bool is_registered();

	void set_active(bool active);
	bool is_active();

	void set_phone_rect(float x1, float y1, float x2, float y2);
	void get_phone_rect(float &x1, float &y1, float &x2, float &y2);

	void set_mode(int mode); // 0 = Phone, 1 = Fullscreen, 2 = Casino
	void set_cam_mode(int mode);

	void set_mouse_pos(float x, float y);
	void get_mouse_pos(float &x, float &y);

	void set_phone_render_active(bool active);
	bool is_phone_render_active();

	void set_rotation(int rot);
	int get_rotation();

	void set_gamma(float gamma);
	float get_gamma();

	bool get_balatro_state(balatro_gta::proto::BalatroState &outState);
	void send_input_event(uint16_t type, uint16_t code, float x, float y, uint32_t extra);

	void update_heartbeat();
}

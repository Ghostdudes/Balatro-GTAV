// compositor.cpp
// DirectX 11 ReShade add-on compositor for Balatro <-> GTA V passthrough.
//
// Phone mode: GTA renders the phone screen (scaleform UI) into a small render target (512x256 BGRA).
// We downscale Balatro's latest frame on the CPU into a texture of exactly that size and format and
// copy_resource() it over GTA's phone texture right after GTA finishes drawing into it. Both the 3D phone
// in the player's hand and the phone shown on screen sample that same texture, so both show Balatro.
//
// Fullscreen mode: the BalatroPhone.fx effect draws the full-resolution frame over the back buffer.

#include "compositor.h"
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <reshade.hpp>

using namespace reshade::api;
using namespace balatro_gta::proto;

namespace
{
	std::atomic<bool> g_registered{false};
	std::atomic<bool> g_active{false};
	std::atomic<int>  g_mode{0}; // 0 = In-Game Phone, 1 = Fullscreen, 2 = Casino
	std::atomic<bool> g_phoneRenderActive{false};
	std::atomic<int>  g_rotation{1}; // 0 = 0 deg, 1 = 90 deg CW, 2 = 180 deg, 3 = 270 deg CW

	float g_phoneX1 = 0.7000f;
	float g_phoneY1 = 0.5470f;
	float g_phoneX2 = 0.9160f;
	float g_phoneY2 = 0.7760f;
	std::atomic<int>   g_camMode{0};
	std::atomic<float> g_mouseX{0.5f};
	std::atomic<float> g_mouseY{0.5f};
	std::mutex g_rectLock;

	std::atomic<float> g_gamma{1.25f};
	uint8_t g_gammaLut[256] = {};
	float g_lutBuiltGamma = 0.0f;

	void ensure_gamma_lut(float gamma)
	{
		if (std::abs(g_lutBuiltGamma - gamma) < 0.001f)
			return;
		g_lutBuiltGamma = gamma;
		for (int i = 0; i < 256; ++i)
		{
			float norm = i / 255.0f;
			float corrected = std::pow(norm, gamma);
			g_gammaLut[i] = static_cast<uint8_t>(std::clamp(std::round(corrected * 255.0f), 0.0f, 255.0f));
		}
	}

	inline uint32_t apply_gamma(uint32_t c, bool bgra)
	{
		uint8_t r = g_gammaLut[c & 0xFFu];
		uint8_t g = g_gammaLut[(c >> 8) & 0xFFu];
		uint8_t b = g_gammaLut[(c >> 16) & 0xFFu];
		uint32_t a = c & 0xFF000000u;
		if (bgra)
			return a | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
		else
			return a | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
	}

	HANDLE g_mapping = nullptr;
	uint8_t *g_view = nullptr;
	uint32_t g_frontSlot = 0;
	DWORD g_nextOpenAttempt = 0;

	effect_runtime *g_runtime = nullptr;

	void logf(const char *fmt, ...)
	{
		if (!g_registered)
			return;
		char buf[256];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		reshade::log::message(reshade::log::level::info, buf);
	}

	// ---- Full-resolution layer (fullscreen mode, sampled by BalatroPhone.fx) ----
	struct Layer
	{
		resource tex = {0};
		resource_view srv = {0};
	};
	Layer g_balatro;
	bool g_balatroInShaderResource = false;
	uint32_t g_width = 0, g_height = 0;
	uint64_t g_lastFrameId = 0;
	bool g_hasFrame = false;

	// ---- Phone-sized texture cache (phone mode, copied into GTA's phone render target) ----
	struct PhoneBuffer
	{
		resource tex = {0};
		uint32_t w = 0, h = 0;
		format fmt = format::unknown;
		uint64_t frameId = ~0ull;
		bool ready = false;
		bool inCopySource = false;
	};
	std::vector<PhoneBuffer> g_phoneTexCache;
	std::mutex g_phoneCacheLock;

	// Background worker thread for asynchronous downscaling and sharpening
	std::mutex g_phonePixelsLock;
	std::vector<uint32_t> g_phoneReadyPixels;
	std::atomic<uint64_t> g_workerReadyFrameId{0};
	std::atomic<bool> g_workerRunning{false};
	std::thread g_workerThread;

	// GTA's phone render target, captured while bound; copied over when GTA moves on to another target.
	resource g_actualPhoneRes = {0};
	resource_view g_actualPhoneRtv = {0};
	std::atomic<command_list *> g_phoneCmdList{nullptr};
	uint64_t g_copyCount = 0;
	std::atomic<bool> g_phoneCopiedThisFrame{false};

	template <typename T>
	T read_val(const uint8_t *p)
	{
		T v;
		std::memcpy(&v, p, sizeof(T));
		return v;
	}

	std::mutex g_mappingLock;
	bool open_mapping()
	{
		if (g_view != nullptr)
			return true;
		std::lock_guard<std::mutex> lk(g_mappingLock);
		if (g_view != nullptr)
			return true;
		if (GetTickCount() < g_nextOpenAttempt)
			return false;
		g_nextOpenAttempt = GetTickCount() + 500;

		g_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kMappingName);
		if (g_mapping == nullptr)
			return false;

		g_view = static_cast<uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(kMappingBytes)));
		if (g_view == nullptr)
		{
			CloseHandle(g_mapping);
			g_mapping = nullptr;
			return false;
		}

		auto *pGtaPid = reinterpret_cast<uint32_t *>(g_view + kOffHeader + 8);
		*pGtaPid = GetCurrentProcessId();

		if (g_registered)
			reshade::log::message(reshade::log::level::info, "BalatroCompositor: connected to Balatro shared memory stream");
		return true;
	}

	std::mutex g_acquireLock;
	// Takes the newest published slot (if any) and returns the front slot's pixels (RGBA, top-down).
	bool acquire_front(const uint8_t *&px, uint32_t &w, uint32_t &h, uint64_t &frameId)
	{
		if (!open_mapping())
			return false;

		std::lock_guard<std::mutex> lk(g_acquireLock);
		auto *pCtl = reinterpret_cast<volatile LONG *>(g_view + kOffOverlayCtl);
		if (*pCtl & kOverlayDirty)
		{
			LONG old_ctl = InterlockedExchange(pCtl, static_cast<LONG>(g_frontSlot));
			g_frontSlot = static_cast<uint32_t>(old_ctl & 3);
		}
		if (g_frontSlot > 2)
			g_frontSlot = 0;

		const uint8_t *desc = g_view + kOffOverlaySlotHdr + (g_frontSlot * 0x40);
		w = read_val<uint32_t>(desc + 0);
		h = read_val<uint32_t>(desc + 4);
		frameId = read_val<uint64_t>(desc + 16);
		if (w == 0 || h == 0 || w > kMaxOverlayW || h > kMaxOverlayH)
			return false;

		px = g_view + kOffOverlayPixels + (kOverlaySlotBytes * g_frontSlot);
		return true;
	}

	void destroy_layer(device *dev)
	{
		if (g_balatro.srv.handle != 0)
			dev->destroy_resource_view(g_balatro.srv);
		if (g_balatro.tex.handle != 0)
			dev->destroy_resource(g_balatro.tex);
		g_balatro = Layer();
		g_balatroInShaderResource = false;
		g_width = g_height = 0;
		g_hasFrame = false;
	}

	void destroy_phone_tex(device *dev)
	{
		std::lock_guard<std::mutex> lk(g_phoneCacheLock);
		for (auto &buf : g_phoneTexCache)
		{
			if (buf.tex.handle != 0)
				dev->destroy_resource(buf.tex);
		}
		g_phoneTexCache.clear();
	}

	bool create_layer(device *dev, Layer &layer, uint32_t w, uint32_t h, format fmt)
	{
		if (!dev->create_resource(
				resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::copy_dest),
				nullptr, resource_usage::copy_dest, &layer.tex))
			return false;
		return dev->create_resource_view(layer.tex, resource_usage::shader_resource, resource_view_desc(fmt), &layer.srv);
	}

	void bind_textures(effect_runtime *runtime)
	{
		if (runtime != nullptr && g_balatro.srv.handle != 0)
			runtime->update_texture_bindings("BALATRO_TEX", g_balatro.srv, g_balatro.srv);
	}

	// Fullscreen & Large Phone mode: full-resolution upload for the ReShade effect.
	void upload_frame(effect_runtime *runtime, command_list *cmd_list)
	{
		if (runtime == nullptr)
			return;

		const uint8_t *px;
		uint32_t w, h;
		uint64_t frameId;
		if (!acquire_front(px, w, h, frameId))
			return;

		device *dev = runtime->get_device();
		if (dev == nullptr)
			return;

		if (w != g_width || h != g_height)
		{
			destroy_layer(dev);
			if (!create_layer(dev, g_balatro, w, h, format::r8g8b8a8_unorm))
			{
				destroy_layer(dev);
				return;
			}
			g_width = w;
			g_height = h;
			g_balatroInShaderResource = false;
			bind_textures(runtime);
		}

		if (!g_hasFrame || frameId != g_lastFrameId)
		{
			if (dev->get_api() == device_api::d3d12 && g_balatroInShaderResource && cmd_list != nullptr)
			{
				cmd_list->barrier(g_balatro.tex, resource_usage::shader_resource, resource_usage::copy_dest);
				g_balatroInShaderResource = false;
			}

			subresource_data data;
			data.data = const_cast<uint8_t *>(px);
			data.row_pitch = w * 4;
			data.slice_pitch = w * h * 4;
			dev->update_texture_region(data, g_balatro.tex, 0);

			g_lastFrameId = frameId;
			g_hasFrame = true;
		}

		if (dev->get_api() == device_api::d3d12 && !g_balatroInShaderResource && cmd_list != nullptr)
		{
			cmd_list->barrier(g_balatro.tex, resource_usage::copy_dest, resource_usage::shader_resource);
			g_balatroInShaderResource = true;
		}
	}

	bool phone_mode_on()
	{
		return g_active.load() && (g_mode.load() == 0 || g_mode.load() == 1) && g_phoneRenderActive.load();
	}

	// Processes Balatro frame on the asynchronous worker thread (zero render thread overhead)
	void process_phone_frame(const uint8_t *px, uint32_t sw, uint32_t sh, int rot, float gamma)
	{
		const uint32_t dw = 768, dh = 768;
		static std::vector<uint32_t> s_downscaled(size_t(dw) * dh);
		static std::vector<uint32_t> s_sharpened(size_t(dw) * dh);

		ensure_gamma_lut(gamma);

		struct Span { uint32_t s0, s1; };
		Span spansX[768];
		Span spansY[768];

		const auto *src32 = reinterpret_cast<const uint32_t *>(px);
		uint32_t *dst32 = s_downscaled.data();

		switch (rot)
		{
		case 1: // 90 deg CW
			for (uint32_t x = 0; x < dw; ++x)
			{
				uint32_t s0 = ((dw - 1 - x) * sh) / dw;
				uint32_t s1 = ((dw - x) * sh + dw - 1) / dw;
				if (s1 <= s0) s1 = s0 + 1;
				if (s1 > sh) s1 = sh;
				spansX[x] = { s0, s1 };
			}
			for (uint32_t y = 0; y < dh; ++y)
			{
				uint32_t s0 = (y * sw) / dh;
				uint32_t s1 = ((y + 1) * sw + dh - 1) / dh;
				if (s1 <= s0) s1 = s0 + 1;
				if (s1 > sw) s1 = sw;
				spansY[y] = { s0, s1 };
			}
			for (uint32_t y = 0; y < dh; ++y)
			{
				const uint32_t sx0 = spansY[y].s0;
				const uint32_t sx1 = spansY[y].s1;
				uint32_t *row = dst32 + size_t(y) * dw;
				for (uint32_t x = 0; x < dw; ++x)
				{
					const uint32_t sy0 = spansX[x].s0;
					const uint32_t sy1 = spansX[x].s1;
					uint32_t sumR = 0, sumG = 0, sumB = 0, count = 0;
					for (uint32_t sy = sy0; sy < sy1; ++sy)
					{
						const uint32_t *srcRow = src32 + size_t(sy) * sw;
						for (uint32_t sx = sx0; sx < sx1; ++sx)
						{
							uint32_t c = srcRow[sx];
							sumR += (c & 0xFFu);
							sumG += ((c >> 8) & 0xFFu);
							sumB += ((c >> 16) & 0xFFu);
							count++;
						}
					}
					uint8_t r = (count == 1) ? g_gammaLut[sumR] : (count == 2) ? g_gammaLut[sumR >> 1] : (count == 4) ? g_gammaLut[sumR >> 2] : g_gammaLut[sumR / count];
					uint8_t g = (count == 1) ? g_gammaLut[sumG] : (count == 2) ? g_gammaLut[sumG >> 1] : (count == 4) ? g_gammaLut[sumG >> 2] : g_gammaLut[sumG / count];
					uint8_t b = (count == 1) ? g_gammaLut[sumB] : (count == 2) ? g_gammaLut[sumB >> 1] : (count == 4) ? g_gammaLut[sumB >> 2] : g_gammaLut[sumB / count];
					row[x] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
				}
			}
			break;

		case 2: // 180 deg
			for (uint32_t x = 0; x < dw; ++x)
			{
				uint32_t s0 = ((dw - 1 - x) * sw) / dw;
				uint32_t s1 = ((dw - x) * sw + dw - 1) / dw;
				if (s1 <= s0) s1 = s0 + 1;
				if (s1 > sw) s1 = sw;
				spansX[x] = { s0, s1 };
			}
			for (uint32_t y = 0; y < dh; ++y)
			{
				uint32_t s0 = ((dh - 1 - y) * sh) / dh;
				uint32_t s1 = ((dh - y) * sh + dh - 1) / dh;
				if (s1 <= s0) s1 = s0 + 1;
				if (s1 > sh) s1 = sh;
				spansY[y] = { s0, s1 };
			}
			for (uint32_t y = 0; y < dh; ++y)
			{
				const uint32_t sx0 = spansY[y].s0;
				const uint32_t sx1 = spansY[y].s1;
				uint32_t *row = dst32 + size_t(y) * dw;
				for (uint32_t x = 0; x < dw; ++x)
				{
					const uint32_t sy0 = spansX[x].s0;
					const uint32_t sy1 = spansX[x].s1;
					uint32_t sumR = 0, sumG = 0, sumB = 0, count = 0;
					for (uint32_t sy = sy0; sy < sy1; ++sy)
					{
						const uint32_t *srcRow = src32 + size_t(sy) * sw;
						for (uint32_t sx = sx0; sx < sx1; ++sx)
						{
							uint32_t c = srcRow[sx];
							sumR += (c & 0xFFu);
							sumG += ((c >> 8) & 0xFFu);
							sumB += ((c >> 16) & 0xFFu);
							count++;
						}
					}
					uint8_t r = (count == 1) ? g_gammaLut[sumR] : (count == 2) ? g_gammaLut[sumR >> 1] : (count == 4) ? g_gammaLut[sumR >> 2] : g_gammaLut[sumR / count];
					uint8_t g = (count == 1) ? g_gammaLut[sumG] : (count == 2) ? g_gammaLut[sumG >> 1] : (count == 4) ? g_gammaLut[sumG >> 2] : g_gammaLut[sumG / count];
					uint8_t b = (count == 1) ? g_gammaLut[sumB] : (count == 2) ? g_gammaLut[sumB >> 1] : (count == 4) ? g_gammaLut[sumB >> 2] : g_gammaLut[sumB / count];
					row[x] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
				}
			}
			break;

		case 3: // 270 deg CW
			for (uint32_t x = 0; x < dw; ++x)
			{
				uint32_t s0 = (x * sh) / dw;
				uint32_t s1 = ((x + 1) * sh + dw - 1) / dw;
				if (s1 <= s0) s1 = s0 + 1;
				if (s1 > sh) s1 = sh;
				spansX[x] = { s0, s1 };
			}
			for (uint32_t y = 0; y < dh; ++y)
			{
				uint32_t s0 = ((dh - 1 - y) * sw) / dh;
				uint32_t s1 = ((dh - y) * sw + dh - 1) / dh;
				if (s1 <= s0) s1 = s0 + 1;
				if (s1 > sw) s1 = sw;
				spansY[y] = { s0, s1 };
			}
			for (uint32_t y = 0; y < dh; ++y)
			{
				const uint32_t sx0 = spansY[y].s0;
				const uint32_t sx1 = spansY[y].s1;
				uint32_t *row = dst32 + size_t(y) * dw;
				for (uint32_t x = 0; x < dw; ++x)
				{
					const uint32_t sy0 = spansX[x].s0;
					const uint32_t sy1 = spansX[x].s1;
					uint32_t sumR = 0, sumG = 0, sumB = 0, count = 0;
					for (uint32_t sy = sy0; sy < sy1; ++sy)
					{
						const uint32_t *srcRow = src32 + size_t(sy) * sw;
						for (uint32_t sx = sx0; sx < sx1; ++sx)
						{
							uint32_t c = srcRow[sx];
							sumR += (c & 0xFFu);
							sumG += ((c >> 8) & 0xFFu);
							sumB += ((c >> 16) & 0xFFu);
							count++;
						}
					}
					uint8_t r = (count == 1) ? g_gammaLut[sumR] : (count == 2) ? g_gammaLut[sumR >> 1] : (count == 4) ? g_gammaLut[sumR >> 2] : g_gammaLut[sumR / count];
					uint8_t g = (count == 1) ? g_gammaLut[sumG] : (count == 2) ? g_gammaLut[sumG >> 1] : (count == 4) ? g_gammaLut[sumG >> 2] : g_gammaLut[sumG / count];
					uint8_t b = (count == 1) ? g_gammaLut[sumB] : (count == 2) ? g_gammaLut[sumB >> 1] : (count == 4) ? g_gammaLut[sumB >> 2] : g_gammaLut[sumB / count];
					row[x] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
				}
			}
			break;

		default: // 0 deg
			for (uint32_t x = 0; x < dw; ++x)
			{
				uint32_t s0 = (x * sw) / dw;
				uint32_t s1 = ((x + 1) * sw + dw - 1) / dw;
				if (s1 <= s0) s1 = s0 + 1;
				if (s1 > sw) s1 = sw;
				spansX[x] = { s0, s1 };
			}
			for (uint32_t y = 0; y < dh; ++y)
			{
				uint32_t s0 = (y * sh) / dh;
				uint32_t s1 = ((y + 1) * sh + dh - 1) / dh;
				if (s1 <= s0) s1 = s0 + 1;
				if (s1 > sh) s1 = sh;
				spansY[y] = { s0, s1 };
			}
			for (uint32_t y = 0; y < dh; ++y)
			{
				const uint32_t sx0 = spansY[y].s0;
				const uint32_t sx1 = spansY[y].s1;
				uint32_t *row = dst32 + size_t(y) * dw;
				for (uint32_t x = 0; x < dw; ++x)
				{
					const uint32_t sy0 = spansX[x].s0;
					const uint32_t sy1 = spansX[x].s1;
					uint32_t sumR = 0, sumG = 0, sumB = 0, count = 0;
					for (uint32_t sy = sy0; sy < sy1; ++sy)
					{
						const uint32_t *srcRow = src32 + size_t(sy) * sw;
						for (uint32_t sx = sx0; sx < sx1; ++sx)
						{
							uint32_t c = srcRow[sx];
							sumR += (c & 0xFFu);
							sumG += ((c >> 8) & 0xFFu);
							sumB += ((c >> 16) & 0xFFu);
							count++;
						}
					}
					uint8_t r = (count == 1) ? g_gammaLut[sumR] : (count == 2) ? g_gammaLut[sumR >> 1] : (count == 4) ? g_gammaLut[sumR >> 2] : g_gammaLut[sumR / count];
					uint8_t g = (count == 1) ? g_gammaLut[sumG] : (count == 2) ? g_gammaLut[sumG >> 1] : (count == 4) ? g_gammaLut[sumG >> 2] : g_gammaLut[sumG / count];
					uint8_t b = (count == 1) ? g_gammaLut[sumB] : (count == 2) ? g_gammaLut[sumB >> 1] : (count == 4) ? g_gammaLut[sumB >> 2] : g_gammaLut[sumB / count];
					row[x] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
				}
			}
			break;
		}

		// Fast unsharp sharpening (Laplacian filter with bit-shift >> 1 instead of integer division)
		std::memcpy(&s_sharpened[0], &s_downscaled[0], dw * sizeof(uint32_t));
		std::memcpy(&s_sharpened[(dh - 1) * dw], &s_downscaled[(dh - 1) * dw], dw * sizeof(uint32_t));

		for (uint32_t y = 1; y + 1 < dh; ++y)
		{
			const uint32_t *prevRow = &s_downscaled[(y - 1) * dw];
			const uint32_t *currRow = &s_downscaled[y * dw];
			const uint32_t *nextRow = &s_downscaled[(y + 1) * dw];
			uint32_t *dstRow = &s_sharpened[y * dw];

			dstRow[0] = currRow[0];
			dstRow[dw - 1] = currRow[dw - 1];

			for (uint32_t x = 1; x + 1 < dw; ++x)
			{
				uint32_t c = currRow[x];
				uint32_t up = prevRow[x];
				uint32_t down = nextRow[x];
				uint32_t left = currRow[x - 1];
				uint32_t right = currRow[x + 1];

				uint32_t ul = prevRow[x - 1];
				uint32_t ur = prevRow[x + 1];
				uint32_t dl = nextRow[x - 1];
				uint32_t dr = nextRow[x + 1];

				int c0 = static_cast<int>(c & 0xFFu);
				int c1 = static_cast<int>((c >> 8) & 0xFFu);
				int c2 = static_cast<int>((c >> 16) & 0xFFu);

				int sum0 = static_cast<int>(ul & 0xFFu) + static_cast<int>(up & 0xFFu) + static_cast<int>(ur & 0xFFu) +
				           static_cast<int>(left & 0xFFu) + static_cast<int>(right & 0xFFu) +
				           static_cast<int>(dl & 0xFFu) + static_cast<int>(down & 0xFFu) + static_cast<int>(dr & 0xFFu);

				int sum1 = static_cast<int>((ul >> 8) & 0xFFu) + static_cast<int>((up >> 8) & 0xFFu) + static_cast<int>((ur >> 8) & 0xFFu) +
				           static_cast<int>((left >> 8) & 0xFFu) + static_cast<int>((right >> 8) & 0xFFu) +
				           static_cast<int>((dl >> 8) & 0xFFu) + static_cast<int>((down >> 8) & 0xFFu) + static_cast<int>((dr >> 8) & 0xFFu);

				int sum2 = static_cast<int>((ul >> 16) & 0xFFu) + static_cast<int>((up >> 16) & 0xFFu) + static_cast<int>((ur >> 16) & 0xFFu) +
				           static_cast<int>((left >> 16) & 0xFFu) + static_cast<int>((right >> 16) & 0xFFu) +
				           static_cast<int>((dl >> 16) & 0xFFu) + static_cast<int>((down >> 16) & 0xFFu) + static_cast<int>((dr >> 16) & 0xFFu);

				int lap0 = 8 * c0 - sum0;
				int lap1 = 8 * c1 - sum1;
				int lap2 = 8 * c2 - sum2;

				int s0 = c0 + (lap0 >> 1);
				int s1 = c1 + (lap1 >> 1);
				int s2 = c2 + (lap2 >> 1);

				if (s0 < 0) s0 = 0; else if (s0 > 255) s0 = 255;
				if (s1 < 0) s1 = 0; else if (s1 > 255) s1 = 255;
				if (s2 < 0) s2 = 0; else if (s2 > 255) s2 = 255;

				dstRow[x] = 0xFF000000u | (static_cast<uint32_t>(s2) << 16) | (static_cast<uint32_t>(s1) << 8) | static_cast<uint32_t>(s0);
			}
		}

		// Atomically make processed frame available to the render thread
		{
			std::lock_guard<std::mutex> lk(g_phonePixelsLock);
			if (g_phoneReadyPixels.size() != size_t(dw) * dh)
				g_phoneReadyPixels.resize(size_t(dw) * dh);
			std::memcpy(g_phoneReadyPixels.data(), s_sharpened.data(), size_t(dw) * dh * sizeof(uint32_t));
		}
	}

	void worker_main()
	{
		uint64_t lastProcessedFrame = 0;
		int lastRot = -1;
		float lastGamma = 0.0f;

		while (g_workerRunning.load())
		{
			if (!phone_mode_on())
			{
				Sleep(16);
				continue;
			}

			const uint8_t *px = nullptr;
			uint32_t sw = 0, sh = 0;
			uint64_t frameId = 0;
			if (!acquire_front(px, sw, sh, frameId))
			{
				Sleep(8);
				continue;
			}

			const int rot = g_rotation.load();
			const float gamma = g_gamma.load();

			if (frameId == lastProcessedFrame && rot == lastRot && std::abs(gamma - lastGamma) < 0.001f)
			{
				Sleep(4);
				continue;
			}

			process_phone_frame(px, sw, sh, rot, gamma);
			g_workerReadyFrameId.store(frameId);

			lastProcessedFrame = frameId;
			lastRot = rot;
			lastGamma = gamma;

			Sleep(4);
		}
	}

	// Phone mode: uploads pre-processed Balatro frame from worker thread to GPU (takes <0.05ms)
	void upload_phone_tex(device *dev, command_list *cmd_list)
	{
		if (dev == nullptr)
			return;

		const uint32_t dw = 768, dh = 768;
		const format fmt = format::b8g8r8a8_unorm;

		std::unique_lock<std::mutex> lk(g_phoneCacheLock);
		PhoneBuffer *buf = nullptr;
		for (auto &b : g_phoneTexCache)
		{
			if (b.w == dw && b.h == dh && b.fmt == fmt)
			{
				buf = &b;
				break;
			}
		}

		if (buf == nullptr)
		{
			PhoneBuffer newBuf;
			if (!dev->create_resource(
					resource_desc(dw, dh, 1, 1, fmt, 1, memory_heap::default_, resource_usage::copy_source | resource_usage::copy_dest | resource_usage::shader_resource),
					nullptr, resource_usage::copy_dest, &newBuf.tex))
			{
				logf("BalatroCompositor: FAILED to create phone texture %ux%u fmt %u", dw, dh, static_cast<uint32_t>(fmt));
				return;
			}
			newBuf.w = dw;
			newBuf.h = dh;
			newBuf.fmt = fmt;
			newBuf.inCopySource = false;
			g_phoneTexCache.push_back(std::move(newBuf));
			buf = &g_phoneTexCache.back();
			logf("BalatroCompositor: cached phone texture %ux%u fmt %u", dw, dh, static_cast<uint32_t>(fmt));
		}

		uint64_t readyId = g_workerReadyFrameId.load();
		if (readyId != 0 && readyId != buf->frameId)
		{
			std::unique_lock<std::mutex> pixLk(g_phonePixelsLock, std::try_to_lock);
			if (pixLk.owns_lock() && !g_phoneReadyPixels.empty())
			{
				subresource_data data;
				data.data = g_phoneReadyPixels.data();
				data.row_pitch = dw * 4;
				data.slice_pitch = dw * dh * 4;
				dev->update_texture_region(data, buf->tex, 0);

				if (!buf->ready)
					logf("BalatroCompositor: first Balatro frame uploaded to cached phone texture %ux%u (frame %llu)", dw, dh, static_cast<unsigned long long>(readyId));

				buf->frameId = readyId;
				buf->ready = true;
			}
		}
	}

	// Overwrite GTA's phone render target with Balatro (called when GTA switches away from PHONE_SCREEN).
	void copy_into_phone(command_list *cmd_list, resource phoneRes)
	{
		if (cmd_list == nullptr || phoneRes.handle == 0 || g_phoneCopiedThisFrame.load())
			return;
		device *dev = cmd_list->get_device();
		PhoneBuffer *buf = nullptr;
		{
			std::lock_guard<std::mutex> lk(g_phoneCacheLock);
			if (!g_phoneTexCache.empty() && g_phoneTexCache.front().ready)
				buf = &g_phoneTexCache.front();
		}
		if (buf == nullptr || !buf->ready)
			return;

		if (dev->get_api() == device_api::d3d11)
		{
			cmd_list->barrier(phoneRes, resource_usage::render_target, resource_usage::copy_dest);
			cmd_list->copy_resource(buf->tex, phoneRes);
			cmd_list->barrier(phoneRes, resource_usage::copy_dest, resource_usage::render_target);
		}
		else if (dev->get_api() == device_api::d3d12)
		{
			cmd_list->barrier(buf->tex, resource_usage::copy_dest, resource_usage::copy_source);
			cmd_list->barrier(phoneRes, resource_usage::render_target, resource_usage::copy_dest);
			cmd_list->copy_resource(buf->tex, phoneRes);
			cmd_list->barrier(phoneRes, resource_usage::copy_dest, resource_usage::render_target);
			cmd_list->barrier(buf->tex, resource_usage::copy_source, resource_usage::copy_dest);
		}
		else
		{
			cmd_list->copy_resource(buf->tex, phoneRes);
		}

		g_phoneCopiedThisFrame.store(true);

		if (g_copyCount++ == 0)
		{
			const resource_desc desc = dev->get_resource_desc(phoneRes);
			logf("BalatroCompositor: first copy of Balatro into GTA phone render target (resource 0x%llx, %ux%u, api=0x%x)",
				static_cast<unsigned long long>(phoneRes.handle), desc.texture.width, desc.texture.height,
				static_cast<uint32_t>(dev->get_api()));
		}
	}

	bool is_phone_target(device *dev, resource res, resource_desc &desc)
	{
		desc = dev->get_resource_desc(res);
		if (desc.type != resource_type::texture_2d || desc.texture.samples != 1)
			return false;

		// Fast reject: GTA V's PHONE_SCREEN render target is 768x768.
		if (desc.texture.width != 768 || desc.texture.height != 768)
			return false;

		// The phone screen is a single 2D UI render target with NO mipmaps.
		// Cubemaps and texture arrays have depth_or_layers > 1 (e.g. 6 for cubemaps).
		// Rejecting them prevents overwriting environment/sky reflection probes.
		if (desc.texture.depth_or_layers != 1)
			return false;

		// Reflection probes, cubemaps, and sky/cloud textures have mipmaps (levels > 1)
		// for roughness convolution. The Scaleform phone screen always has exactly 1 level.
		if (desc.texture.levels != 1)
			return false;

		if (desc.texture.format != format::b8g8r8a8_unorm &&
		    desc.texture.format != format::b8g8r8a8_typeless &&
		    desc.texture.format != format::b8g8r8a8_unorm_srgb &&
		    desc.texture.format != format::r8g8b8a8_unorm &&
		    desc.texture.format != format::r8g8b8a8_typeless)
			return false;

		// Direct3D 11 verification: reject any resource flagged as a cubemap or array
		if (dev->get_api() == device_api::d3d11)
		{
			if (ID3D11Resource *d3dRes = reinterpret_cast<ID3D11Resource *>(res.handle))
			{
				ID3D11Texture2D *tex2D = nullptr;
				if (SUCCEEDED(d3dRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex2D))))
				{
					D3D11_TEXTURE2D_DESC d3dDesc;
					tex2D->GetDesc(&d3dDesc);
					tex2D->Release();

					if ((d3dDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0)
						return false;
					if (d3dDesc.ArraySize != 1)
						return false;
					if (d3dDesc.MipLevels != 1)
						return false;
				}
			}
		}

		return true;
	}

	void watch_effect_file(effect_runtime *runtime)
	{
		static DWORD next = 0;
		static FILETIME last = {};
		static wchar_t path[MAX_PATH] = {};
		if (GetTickCount() < next)
			return;
		next = GetTickCount() + 1000;
		if (path[0] == 0)
		{
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			wchar_t *slash = wcsrchr(path, L'\\');
			if (slash)
				wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"reshade-shaders\\Shaders\\BalatroPhone.fx");
		}
		WIN32_FILE_ATTRIBUTE_DATA info;
		if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info))
			return;
		if (last.dwLowDateTime != 0 && CompareFileTime(&info.ftLastWriteTime, &last) != 0)
			runtime->reload_effect_next_frame(nullptr);
		last = info.ftLastWriteTime;
	}

	void on_present(effect_runtime *runtime)
	{
		g_runtime = runtime;
		watch_effect_file(runtime);

		if (phone_mode_on())
		{
			device *dev = runtime->get_device();
			if (dev != nullptr)
			{
				if (auto queue = runtime->get_command_queue())
				{
					command_list *cmd_list = queue->get_immediate_command_list();
					upload_phone_tex(dev, cmd_list);

					// Legacy D3D11 fallback only: in D3D11, immediate context copy is safe
					if (dev->get_api() == device_api::d3d11)
					{
						resource targetRes = g_actualPhoneRes;
						if (!g_phoneCopiedThisFrame.load() && targetRes.handle != 0)
							copy_into_phone(cmd_list, targetRes);
					}
				}
			}
		}

		g_phoneCopiedThisFrame.store(false);
		g_phoneCmdList.store(nullptr);

		// Periodic status line so the log shows whether the phone path is alive.
		static DWORD nextStatus = 0;
		if (g_active.load() && GetTickCount() >= nextStatus)
		{
			nextStatus = GetTickCount() + 5000;
			bool cachedReady = false;
			{
				std::lock_guard<std::mutex> lk(g_phoneCacheLock);
				cachedReady = !g_phoneTexCache.empty() && g_phoneTexCache.front().ready;
			}
			logf("BalatroCompositor: status active=%d mode=%d phoneRenderActive=%d phoneTexReady=%d phoneRes=0x%llx copies=%llu",
				g_active.load() ? 1 : 0, g_mode.load(), g_phoneRenderActive.load() ? 1 : 0, cachedReady ? 1 : 0,
				static_cast<unsigned long long>(g_actualPhoneRes.handle),
				static_cast<unsigned long long>(g_copyCount));
		}
	}

	void on_reloaded_effects(effect_runtime *runtime)
	{
		g_runtime = runtime;
		if (g_width != 0)
			bind_textures(runtime);
	}

	void on_destroy_effect_runtime(effect_runtime *runtime)
	{
		if (g_runtime == runtime)
			g_runtime = nullptr;
		destroy_layer(runtime->get_device());
		destroy_phone_tex(runtime->get_device());
		g_actualPhoneRes = {0};
		g_actualPhoneRtv = {0};
		g_phoneCmdList.store(nullptr);
		g_phoneCopiedThisFrame.store(false);
	}

	void on_begin_effects(effect_runtime *runtime, command_list *cmd_list, resource_view, resource_view)
	{
		g_runtime = runtime;
		const bool active = g_active.load();
		const int mode = g_mode.load();
		const bool effectNeeded = active && (mode == 1 || mode == 2);

		// 1. In Mode 0 (In-Game Phone), upload ready texture from background worker thread (<0.05ms)
		if (phone_mode_on())
			upload_phone_tex(runtime->get_device(), cmd_list);

		// 2. In Mode 1 (Large Phone) and Mode 2 (Fullscreen), update full-resolution frame
		if (effectNeeded)
			upload_frame(runtime, cmd_list);

		// 3. Automatically enable/disable BalatroPhone technique in ReShade
		effect_technique tech = runtime->find_technique("BalatroPhone.fx", "BalatroPhone");
		if (tech.handle == 0)
			tech = runtime->find_technique(nullptr, "BalatroPhone");
		if (tech.handle != 0)
		{
			if (runtime->get_technique_state(tech) != effectNeeded)
			{
				runtime->set_technique_state(tech, effectNeeded);
				logf("BalatroCompositor: set BalatroPhone technique state to %d", effectNeeded ? 1 : 0);
			}
		}

		// 4. Ensure BALATRO_TEX is bound to our full-res texture
		if (effectNeeded && g_balatro.srv.handle != 0)
			runtime->update_texture_bindings("BALATRO_TEX", g_balatro.srv, g_balatro.srv);

		// 5. Update uniform variables
		if (const effect_uniform_variable v = runtime->find_uniform_variable(nullptr, "BalatroActive"); v.handle != 0)
			runtime->set_uniform_value_bool(v, active);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(nullptr, "DisplayMode"); v.handle != 0)
			runtime->set_uniform_value_int(v, mode);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(nullptr, "CamMode"); v.handle != 0)
			runtime->set_uniform_value_int(v, g_camMode.load());

		float x1, y1, x2, y2;
		compositor::get_phone_rect(x1, y1, x2, y2);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(nullptr, "PhoneX1"); v.handle != 0)
			runtime->set_uniform_value_float(v, x1);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(nullptr, "PhoneY1"); v.handle != 0)
			runtime->set_uniform_value_float(v, y1);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(nullptr, "PhoneX2"); v.handle != 0)
			runtime->set_uniform_value_float(v, x2);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(nullptr, "PhoneY2"); v.handle != 0)
			runtime->set_uniform_value_float(v, y2);

		float mx = g_mouseX.load(), my = g_mouseY.load();
		if (const effect_uniform_variable v = runtime->find_uniform_variable(nullptr, "MouseX"); v.handle != 0)
			runtime->set_uniform_value_float(v, mx);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(nullptr, "MouseY"); v.handle != 0)
			runtime->set_uniform_value_float(v, my);
	}

	void on_bind_render_targets(command_list *cmd_list, uint32_t count, const resource_view *rtvs, resource_view)
	{
		if (!phone_mode_on())
		{
			g_phoneCmdList.store(nullptr, std::memory_order_relaxed);
			return;
		}

		// Fast path: once Balatro has been copied into the phone render target for this frame,
		// bypass all subsequent render target bindings this frame (saves thousands of calls).
		if (g_phoneCopiedThisFrame.load(std::memory_order_relaxed))
			return;

		if (count == 0 || rtvs == nullptr || rtvs[0].handle == 0)
		{
			if (g_phoneCmdList.load(std::memory_order_acquire) == cmd_list)
			{
				copy_into_phone(cmd_list, g_actualPhoneRes);
				g_phoneCmdList.store(nullptr, std::memory_order_release);
			}
			return;
		}

		resource newPhone = {0};
		if (g_actualPhoneRtv.handle != 0)
		{
			// Ultra fast path: 0.5-nanosecond integer comparison (NO ReShade mutex lock, NO COM calls)
			if (rtvs[0].handle == g_actualPhoneRtv.handle)
			{
				newPhone = g_actualPhoneRes;
			}
		}
		else
		{
			// Discovery path (runs only once on the first frame until PHONE_SCREEN is first found)
			device *dev = cmd_list->get_device();
			if (dev != nullptr)
			{
				resource res = dev->get_resource_from_view(rtvs[0]);
				if (res.handle != 0)
				{
					resource_desc desc;
					if (is_phone_target(dev, res, desc))
					{
						newPhone = res;
						g_actualPhoneRes = res;
						g_actualPhoneRtv = rtvs[0];
						logf("BalatroCompositor: phone render target discovered: resource 0x%llx rtv=0x%llx %ux%u fmt %u",
							static_cast<unsigned long long>(res.handle),
							static_cast<unsigned long long>(rtvs[0].handle),
							desc.texture.width, desc.texture.height,
							static_cast<uint32_t>(desc.texture.format));
					}
				}
			}
		}

		// GTA just switched away from the phone target on this command list: replace it!
		if (g_phoneCmdList.load(std::memory_order_acquire) == cmd_list && newPhone.handle == 0)
		{
			copy_into_phone(cmd_list, g_actualPhoneRes);
			g_phoneCmdList.store(nullptr, std::memory_order_release);
		}

		if (newPhone.handle != 0)
			g_phoneCmdList.store(cmd_list, std::memory_order_release);
	}

	void on_close_command_list(command_list *cmd_list)
	{
		// In D3D12 (Enhanced), if GTA V finishes the phone UI and closes the command list without
		// binding another target first, copy Balatro into PHONE_SCREEN right before closing.
		if (g_phoneCmdList.load(std::memory_order_acquire) == cmd_list)
		{
			copy_into_phone(cmd_list, g_actualPhoneRes);
			g_phoneCmdList.store(nullptr, std::memory_order_release);
		}
	}
}

namespace compositor
{
	bool try_register(void *module)
	{
		if (g_registered)
			return true;
		if (!reshade::register_addon(module))
			return false;

		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
		reshade::register_event<reshade::addon_event::close_command_list>(on_close_command_list);

		if (!g_workerRunning.exchange(true))
		{
			g_workerThread = std::thread(worker_main);
		}

		g_registered = true;
		reshade::log::message(reshade::log::level::info, "BalatroCompositor: registered");
		return true;
	}

	void unregister(void *module)
	{
		if (g_registered.exchange(false))
		{
			reshade::unregister_event<reshade::addon_event::close_command_list>(on_close_command_list);
			reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
			reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
			reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
			reshade::unregister_event<reshade::addon_event::reshade_present>(on_present);
			reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
			reshade::unregister_addon(module);
		}

		if (g_workerRunning.exchange(false))
		{
			if (g_workerThread.joinable())
				g_workerThread.join();
		}

		if (g_view)
		{
			UnmapViewOfFile(g_view);
			g_view = nullptr;
		}
		if (g_mapping)
		{
			CloseHandle(g_mapping);
			g_mapping = nullptr;
		}
	}

	bool is_registered()
	{
		return g_registered.load();
	}

	void set_active(bool active)
	{
		g_active.store(active);
		if (open_mapping())
		{
			auto *pGtaFlags = reinterpret_cast<uint32_t *>(g_view + kOffGtaState + 4);
			if (active)
				*pGtaFlags |= (g_mode.load() == 2 ? kGtaCasinoSeated : kGtaPhoneOpen) | kGtaInputFocused;
			else
				*pGtaFlags &= ~(kGtaPhoneOpen | kGtaCasinoSeated | kGtaInputFocused);
		}
	}

	bool is_active()
	{
		return g_active.load();
	}

	void set_phone_render_active(bool active)
	{
		g_phoneRenderActive.store(active);
	}

	bool is_phone_render_active()
	{
		return g_phoneRenderActive.load();
	}

	void set_rotation(int rot)
	{
		g_rotation.store(rot);
	}

	int get_rotation()
	{
		return g_rotation.load();
	}

	void set_gamma(float gamma)
	{
		g_gamma.store(gamma);
	}

	float get_gamma()
	{
		return g_gamma.load();
	}

	void set_phone_rect(float x1, float y1, float x2, float y2)
	{
		std::lock_guard<std::mutex> lk(g_rectLock);
		g_phoneX1 = x1;
		g_phoneY1 = y1;
		g_phoneX2 = x2;
		g_phoneY2 = y2;
	}

	void get_phone_rect(float &x1, float &y1, float &x2, float &y2)
	{
		std::lock_guard<std::mutex> lk(g_rectLock);
		x1 = g_phoneX1;
		y1 = g_phoneY1;
		x2 = g_phoneX2;
		y2 = g_phoneY2;
	}

	void set_mode(int mode)
	{
		g_mode.store(mode);
	}

	void set_cam_mode(int mode)
	{
		g_camMode.store(mode);
	}

	void set_mouse_pos(float x, float y)
	{
		g_mouseX.store(x);
		g_mouseY.store(y);
	}

	void get_mouse_pos(float &x, float &y)
	{
		x = g_mouseX.load();
		y = g_mouseY.load();
	}

	bool get_balatro_state(BalatroState &outState)
	{
		if (!open_mapping())
			return false;

		const auto *pState = reinterpret_cast<const BalatroState *>(g_view + kOffBalatroState);
		uint32_t seq1 = pState->seq;
		if (seq1 & 1)
			return false;

		outState = *pState;
		uint32_t seq2 = pState->seq;
		return (seq1 == seq2);
	}

	void send_input_event(uint16_t type, uint16_t code, float x, float y, uint32_t extra)
	{
		if (!open_mapping())
			return;

		auto *pHead = reinterpret_cast<volatile uint64_t *>(g_view + kOffInputRing + kInputRingHeadOff);
		uint64_t head = *pHead;
		uint32_t idx = static_cast<uint32_t>(head % kInputRingEntries);

		auto *events = reinterpret_cast<InputEvent *>(g_view + kOffInputRing + kInputRingDataOff);
		events[idx].type = type;
		events[idx].code = code;
		events[idx].x = x;
		events[idx].y = y;
		events[idx].extra = extra;
		events[idx].pad[0] = 0;
		events[idx].pad[1] = 0;

		*pHead = head + 1;
	}

	void update_heartbeat()
	{
		if (open_mapping())
		{
			auto *pHb = reinterpret_cast<uint64_t *>(g_view + kOffHeader + 16);
			*pHb = GetTickCount64();

			auto *pGtaFlags = reinterpret_cast<uint32_t *>(g_view + kOffGtaState + 4);
			if (g_active.load())
				*pGtaFlags |= (g_mode.load() == 2 ? kGtaCasinoSeated : kGtaPhoneOpen) | kGtaInputFocused;
			else
				*pGtaFlags &= ~(kGtaPhoneOpen | kGtaCasinoSeated | kGtaInputFocused);
		}
	}
}

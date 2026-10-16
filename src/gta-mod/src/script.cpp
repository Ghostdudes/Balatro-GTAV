// script.cpp
// ScriptHookV script component for Balatro <-> GTA V passthrough.
//
// Features:
// 1. Phone View (Mode 0): In 1st person, Michael holds the phone in hand at its natural angle;
//    mouse cursor is mapped using 2D Projective Homography for sub-pixel accuracy across perspective tilt.
//    In 3rd person, displays the landscape phone HUD in the bottom-right corner.
// 2. Large Phone View (Mode 1, F10): Enlarges and centers the in-engine mobile phone UI to fill the screen
//    while preserving the authentic GTA phone bezel, casing, and feel.
// 3. Auto-Launcher: Automatically launches Balatro with '--gta-passthrough' when opened if not already running.
//    Normal standalone Balatro launches remain 100% vanilla, unmuted, and unthrottled.
// 4. Input forwarding: Accurate mouse move, LMB, RMB, and scroll wheel forwarding to Balatro.

#include "compositor.h"
#include "natives.h"
#include <main.h>
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>

using namespace balatro_gta::proto;

namespace
{
	HMODULE g_module = nullptr;
	int g_currentMode = 0; // 0 = Standard Phone, 1 = Large Phone (Full Screen)

	const int kDisabledControls[] = {
		1, 2,                                         // Look LR, Look UD (freeze camera rotation)
		24, 25, 257, 140, 141, 142, 143, 263, 264,   // Attack, aim, melee
		14, 15, 16, 17, 37, 261, 262,                // Weapon wheel / next / prev
		157, 158, 159, 160, 161, 162, 163, 164, 165, // Weapon slots
		44, 45, 47, 58,                               // Cover, reload, grenade
		68, 69, 70, 91, 92, 99, 100, 114, 115, 116,  // Vehicle weapons
		22, 23, 75,                                   // Jump, enter/exit vehicle
		27, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181 // Phone controls
	};

	// 2D Projective Homography solver for mapping screen coordinates to texture/Balatro coordinates
	struct Homography2D
	{
		float m[3][3] = {};
		bool valid = false;

		bool compute(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3)
		{
			float dx1 = x1 - x2, dx2 = x3 - x2, dx3 = x0 - x1 + x2 - x3;
			float dy1 = y1 - y2, dy2 = y3 - y2, dy3 = y0 - y1 + y2 - y3;

			float denom = dx1 * dy2 - dx2 * dy1;
			float a11, a12, a13, a21, a22, a23, a31, a32, a33 = 1.0f;

			if (std::abs(denom) < 1e-7f)
			{
				a31 = 0.0f;
				a32 = 0.0f;
				a11 = x1 - x0;
				a12 = x3 - x0;
				a13 = x0;
				a21 = y1 - y0;
				a22 = y3 - y0;
				a23 = y0;
			}
			else
			{
				a31 = (dx3 * dy2 - dx2 * dy3) / denom;
				a32 = (dx1 * dy3 - dx3 * dy1) / denom;
				a11 = x1 - x0 + a31 * x1;
				a12 = x3 - x0 + a32 * x3;
				a13 = x0;
				a21 = y1 - y0 + a31 * y1;
				a22 = y3 - y0 + a32 * y3;
				a23 = y0;
			}

			float det = a11 * (a22 * a33 - a23 * a32) - a12 * (a21 * a33 - a23 * a31) + a13 * (a21 * a32 - a22 * a31);
			if (std::abs(det) < 1e-7f)
			{
				valid = false;
				return false;
			}

			m[0][0] = (a22 * a33 - a23 * a32) / det;
			m[0][1] = (a13 * a32 - a12 * a33) / det;
			m[0][2] = (a12 * a23 - a13 * a22) / det;

			m[1][0] = (a23 * a31 - a21 * a33) / det;
			m[1][1] = (a11 * a33 - a13 * a31) / det;
			m[1][2] = (a13 * a21 - a11 * a23) / det;

			m[2][0] = (a21 * a32 - a22 * a31) / det;
			m[2][1] = (a12 * a31 - a11 * a32) / det;
			m[2][2] = (a11 * a22 - a12 * a21) / det;

			valid = true;
			return true;
		}

		bool map(float x, float y, float &outU, float &outV, bool &overQuad) const
		{
			if (!valid)
				return false;

			float w = m[2][0] * x + m[2][1] * y + m[2][2];
			if (std::abs(w) < 1e-7f)
				return false;

			float u = (m[0][0] * x + m[0][1] * y + m[0][2]) / w;
			float v = (m[1][0] * x + m[1][1] * y + m[1][2]) / w;

			overQuad = (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f);
			outU = std::clamp(u, 0.0f, 1.0f);
			outV = std::clamp(v, 0.0f, 1.0f);
			return true;
		}
	} g_fpHomography;

	struct PhoneCfg
	{
		// Default landscape placement for 3rd-person HUD phone (bottom-right)
		float posX = 89.62f, posY = -25.305f, posZ = -113.0f;
		float rotX = -90.0f, rotY = 0.0f, rotZ = 90.0f;
		float scale = 575.0f;

		// Large Phone placement (Mode 1 / F10: centered, large UI phone)
		float largePosX = 6.0f, largePosY = 0.0f, largePosZ = -60.0f;
		float largeScale = 740.0f;
		float largeScreenX1 = 0.2432f, largeScreenY1 = 0.1840f;
		float largeScreenX2 = 0.7764f, largeScreenY2 = 0.8247f;
		bool showTooltip = true;

		// Visual bounding box indicator and 3D tracker settings
		bool showDebugQuad = false;
		DWORD boundsHighlightTimer = 0;
		float fpScreenInset = 0.90f;

		int rotation = 1; // 0 = 0 deg, 1 = 90 deg CW, 2 = 180 deg, 3 = 270 deg CW
		float gamma = 1.25f;

		// Third-person HUD phone active screen bounds
		float hudX1 = 0.7021f, hudY1 = 0.5365f;
		float hudX2 = 0.9092f, hudY2 = 0.7865f;

		// First-person in-hand phone active screen mapping quad corners
		float fpTL_X = 0.6162f, fpTL_Y = 0.5608f;
		float fpTR_X = 0.8477f, fpTR_Y = 0.5608f;
		float fpBR_X = 0.8477f, fpBR_Y = 0.8438f;
		float fpBL_X = 0.6162f, fpBL_Y = 0.8438f;

		// Auto-launch Balatro settings
		std::string balatroExe = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Balatro\\Balatro.exe";
		std::string balatroArgs = "--gta-passthrough";
		bool autoLaunchBalatro = true;

		// Keybinds (default: F9 for toggle, F10 for mode cycle)
		int toggleKey = VK_F9;
		int modeKey = VK_F10;
		std::string toggleKeyStr = "F9";
		std::string modeKeyStr = "F10";
	} g_cfg;

	std::string g_iniPath;
	FILETIME g_iniTime = {};
	bool g_phoneOpen = false;
	int g_lastCamMode = -1;

	void recompute_large_phone_bounds()
	{
		float zDist = std::max(10.0f, std::abs(g_cfg.largePosZ));
		float zRatio = 113.0f / zDist;
		float sRatio = g_cfg.largeScale / 575.0f;
		float scaleFactor = sRatio * zRatio;

		// Calibrated base phone screen dimensions for Large View:
		// Target w = 0.5332, h = 0.6407 at Scale=740, PosZ=-60 -> base w = 0.2200, h = 0.2643
		float w = 0.2200f * scaleFactor;
		float h = 0.2643f * scaleFactor;

		// Center: PosX=6.0, PosY=0.0 is center of screen (cx=0.5098, cy=0.5044)
		float cx = 0.5098f + ((g_cfg.largePosX - 6.0f) / zDist) * 0.4297f;
		float cy = 0.5044f - (g_cfg.largePosY / zDist) * 0.7279f;

		g_cfg.largeScreenX1 = cx - w * 0.5f;
		g_cfg.largeScreenX2 = cx + w * 0.5f;
		g_cfg.largeScreenY1 = cy - h * 0.5f;
		g_cfg.largeScreenY2 = cy + h * 0.5f;
	}

	float ini_float(const char *sec, const char *key, float def)
	{
		char buf[64], defs[64];
		snprintf(defs, sizeof(defs), "%g", def);
		GetPrivateProfileStringA(sec, key, defs, buf, sizeof(buf), g_iniPath.c_str());
		return static_cast<float>(atof(buf));
	}

	int ini_int(const char *sec, const char *key, int def)
	{
		char buf[64], defs[64];
		snprintf(defs, sizeof(defs), "%d", def);
		GetPrivateProfileStringA(sec, key, defs, buf, sizeof(buf), g_iniPath.c_str());
		return atoi(buf);
	}

	std::string ini_string(const char *sec, const char *key, const std::string &def)
	{
		char buf[MAX_PATH];
		GetPrivateProfileStringA(sec, key, def.c_str(), buf, sizeof(buf), g_iniPath.c_str());
		return std::string(buf);
	}

	void ini_write(const char *sec, const char *key, float v)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "%.4f", v);
		WritePrivateProfileStringA(sec, key, buf, g_iniPath.c_str());
	}

	void ini_write_int(const char *sec, const char *key, int v)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "%d", v);
		WritePrivateProfileStringA(sec, key, buf, g_iniPath.c_str());
	}

	void ini_write_string(const char *sec, const char *key, const std::string &v)
	{
		WritePrivateProfileStringA(sec, key, v.c_str(), g_iniPath.c_str());
	}

	void save_cfg()
	{
		ini_write("Phone", "PosX", g_cfg.posX);
		ini_write("Phone", "PosY", g_cfg.posY);
		ini_write("Phone", "PosZ", g_cfg.posZ);
		ini_write("Phone", "RotX", g_cfg.rotX);
		ini_write("Phone", "RotY", g_cfg.rotY);
		ini_write("Phone", "RotZ", g_cfg.rotZ);
		ini_write("Phone", "Scale", g_cfg.scale);

		ini_write("LargePhone", "PosX", g_cfg.largePosX);
		ini_write("LargePhone", "PosY", g_cfg.largePosY);
		ini_write("LargePhone", "PosZ", g_cfg.largePosZ);
		ini_write("LargePhone", "Scale", g_cfg.largeScale);
		ini_write("LargePhone", "ScreenX1", g_cfg.largeScreenX1);
		ini_write("LargePhone", "ScreenY1", g_cfg.largeScreenY1);
		ini_write("LargePhone", "ScreenX2", g_cfg.largeScreenX2);
		ini_write("LargePhone", "ScreenY2", g_cfg.largeScreenY2);
		ini_write_int("LargePhone", "ShowTooltip", g_cfg.showTooltip ? 1 : 0);

		ini_write_int("Screen", "Rotation", g_cfg.rotation);
		ini_write("Screen", "Gamma", g_cfg.gamma);
		ini_write("Screen", "ScreenX1", g_cfg.hudX1);
		ini_write("Screen", "ScreenY1", g_cfg.hudY1);
		ini_write("Screen", "ScreenX2", g_cfg.hudX2);
		ini_write("Screen", "ScreenY2", g_cfg.hudY2);

		ini_write("Screen_FP", "TL_X", g_cfg.fpTL_X);
		ini_write("Screen_FP", "TL_Y", g_cfg.fpTL_Y);
		ini_write("Screen_FP", "TR_X", g_cfg.fpTR_X);
		ini_write("Screen_FP", "TR_Y", g_cfg.fpTR_Y);
		ini_write("Screen_FP", "BR_X", g_cfg.fpBR_X);
		ini_write("Screen_FP", "BR_Y", g_cfg.fpBR_Y);
		ini_write("Screen_FP", "BL_X", g_cfg.fpBL_X);
		ini_write("Screen_FP", "BL_Y", g_cfg.fpBL_Y);
		ini_write("Screen_FP", "Inset", g_cfg.fpScreenInset);

		ini_write_int("Debug", "ShowBounds", g_cfg.showDebugQuad ? 1 : 0);

		ini_write_string("Balatro", "ExecutablePath", g_cfg.balatroExe);
		ini_write_string("Balatro", "Arguments", g_cfg.balatroArgs);
		ini_write_int("Balatro", "AutoLaunchOnOpen", g_cfg.autoLaunchBalatro ? 1 : 0);

		ini_write_string("Controls", "ToggleKey", g_cfg.toggleKeyStr.empty() ? "F9" : g_cfg.toggleKeyStr);
		ini_write_string("Controls", "ModeKey", g_cfg.modeKeyStr.empty() ? "F10" : g_cfg.modeKeyStr);

		WIN32_FILE_ATTRIBUTE_DATA info;
		if (GetFileAttributesExA(g_iniPath.c_str(), GetFileExInfoStandard, &info))
			g_iniTime = info.ftLastWriteTime;
	}

	int parse_key_code(const std::string &str, int defaultKey)
	{
		if (str.empty())
			return defaultKey;

		// Hex representation: "0x78"
		if (str.size() > 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X'))
		{
			try {
				return std::stoi(str, nullptr, 16);
			} catch (...) { return defaultKey; }
		}

		// Decimal digits: "120"
		bool allDigits = true;
		for (char c : str)
		{
			if (!isdigit(static_cast<unsigned char>(c)))
			{
				allDigits = false;
				break;
			}
		}
		if (allDigits)
		{
			try {
				return std::stoi(str);
			} catch (...) { return defaultKey; }
		}

		// Case-insensitive uppercase key names
		std::string s = str;
		for (char &c : s)
			c = static_cast<char>(toupper(static_cast<unsigned char>(c)));

		// F1 - F24
		if (s.size() >= 2 && s[0] == 'F')
		{
			int fNum = atoi(s.c_str() + 1);
			if (fNum >= 1 && fNum <= 12)
				return VK_F1 + (fNum - 1);
			if (fNum >= 13 && fNum <= 24)
				return VK_F13 + (fNum - 13);
		}

		// Common keyboard keys
		if (s == "TAB") return VK_TAB;
		if (s == "INSERT" || s == "INS") return VK_INSERT;
		if (s == "DELETE" || s == "DEL") return VK_DELETE;
		if (s == "HOME") return VK_HOME;
		if (s == "END") return VK_END;
		if (s == "PAGEUP" || s == "PGUP") return VK_PRIOR;
		if (s == "PAGEDOWN" || s == "PGDN") return VK_NEXT;
		if (s == "PAUSE") return VK_PAUSE;
		if (s == "CAPSLOCK" || s == "CAPS") return VK_CAPITAL;
		if (s == "ESCAPE" || s == "ESC") return VK_ESCAPE;
		if (s == "SPACE") return VK_SPACE;
		if (s == "TILDE" || s == "GRAVE" || s == "`" || s == "~") return VK_OEM_3;
		if (s == "BACKSPACE" || s == "BACK") return VK_BACK;

		// Numpad 0-9
		if (s.rfind("NUMPAD", 0) == 0 && s.size() == 7 && isdigit(s[6]))
			return VK_NUMPAD0 + (s[6] - '0');
		if (s.rfind("NUM", 0) == 0 && s.size() == 4 && isdigit(s[3]))
			return VK_NUMPAD0 + (s[3] - '0');

		// Single alphanumeric character (A-Z, 0-9)
		if (s.size() == 1)
		{
			char c = s[0];
			if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
				return static_cast<int>(c);
		}

		return defaultKey;
	}

	void load_cfg_if_changed()
	{
		static DWORD s_lastCheck = 0;
		DWORD now = GetTickCount();
		if (now - s_lastCheck < 1000)
			return;
		s_lastCheck = now;

		if (g_iniPath.empty())
		{
			char path[MAX_PATH];
			GetModuleFileNameA(nullptr, path, MAX_PATH);
			std::string p(path);
			g_iniPath = p.substr(0, p.find_last_of("\\/") + 1) + "BalatroPassthrough.ini";
			if (GetFileAttributesA(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
				save_cfg();
		}
		WIN32_FILE_ATTRIBUTE_DATA info;
		if (!GetFileAttributesExA(g_iniPath.c_str(), GetFileExInfoStandard, &info))
			return;
		if (CompareFileTime(&info.ftLastWriteTime, &g_iniTime) == 0)
			return;
		g_iniTime = info.ftLastWriteTime;

		g_cfg.posX = ini_float("Phone", "PosX", g_cfg.posX);
		g_cfg.posY = ini_float("Phone", "PosY", g_cfg.posY);
		g_cfg.posZ = ini_float("Phone", "PosZ", g_cfg.posZ);
		g_cfg.rotX = ini_float("Phone", "RotX", g_cfg.rotX);
		g_cfg.rotY = ini_float("Phone", "RotY", g_cfg.rotY);
		g_cfg.rotZ = ini_float("Phone", "RotZ", g_cfg.rotZ);
		g_cfg.scale = ini_float("Phone", "Scale", g_cfg.scale);

		g_cfg.largePosX = ini_float("LargePhone", "PosX", g_cfg.largePosX);
		g_cfg.largePosY = ini_float("LargePhone", "PosY", g_cfg.largePosY);
		g_cfg.largePosZ = ini_float("LargePhone", "PosZ", g_cfg.largePosZ);
		g_cfg.largeScale = ini_float("LargePhone", "Scale", g_cfg.largeScale);
		g_cfg.largeScreenX1 = ini_float("LargePhone", "ScreenX1", 0.0f);
		g_cfg.largeScreenY1 = ini_float("LargePhone", "ScreenY1", 0.0f);
		g_cfg.largeScreenX2 = ini_float("LargePhone", "ScreenX2", 0.0f);
		g_cfg.largeScreenY2 = ini_float("LargePhone", "ScreenY2", 0.0f);
		g_cfg.showTooltip = (ini_int("LargePhone", "ShowTooltip", g_cfg.showTooltip ? 1 : 0) != 0);

		// If bounds are uninitialized or use the old outer casing box (width > 0.60 at default scale 740), recompute
		if (g_cfg.largeScreenX2 <= g_cfg.largeScreenX1 || (g_cfg.largeScreenX2 - g_cfg.largeScreenX1) > 0.60f)
		{
			recompute_large_phone_bounds();
			save_cfg();
		}

		g_cfg.showDebugQuad = (ini_int("Debug", "ShowBounds", g_cfg.showDebugQuad ? 1 : 0) != 0);
		g_cfg.fpScreenInset = ini_float("Screen_FP", "Inset", g_cfg.fpScreenInset);

		g_cfg.rotation = ini_int("Screen", "Rotation", g_cfg.rotation);
		compositor::set_rotation(g_cfg.rotation);

		g_cfg.gamma = ini_float("Screen", "Gamma", g_cfg.gamma);
		compositor::set_gamma(g_cfg.gamma);

		g_cfg.hudX1 = ini_float("Screen", "ScreenX1", g_cfg.hudX1);
		g_cfg.hudY1 = ini_float("Screen", "ScreenY1", g_cfg.hudY1);
		g_cfg.hudX2 = ini_float("Screen", "ScreenX2", g_cfg.hudX2);
		g_cfg.hudY2 = ini_float("Screen", "ScreenY2", g_cfg.hudY2);
		if (g_cfg.hudX1 < 0.68f || (g_cfg.hudX2 - g_cfg.hudX1) > 0.25f)
		{
			g_cfg.hudX1 = 0.7021f; g_cfg.hudY1 = 0.5365f;
			g_cfg.hudX2 = 0.9092f; g_cfg.hudY2 = 0.7865f;
			save_cfg();
		}
		compositor::set_phone_rect(g_cfg.hudX1, g_cfg.hudY1, g_cfg.hudX2, g_cfg.hudY2);

		g_cfg.fpTL_X = ini_float("Screen_FP", "TL_X", g_cfg.fpTL_X);
		g_cfg.fpTL_Y = ini_float("Screen_FP", "TL_Y", g_cfg.fpTL_Y);
		g_cfg.fpTR_X = ini_float("Screen_FP", "TR_X", g_cfg.fpTR_X);
		g_cfg.fpTR_Y = ini_float("Screen_FP", "TR_Y", g_cfg.fpTR_Y);
		g_cfg.fpBR_X = ini_float("Screen_FP", "BR_X", g_cfg.fpBR_X);
		g_cfg.fpBR_Y = ini_float("Screen_FP", "BR_Y", g_cfg.fpBR_Y);
		g_cfg.fpBL_X = ini_float("Screen_FP", "BL_X", g_cfg.fpBL_X);
		g_cfg.fpBL_Y = ini_float("Screen_FP", "BL_Y", g_cfg.fpBL_Y);
		if (g_cfg.fpTL_X <= 0.0f || g_cfg.fpTR_X <= g_cfg.fpTL_X || g_cfg.fpTR_X > 0.950f)
		{
			g_cfg.fpTL_X = 0.6162f; g_cfg.fpTL_Y = 0.5608f;
			g_cfg.fpTR_X = 0.8477f; g_cfg.fpTR_Y = 0.5608f;
			g_cfg.fpBR_X = 0.8477f; g_cfg.fpBR_Y = 0.8438f;
			g_cfg.fpBL_X = 0.6162f; g_cfg.fpBL_Y = 0.8438f;
			save_cfg();
		}

		g_fpHomography.compute(g_cfg.fpTL_X, g_cfg.fpTL_Y,
		                      g_cfg.fpTR_X, g_cfg.fpTR_Y,
		                      g_cfg.fpBR_X, g_cfg.fpBR_Y,
		                      g_cfg.fpBL_X, g_cfg.fpBL_Y);

		g_cfg.balatroExe = ini_string("Balatro", "ExecutablePath", g_cfg.balatroExe);
		g_cfg.balatroArgs = ini_string("Balatro", "Arguments", g_cfg.balatroArgs);
		g_cfg.autoLaunchBalatro = (ini_int("Balatro", "AutoLaunchOnOpen", 1) != 0);

		g_cfg.toggleKeyStr = ini_string("Controls", "ToggleKey", "F9");
		g_cfg.toggleKey = parse_key_code(g_cfg.toggleKeyStr, VK_F9);

		g_cfg.modeKeyStr = ini_string("Controls", "ModeKey", "F10");
		g_cfg.modeKey = parse_key_code(g_cfg.modeKeyStr, VK_F10);
	}

	bool is_balatro_running()
	{
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE)
			return false;

		PROCESSENTRY32W pe;
		pe.dwSize = sizeof(pe);
		bool running = false;
		if (Process32FirstW(snap, &pe))
		{
			do
			{
				if (_wcsicmp(pe.szExeFile, L"Balatro.exe") == 0)
				{
					running = true;
					break;
				}
			} while (Process32NextW(snap, &pe));
		}
		CloseHandle(snap);
		return running;
	}

	void launch_balatro_if_needed()
	{
		if (!g_cfg.autoLaunchBalatro)
			return;
		if (is_balatro_running())
			return;

		// Set environment variable so Balatro bridge activates even if arguments are wrapped by a launcher
		SetEnvironmentVariableA("BALATRO_GTA_PASSTHROUGH", "1");

		char cmd[MAX_PATH * 2];
		snprintf(cmd, sizeof(cmd), "\"%s\" %s", g_cfg.balatroExe.c_str(), g_cfg.balatroArgs.c_str());

		STARTUPINFOA si = { sizeof(si) };
		PROCESS_INFORMATION pi = {};

		std::string dir = g_cfg.balatroExe;
		size_t lastSlash = dir.find_last_of("\\/");
		const char *workDir = (lastSlash != std::string::npos) ? dir.substr(0, lastSlash).c_str() : nullptr;

		if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, workDir, &si, &pi))
		{
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			natives::Notify("Balatro: ~g~Launched~s~ with --gta-passthrough");
		}
		else
		{
			natives::Notify("Balatro: ~r~Failed~s~ to launch executable");
		}
	}

	int phone_type_for_player()
	{
		const Hash model = natives::GetEntityModel(natives::PlayerPedId());
		if (model == 0x9B810FA2) // Trevor: Facade
			return 1;
		if (model == 0x9B22DBAF) // Franklin: Badger
			return 2;
		return 0;                // Michael: iFruit
	}

	// Dynamic 3D model tracker for the in-hand phone in first-person mode
	struct Fp3DTracker
	{
		Entity phoneEntity = 0;
		DWORD nextScanTime = 0;
		bool initialized = false;

		// Local 3D screen corners on the phone entity
		Vector3 localTL = {};
		Vector3 localTR = {};
		Vector3 localBR = {};
		Vector3 localBL = {};

		// Projected 2D screen coordinates
		float screenTL_X = 0.6128f, screenTL_Y = 0.5624f;
		float screenTR_X = 0.8384f, screenTR_Y = 0.5624f;
		float screenBR_X = 0.8442f, screenBR_Y = 0.8475f;
		float screenBL_X = 0.6196f, screenBL_Y = 0.8458f;
		bool valid = false;

		void reset()
		{
			phoneEntity = 0;
			initialized = false;
			valid = false;
		}

		bool update(Ped ped)
		{
			DWORD now = GetTickCount();

			if (phoneEntity != 0 && !natives::DoesEntityExist(phoneEntity))
			{
				reset();
			}

			if (phoneEntity == 0 && now >= nextScanTime)
			{
				nextScanTime = now + 400;
				Vector3 pCoords = natives::GetEntityCoords(ped, TRUE);

				const Hash models[] = {
					natives::GetHashKey("prop_phone_ing"),
					natives::GetHashKey("prop_phone_ing_02"),
					natives::GetHashKey("prop_phone_ing_03"),
					natives::GetHashKey("prop_npc_phone"),
					natives::GetHashKey("prop_amb_phone")
				};

				for (Hash m : models)
				{
					Entity candidate = natives::GetClosestObjectOfType(pCoords.x, pCoords.y, pCoords.z, 2.5f, m, FALSE, FALSE, FALSE);
					if (candidate != 0 && natives::DoesEntityExist(candidate))
					{
						phoneEntity = candidate;
						initialized = false;
						break;
					}
				}
			}

			if (phoneEntity == 0)
			{
				valid = false;
				return false;
			}

			if (!initialized)
			{
				Hash model = natives::GetEntityModel(phoneEntity);
				Vector3 minDim = {}, maxDim = {};
				natives::GetModelDimensions(model, &minDim, &maxDim);

				float spanX = maxDim.x - minDim.x;
				float spanY = maxDim.y - minDim.y;
				float spanZ = maxDim.z - minDim.z;

				// Thickness axis is smallest span
				int thickAxis = 0;
				if (spanY < spanX && spanY < spanZ) thickAxis = 1;
				else if (spanZ < spanX && spanZ < spanY) thickAxis = 2;

				int axisA = (thickAxis == 0) ? 1 : 0;
				int axisB = (thickAxis == 2) ? 1 : 2;

				float minA = (axisA == 0) ? minDim.x : ((axisA == 1) ? minDim.y : minDim.z);
				float maxA = (axisA == 0) ? maxDim.x : ((axisA == 1) ? maxDim.y : maxDim.z);
				float minB = (axisB == 0) ? minDim.x : ((axisB == 1) ? minDim.y : minDim.z);
				float maxB = (axisB == 0) ? maxDim.x : ((axisB == 1) ? maxDim.y : maxDim.z);
				float minT = (thickAxis == 0) ? minDim.x : ((thickAxis == 1) ? minDim.y : minDim.z);
				float maxT = (thickAxis == 0) ? maxDim.x : ((thickAxis == 1) ? maxDim.y : maxDim.z);

				auto makeVec = [](int tAxis, int aAxis, int bAxis, float t, float a, float b) -> Vector3 {
					Vector3 v = {};
					if (tAxis == 0) v.x = t; else if (aAxis == 0) v.x = a; else v.x = b;
					if (tAxis == 1) v.y = t; else if (aAxis == 1) v.y = a; else v.y = b;
					if (tAxis == 2) v.z = t; else if (aAxis == 2) v.z = a; else v.z = b;
					return v;
				};

				Vector3 centerMin = makeVec(thickAxis, axisA, axisB, minT, (minA + maxA) * 0.5f, (minB + maxB) * 0.5f);
				Vector3 centerMax = makeVec(thickAxis, axisA, axisB, maxT, (minA + maxA) * 0.5f, (minB + maxB) * 0.5f);

				Vector3 wMin = natives::GetOffsetFromEntityInWorldCoords(phoneEntity, centerMin.x, centerMin.y, centerMin.z);
				Vector3 wMax = natives::GetOffsetFromEntityInWorldCoords(phoneEntity, centerMax.x, centerMax.y, centerMax.z);
				Vector3 camPos = natives::GetGameplayCamCoord();

				float dMin = (wMin.x - camPos.x) * (wMin.x - camPos.x) + (wMin.y - camPos.y) * (wMin.y - camPos.y) + (wMin.z - camPos.z) * (wMin.z - camPos.z);
				float dMax = (wMax.x - camPos.x) * (wMax.x - camPos.x) + (wMax.y - camPos.y) * (wMax.y - camPos.y) + (wMax.z - camPos.z) * (wMax.z - camPos.z);
				float frontT = (dMin < dMax) ? minT : maxT;

				Vector3 c[4];
				c[0] = makeVec(thickAxis, axisA, axisB, frontT, minA, minB);
				c[1] = makeVec(thickAxis, axisA, axisB, frontT, maxA, minB);
				c[2] = makeVec(thickAxis, axisA, axisB, frontT, maxA, maxB);
				c[3] = makeVec(thickAxis, axisA, axisB, frontT, minA, maxB);

				float sx[4] = {}, sy[4] = {};
				bool allOnScreen = true;
				for (int i = 0; i < 4; ++i)
				{
					Vector3 w = natives::GetOffsetFromEntityInWorldCoords(phoneEntity, c[i].x, c[i].y, c[i].z);
					if (!natives::GetScreenCoordFromWorldCoord(w.x, w.y, w.z, &sx[i], &sy[i]))
						allOnScreen = false;
				}

				if (!allOnScreen)
					return false;

				int idxTL = 0, idxBR = 0, idxTR = 0, idxBL = 0;
				float minSum = 1e9f, maxSum = -1e9f, maxDiff = -1e9f, minDiff = 1e9f;

				for (int i = 0; i < 4; ++i)
				{
					float s = sx[i] + sy[i];
					float d = sx[i] - sy[i];
					if (s < minSum) { minSum = s; idxTL = i; }
					if (s > maxSum) { maxSum = s; idxBR = i; }
					if (d > maxDiff) { maxDiff = d; idxTR = i; }
					if (d < minDiff) { minDiff = d; idxBL = i; }
				}

				Vector3 faceCenter = makeVec(thickAxis, axisA, axisB, frontT, (minA + maxA) * 0.5f, (minB + maxB) * 0.5f);
				float inset = g_cfg.fpScreenInset;

				auto applyInset = [&](const Vector3 &pt) -> Vector3 {
					Vector3 res;
					res.x = faceCenter.x + (pt.x - faceCenter.x) * inset;
					res.y = faceCenter.y + (pt.y - faceCenter.y) * inset;
					res.z = faceCenter.z + (pt.z - faceCenter.z) * inset;
					return res;
				};

				localTL = applyInset(c[idxTL]);
				localTR = applyInset(c[idxTR]);
				localBR = applyInset(c[idxBR]);
				localBL = applyInset(c[idxBL]);

				initialized = true;
			}

			Vector3 wTL = natives::GetOffsetFromEntityInWorldCoords(phoneEntity, localTL.x, localTL.y, localTL.z);
			Vector3 wTR = natives::GetOffsetFromEntityInWorldCoords(phoneEntity, localTR.x, localTR.y, localTR.z);
			Vector3 wBR = natives::GetOffsetFromEntityInWorldCoords(phoneEntity, localBR.x, localBR.y, localBR.z);
			Vector3 wBL = natives::GetOffsetFromEntityInWorldCoords(phoneEntity, localBL.x, localBL.y, localBL.z);

			float tlx, tly, trx, try_, brx, bry, blx, bly;
			if (natives::GetScreenCoordFromWorldCoord(wTL.x, wTL.y, wTL.z, &tlx, &tly) &&
			    natives::GetScreenCoordFromWorldCoord(wTR.x, wTR.y, wTR.z, &trx, &try_) &&
			    natives::GetScreenCoordFromWorldCoord(wBR.x, wBR.y, wBR.z, &brx, &bry) &&
			    natives::GetScreenCoordFromWorldCoord(wBL.x, wBL.y, wBL.z, &blx, &bly))
			{
				screenTL_X = tlx; screenTL_Y = tly;
				screenTR_X = trx; screenTR_Y = try_;
				screenBR_X = brx; screenBR_Y = bry;
				screenBL_X = blx; screenBL_Y = bly;
				valid = true;
				return true;
			}

			valid = false;
			return false;
		}
	} g_fpTracker;

	int get_current_cam_view_mode()
	{
		Ped ped = natives::PlayerPedId();
		if (natives::IsPedInAnyVehicle(ped, FALSE))
			return natives::GetFollowVehicleCamViewMode();
		return natives::GetFollowPedCamViewMode();
	}

	void apply_phone_view_mode(int camMode)
	{
		if (g_currentMode == 2) // Mode 2: Fullscreen View
		{
			// Hide in-engine mobile phone UI off-screen
			natives::SetMobilePhonePosition(0.0f, -1000.0f, -1000.0f);
			natives::SetMobilePhoneScale(0.0f);
			natives::SetCellCamLean(FALSE);
			compositor::set_phone_rect(0.0f, 0.0f, 1.0f, 1.0f);
		}
		else if (g_currentMode == 1) // Mode 1: Large Phone View (F10)
		{
			// Enlarge and center the in-engine mobile phone UI
			natives::SetMobilePhonePosition(g_cfg.largePosX, g_cfg.largePosY, g_cfg.largePosZ);
			natives::SetMobilePhoneRotation(g_cfg.rotX, g_cfg.rotY, g_cfg.rotZ);
			natives::SetMobilePhoneScale(g_cfg.largeScale);
			natives::SetCellCamLean(FALSE);
			compositor::set_phone_rect(g_cfg.largeScreenX1, g_cfg.largeScreenY1, g_cfg.largeScreenX2, g_cfg.largeScreenY2);
		}
		else // Mode 0: Standard Phone View
		{
			if (camMode == 4) // First-person mode
			{
				// Hide redundant 2D HUD phone so the player looks directly at the 3D phone in hand
				natives::SetMobilePhonePosition(0.0f, -1000.0f, -1000.0f);
				natives::SetMobilePhoneScale(0.0f);
				natives::SetCellCamLean(TRUE);
				compositor::set_phone_rect(g_cfg.fpTL_X, g_cfg.fpTL_Y, g_cfg.fpTR_X, g_cfg.fpBR_Y);
			}
			else // Third-person mode
			{
				// Position HUD phone in landscape (matching email orientation)
				natives::SetMobilePhonePosition(g_cfg.posX, g_cfg.posY, g_cfg.posZ);
				natives::SetMobilePhoneRotation(g_cfg.rotX, g_cfg.rotY, g_cfg.rotZ);
				natives::SetMobilePhoneScale(g_cfg.scale);
				natives::SetCellCamLean(FALSE);
				compositor::set_phone_rect(g_cfg.hudX1, g_cfg.hudY1, g_cfg.hudX2, g_cfg.hudY2);
			}
		}
	}

	struct FindGtaContext {
		DWORD pid;
		HWND hwnd;
	};

	HWND get_gta_window()
	{
		static HWND s_hwnd = nullptr;
		if (s_hwnd && IsWindow(s_hwnd))
			return s_hwnd;

		s_hwnd = FindWindowA("grcWindow", nullptr);
		if (s_hwnd && IsWindow(s_hwnd))
			return s_hwnd;

		FindGtaContext ctx = { GetCurrentProcessId(), nullptr };
		EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
			auto *c = reinterpret_cast<FindGtaContext *>(lp);
			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid == c->pid && IsWindowVisible(hwnd) && GetWindowTextLengthA(hwnd) > 0)
			{
				c->hwnd = hwnd;
				return FALSE;
			}
			return TRUE;
		}, reinterpret_cast<LPARAM>(&ctx));

		s_hwnd = ctx.hwnd;
		return s_hwnd;
	}

	bool is_gta_focused()
	{
		HWND gtaHwnd = get_gta_window();
		if (!gtaHwnd)
			return false;
		return (GetForegroundWindow() == gtaHwnd);
	}

	bool is_key_just_pressed(int vk)
	{
		static bool s_down[256] = {};
		if (!is_gta_focused())
		{
			s_down[vk] = false;
			return false;
		}
		bool cur = (GetAsyncKeyState(vk) & 0x8000) != 0;
		bool just = cur && !s_down[vk];
		s_down[vk] = cur;
		return just;
	}

	bool is_key_down_or_repeat(int vk)
	{
		static DWORD s_lastTime[256] = {};
		static bool s_down[256] = {};
		if (!is_gta_focused())
		{
			s_down[vk] = false;
			return false;
		}
		bool cur = (GetAsyncKeyState(vk) & 0x8000) != 0;
		DWORD now = GetTickCount();

		if (cur && !s_down[vk])
		{
			s_down[vk] = true;
			s_lastTime[vk] = now + 300;
			return true;
		}
		else if (cur && s_down[vk])
		{
			if (now >= s_lastTime[vk])
			{
				s_lastTime[vk] = now + 40;
				return true;
			}
			return false;
		}
		else
		{
			s_down[vk] = false;
			return false;
		}
	}

	void phone_open()
	{
		if (g_phoneOpen)
			return;

		Ped ped = natives::PlayerPedId();

		// Put away weapon so player can hold phone cleanly
		natives::SetCurrentPedWeapon(ped, 0xA2719248 /* Unarmed */, TRUE);

		// Stop any ear phone call task if one was active
		if (natives::IsPedRunningMobilePhoneTask(ped) && natives::IsMobilePhoneToPedEar(ped))
		{
			natives::TaskUseMobilePhone(ped, FALSE, 0);
			natives::ClearPedSecondaryTask(ped);
		}

		// Spawn the in-game mobile phone.
		// In GTA V engine, CREATE_MOBILE_PHONE sets sm_display = true,
		// which causes CTaskPlayerIdles to automatically start CTaskMobilePhone(Mode_ToText),
		// attaching the official 3D phone model into Michael's hand and playing the single-handed email/text pose.
		natives::CreateMobilePhone(phone_type_for_player());

		// Failsafe: if the engine hasn't started the phone task yet, start it directly in Mode_ToText (mode 2)
		if (!natives::IsPedRunningMobilePhoneTask(ped))
		{
			natives::TaskUseMobilePhone(ped, TRUE, 2 /* Mode_ToText */);
		}

		// Activate horizontal landscape mode so Michael holds the phone sideways in both 3rd & 1st person
		natives::CellHorizontalModeToggle(TRUE);

		int camMode = get_current_cam_view_mode();
		g_lastCamMode = camMode;
		apply_phone_view_mode(camMode);

		compositor::set_phone_render_active(true);
		g_phoneOpen = true;
	}

	void phone_close()
	{
		if (!g_phoneOpen)
			return;

		Ped ped = natives::PlayerPedId();
		if (natives::IsPedRunningMobilePhoneTask(ped))
		{
			natives::TaskUseMobilePhone(ped, FALSE, 0);
		}
		natives::ClearPedSecondaryTask(ped);
		natives::SetCellCamLean(FALSE);
		natives::CellHorizontalModeToggle(FALSE);
		natives::DestroyMobilePhone();
		compositor::set_phone_render_active(false);
		compositor::send_input_event(kInResetInput, 0, 0, 0, 0);
		g_fpTracker.reset();
		g_phoneOpen = false;
	}

	bool get_client_mouse(float &normX, float &normY)
	{
		if (!is_gta_focused())
			return false;

		HWND hwnd = get_gta_window();
		if (!hwnd)
			return false;

		POINT pt;
		if (!GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt))
			return false;

		RECT rc;
		if (!GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0)
			return false;

		normX = static_cast<float>(pt.x) / static_cast<float>(rc.right);
		normY = static_cast<float>(pt.y) / static_cast<float>(rc.bottom);
		normX = (normX < 0.0f) ? 0.0f : ((normX > 1.0f) ? 1.0f : normX);
		normY = (normY < 0.0f) ? 0.0f : ((normY > 1.0f) ? 1.0f : normY);
		return true;
	}

	void set_cursor_visibility(bool visible)
	{
		static bool s_visible = false;
		if (visible == s_visible)
			return;
		s_visible = visible;
		if (visible)
			while (ShowCursor(TRUE) < 0) {}
		else
			while (ShowCursor(FALSE) >= 0) {}
	}

	void close_balatro()
	{
		compositor::send_input_event(kInResetInput, 0, 0, 0, 0);
		compositor::set_active(false);
		phone_close();
		set_cursor_visibility(false);
		natives::Notify("Balatro ~r~Closed~s~");
	}

	void draw_large_phone_tooltip(float topY, float centerX)
	{
		float bannerY = topY - 0.032f;
		if (bannerY < 0.025f) bannerY = 0.025f;

		natives::SetScriptGfxDrawOrder(7);
		natives::SetScriptGfxDrawBehindPausemenu(FALSE);

		// Translucent dark background pill
		natives::DrawRect(centerX, bannerY + 0.012f, 0.440f, 0.028f, 10, 10, 10, 190);

		// Centered text
		natives::SetTextFont(0);
		natives::SetTextScale(0.0f, 0.28f);
		natives::SetTextColour(255, 255, 255, 240);
		natives::SetTextCentre(TRUE);
		natives::BeginTextCommandDisplayText("STRING");
		natives::AddTextComponentSubstringPlayerName("Use ~y~Arrow Keys~s~ to move phone  |  Use ~b~[ & ]~s~ to adjust scale");
		natives::EndTextCommandDisplayText(centerX, bannerY);
	}

	void update_input()
	{
		if (!is_gta_focused())
			return;

		natives::InvalidateIdleCam();
		natives::InvalidateCinematicVehicleIdleMode();

		for (int c : kDisabledControls)
			natives::DisableControlAction(0, c, TRUE);

		// Normalized mouse coordinates [0..1]
		float normX = 0.5f, normY = 0.5f;
		get_client_mouse(normX, normY);

		compositor::set_mouse_pos(normX, normY);

		const float aspect = 16.0f / 9.0f;
		float mappedX = normX, mappedY = normY;
		int curCamMode = get_current_cam_view_mode();
		bool overPhone = false;

		if (g_currentMode == 2) // Mode 2: Fullscreen View
		{
			overPhone = true;
			mappedX = normX;
			mappedY = normY;
		}
		else if (g_currentMode == 1) // Mode 1: Large Phone View (F10)
		{
			float px1 = g_cfg.largeScreenX1, py1 = g_cfg.largeScreenY1;
			float px2 = g_cfg.largeScreenX2, py2 = g_cfg.largeScreenY2;

			if (g_cfg.showTooltip)
			{
				float centerX = (px1 + px2) * 0.5f;
				draw_large_phone_tooltip(py1, centerX);
			}

			// Render visual boundary frame when adjusting or when debug overlay is toggled
			DWORD now = GetTickCount();
			if (g_cfg.showDebugQuad || now < g_cfg.boundsHighlightTimer)
			{
				float cx = (px1 + px2) * 0.5f;
				float cy = (py1 + py2) * 0.5f;
				float w = (px2 - px1);
				float h = (py2 - py1);
				float thick = 0.0025f;

				natives::SetScriptGfxDrawOrder(8);
				natives::SetScriptGfxDrawBehindPausemenu(FALSE);
				// Top and bottom border lines
				natives::DrawRect(cx, py1, w, thick * aspect, 0, 220, 255, 220);
				natives::DrawRect(cx, py2, w, thick * aspect, 0, 220, 255, 220);
				// Left and right border lines
				natives::DrawRect(px1, cy, thick, h, 0, 220, 255, 220);
				natives::DrawRect(px2, cy, thick, h, 0, 220, 255, 220);
			}

			if (px2 > px1 && py2 > py1)
			{
				if (normX >= px1 && normX <= px2 && normY >= py1 && normY <= py2)
				{
					overPhone = true;
					mappedX = (normX - px1) / (px2 - px1);
					mappedY = (normY - py1) / (py2 - py1);
				}
				else
				{
					mappedX = std::clamp((normX - px1) / (px2 - px1), 0.0f, 1.0f);
					mappedY = std::clamp((normY - py1) / (py2 - py1), 0.0f, 1.0f);
				}
			}
		}
		else // Mode 0: Standard Phone View
		{
			if (curCamMode == 4) // First-person mode: calibrated screen homography
			{
				if (!g_fpHomography.valid)
				{
					g_fpHomography.compute(g_cfg.fpTL_X, g_cfg.fpTL_Y,
					                      g_cfg.fpTR_X, g_cfg.fpTR_Y,
					                      g_cfg.fpBR_X, g_cfg.fpBR_Y,
					                      g_cfg.fpBL_X, g_cfg.fpBL_Y);
				}

				DWORD now = GetTickCount();
				if (g_cfg.showDebugQuad || now < g_cfg.boundsHighlightTimer)
				{
					natives::SetScriptGfxDrawOrder(8);
					natives::SetScriptGfxDrawBehindPausemenu(FALSE);

					float curTL_X = g_cfg.fpTL_X;
					float curTL_Y = g_cfg.fpTL_Y;
					float curTR_X = g_cfg.fpTR_X;
					float curTR_Y = g_cfg.fpTR_Y;
					float curBR_X = g_cfg.fpBR_X;
					float curBR_Y = g_cfg.fpBR_Y;
					float curBL_X = g_cfg.fpBL_X;
					float curBL_Y = g_cfg.fpBL_Y;

					float thick = 0.0025f;

					// Top line: TL to TR
					float topCx = (curTL_X + curTR_X) * 0.5f;
					float topCy = (curTL_Y + curTR_Y) * 0.5f;
					float topW = (curTR_X - curTL_X);
					natives::DrawRect(topCx, topCy, topW, thick * aspect, 0, 220, 255, 220);

					// Bottom line: BL to BR
					float botCx = (curBL_X + curBR_X) * 0.5f;
					float botCy = (curBL_Y + curBR_Y) * 0.5f;
					float botW = (curBR_X - curBL_X);
					natives::DrawRect(botCx, botCy, botW, thick * aspect, 0, 220, 255, 220);

					// Left line: TL to BL
					float leftCx = (curTL_X + curBL_X) * 0.5f;
					float leftCy = (curTL_Y + curBL_Y) * 0.5f;
					float leftH = (curBL_Y - curTL_Y);
					natives::DrawRect(leftCx, leftCy, thick, leftH, 0, 220, 255, 220);

					// Right line: TR to BR
					float rightCx = (curTR_X + curBR_X) * 0.5f;
					float rightCy = (curTR_Y + curBR_Y) * 0.5f;
					float rightH = (curBR_Y - curTR_Y);
					natives::DrawRect(rightCx, rightCy, thick, rightH, 0, 220, 255, 220);

					// Corner dots (Red=TL, Green=TR, Blue=BR, Yellow=BL)
					natives::DrawRect(curTL_X, curTL_Y, 0.007f, 0.007f * aspect, 255, 30, 30, 255);
					natives::DrawRect(curTR_X, curTR_Y, 0.007f, 0.007f * aspect, 30, 255, 30, 255);
					natives::DrawRect(curBR_X, curBR_Y, 0.007f, 0.007f * aspect, 30, 120, 255, 255);
					natives::DrawRect(curBL_X, curBL_Y, 0.007f, 0.007f * aspect, 255, 255, 30, 255);
				}

				if (g_fpHomography.valid)
				{
					g_fpHomography.map(normX, normY, mappedX, mappedY, overPhone);
				}
				else
				{
					// Fallback bilinear quad
					float px1 = g_cfg.fpTL_X, px2 = g_cfg.fpTR_X;
					if (px2 > px1)
					{
						float u = (normX - px1) / (px2 - px1);
						float uClamped = std::clamp(u, 0.0f, 1.0f);
						float yTop = (1.0f - uClamped) * g_cfg.fpTL_Y + uClamped * g_cfg.fpTR_Y;
						float yBot = (1.0f - uClamped) * g_cfg.fpBL_Y + uClamped * g_cfg.fpBR_Y;

						if (normX >= px1 && normX <= px2 && normY >= yTop && normY <= yBot)
						{
							overPhone = true;
							mappedX = uClamped;
							mappedY = (normY - yTop) / (yBot - yTop);
						}
						else
						{
							mappedX = uClamped;
							mappedY = std::clamp((normY - yTop) / (yBot - yTop), 0.0f, 1.0f);
						}
					}
				}
			}
			else // Third-person mode: HUD phone screen (axis-aligned)
			{
				float px1 = g_cfg.hudX1, py1 = g_cfg.hudY1;
				float px2 = g_cfg.hudX2, py2 = g_cfg.hudY2;

				DWORD now = GetTickCount();
				if (g_cfg.showDebugQuad || now < g_cfg.boundsHighlightTimer)
				{
					float cx = (px1 + px2) * 0.5f;
					float cy = (py1 + py2) * 0.5f;
					float w = (px2 - px1);
					float h = (py2 - py1);
					float thick = 0.0025f;

					natives::SetScriptGfxDrawOrder(8);
					natives::SetScriptGfxDrawBehindPausemenu(FALSE);
					// Top and bottom border lines
					natives::DrawRect(cx, py1, w, thick * aspect, 0, 220, 255, 220);
					natives::DrawRect(cx, py2, w, thick * aspect, 0, 220, 255, 220);
					// Left and right border lines
					natives::DrawRect(px1, cy, thick, h, 0, 220, 255, 220);
					natives::DrawRect(px2, cy, thick, h, 0, 220, 255, 220);
				}

				if (px2 > px1 && py2 > py1)
				{
					if (normX >= px1 && normX <= px2 && normY >= py1 && normY <= py2)
					{
						overPhone = true;
						mappedX = (normX - px1) / (px2 - px1);
						mappedY = (normY - py1) / (py2 - py1);
					}
					else
					{
						mappedX = std::clamp((normX - px1) / (px2 - px1), 0.0f, 1.0f);
						mappedY = std::clamp((normY - py1) / (py2 - py1), 0.0f, 1.0f);
					}
				}
			}
		}

		// GTA In-game Cursor:
		// When the cursor is OUTSIDE the phone bounds (!overPhone), draw the GTA cursor so the player knows where the mouse is.
		// When the cursor moves INSIDE the phone bounds (overPhone is true), hide the GTA cursor:
		// Balatro renders its own pixel-perfect pointer directly on the phone display!
		if (!overPhone)
		{
			natives::SetScriptGfxDrawOrder(8);
			natives::SetScriptGfxDrawBehindPausemenu(FALSE);
			natives::DrawRect(normX, normY, 0.007f, 0.007f * aspect, 0, 0, 0, 255);
			natives::DrawRect(normX, normY, 0.004f, 0.004f * aspect, 255, 255, 255, 255);
		}

		// Mouse Clicks & Movements
		static bool s_lastLmb = false;
		static bool s_wasOverPhone = false;
		const bool curLmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

		if (overPhone || (curLmb && g_phoneOpen))
		{
			compositor::send_input_event(kInMouseMove, 0, mappedX, mappedY, overPhone ? 1 : 0);
			s_wasOverPhone = true;
		}
		else if (s_wasOverPhone)
		{
			// Tell Balatro the cursor left the phone so it hides the on-screen cursor
			compositor::send_input_event(kInMouseMove, 0, mappedX, mappedY, 0);
			s_wasOverPhone = false;
		}

		if (curLmb != s_lastLmb)
		{
			if (overPhone || !curLmb)
			{
				compositor::send_input_event(kInMouseButton, 1, mappedX, mappedY, curLmb ? 1 : 0);
			}
			s_lastLmb = curLmb;
		}

		static bool s_lastRmb = false;
		const bool curRmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
		if (curRmb != s_lastRmb)
		{
			if (overPhone || !curRmb)
			{
				compositor::send_input_event(kInMouseButton, 2, mappedX, mappedY, curRmb ? 1 : 0);
			}
			s_lastRmb = curRmb;
		}

		// Scroll Wheel
		if (overPhone)
		{
			if (natives::IsDisabledControlJustPressed(0, 14) || natives::IsDisabledControlJustPressed(0, 16))
				compositor::send_input_event(kInMouseWheel, 0, 0.0f, 1.0f, 0);
			if (natives::IsDisabledControlJustPressed(0, 15) || natives::IsDisabledControlJustPressed(0, 17))
				compositor::send_input_event(kInMouseWheel, 0, 0.0f, -1.0f, 0);
		}

		if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) ||
		    (GetAsyncKeyState(VK_BACK) & 0x8000) ||
		    natives::IsDisabledControlJustPressed(0, 177) ||
		    natives::IsDisabledControlJustPressed(2, 177))
		{
			close_balatro();
		}
	}

	void ScriptMain()
	{
		static bool s_wasFocused = true;
		while (true)
		{
			compositor::try_register(g_module);
			compositor::update_heartbeat();
			load_cfg_if_changed();

			bool focused = is_gta_focused();
			if (!focused)
			{
				if (s_wasFocused)
				{
					s_wasFocused = false;
					compositor::send_input_event(kInResetInput, 0, 0, 0, 0);
				}
				WAIT(100);
				continue;
			}
			s_wasFocused = true;

			// Toggle Balatro (Default: F9, configurable via [Controls] ToggleKey in ini)
			if (is_key_just_pressed(g_cfg.toggleKey))
			{
				bool nowActive = !compositor::is_active();
				compositor::set_active(nowActive);
				compositor::set_mode(g_currentMode);
				set_cursor_visibility(nowActive);

				if (nowActive)
				{
					if (!compositor::is_registered())
					{
						natives::Notify("Balatro: ~y~ReShade~s~ with add-ons is required for screen display!");
					}
					launch_balatro_if_needed();
					phone_open();
					char notifyBuf[128];
					if (g_currentMode == 2)
					{
						snprintf(notifyBuf, sizeof(notifyBuf), "Balatro: ~b~Fullscreen Mode~s~ (%s: Phone Mode)", g_cfg.modeKeyStr.c_str());
						natives::Notify(notifyBuf);
					}
					else if (g_currentMode == 1)
					{
						snprintf(notifyBuf, sizeof(notifyBuf), "Balatro: ~b~Large Phone Active~s~\nUse ~y~Arrow Keys~s~ to move | ~b~[ & ]~s~ to change scale");
						natives::Notify(notifyBuf);
					}
					else
					{
						snprintf(notifyBuf, sizeof(notifyBuf), "Balatro ~g~Active~s~ (%s/ESC: Close | %s: Cycle Modes)", g_cfg.toggleKeyStr.c_str(), g_cfg.modeKeyStr.c_str());
						natives::Notify(notifyBuf);
					}
				}
				else
				{
					close_balatro();
				}
			}

			// Cycle Standard Phone -> Large Phone -> Fullscreen (Default: F10, configurable via [Controls] ModeKey in ini)
			if (is_key_just_pressed(g_cfg.modeKey))
			{
				g_currentMode = (g_currentMode + 1) % 3;
				compositor::set_mode(g_currentMode);
				if (compositor::is_active())
				{
					int curCamMode = get_current_cam_view_mode();
					apply_phone_view_mode(curCamMode);
				}
				char notifyBuf[128];
				if (g_currentMode == 0)
				{
					natives::Notify("Balatro View: ~b~Standard Phone~s~");
				}
				else if (g_currentMode == 1)
				{
					natives::Notify("Balatro: ~b~Large Phone Active~s~\nUse ~y~Arrow Keys~s~ to move | ~b~[ & ]~s~ to change scale");
				}
				else
				{
					snprintf(notifyBuf, sizeof(notifyBuf), "Balatro: ~b~Fullscreen Mode~s~ (%s: Standard Phone)", g_cfg.modeKeyStr.c_str());
					natives::Notify(notifyBuf);
				}
			}

			// F11: Toggle Screen Quad / Bounds Debug Overlay
			if (is_key_just_pressed(VK_F11))
			{
				g_cfg.showDebugQuad = !g_cfg.showDebugQuad;
				save_cfg();
				natives::Notify(g_cfg.showDebugQuad ? "Balatro Bounds Overlay: ~g~ON~s~" : "Balatro Bounds Overlay: ~r~OFF~s~");
			}

			if (compositor::is_active())
			{
				Ped ped = natives::PlayerPedId();
				int curCamMode = get_current_cam_view_mode();
				compositor::set_cam_mode(curCamMode);

				if (curCamMode != g_lastCamMode)
				{
					g_lastCamMode = curCamMode;
					apply_phone_view_mode(curCamMode);
				}

				apply_phone_view_mode(curCamMode);

				// Ensure the ped is running the text/email phone task (Mode_ToText = 2)
				if (!natives::IsPedRunningMobilePhoneTask(ped))
				{
					natives::TaskUseMobilePhone(ped, TRUE, 2 /* Mode_ToText */);
				}
				else if (natives::IsMobilePhoneToPedEar(ped))
				{
					natives::TaskUseMobilePhone(ped, FALSE, 0);
				}

				// Keep horizontal mode active so the ped holds the physical phone sideways in both views
				natives::CellHorizontalModeToggle(TRUE);

				// Force GTA V to bind the phone render target each tick so our ReShade hook copies Balatro to it
				int phoneRenderId = -1;
				natives::GetMobilePhoneRenderId(&phoneRenderId);
				if (phoneRenderId != -1)
				{
					natives::SetTextRenderId(phoneRenderId);
					natives::SetScriptGfxDrawOrder(4);
					natives::SetScriptGfxDrawBehindPausemenu(TRUE);
					natives::DrawRect(0.5f, 0.5f, 1.0f, 1.0f, 0, 0, 0, 1); // Alpha 1 (0.4% opacity): forces engine to bind RT while completely invisible
					natives::SetTextRenderId(natives::GetDefaultScriptRendertargetRenderId());
					compositor::set_phone_render_active(true);
				}
				else
				{
					compositor::set_phone_render_active(false);
				}

				// Real-time adjustment and calibration controls
				bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

				if (g_currentMode == 1) // Mode 1: Large Phone View (F10)
				{
					if (ctrlDown)
					{
						// Ctrl + Arrow Keys / [ & ]: Fine-tune 2D input bounds directly
						bool boundsAdjusted = false;
						if (is_key_down_or_repeat(VK_LEFT) || is_key_down_or_repeat(VK_NUMPAD4))
						{
							g_cfg.largeScreenX1 -= 0.003f;
							g_cfg.largeScreenX2 -= 0.003f;
							boundsAdjusted = true;
						}
						else if (is_key_down_or_repeat(VK_RIGHT) || is_key_down_or_repeat(VK_NUMPAD6))
						{
							g_cfg.largeScreenX1 += 0.003f;
							g_cfg.largeScreenX2 += 0.003f;
							boundsAdjusted = true;
						}
						else if (is_key_down_or_repeat(VK_UP) || is_key_down_or_repeat(VK_NUMPAD8))
						{
							g_cfg.largeScreenY1 -= 0.003f;
							g_cfg.largeScreenY2 -= 0.003f;
							boundsAdjusted = true;
						}
						else if (is_key_down_or_repeat(VK_DOWN) || is_key_down_or_repeat(VK_NUMPAD2))
						{
							g_cfg.largeScreenY1 += 0.003f;
							g_cfg.largeScreenY2 += 0.003f;
							boundsAdjusted = true;
						}
						else if (is_key_down_or_repeat(VK_OEM_6) || is_key_down_or_repeat(VK_OEM_PLUS) ||
						         is_key_down_or_repeat(VK_PRIOR) || is_key_down_or_repeat(VK_ADD))
						{
							g_cfg.largeScreenX1 -= 0.003f;
							g_cfg.largeScreenX2 += 0.003f;
							g_cfg.largeScreenY1 -= 0.003f;
							g_cfg.largeScreenY2 += 0.003f;
							boundsAdjusted = true;
						}
						else if (is_key_down_or_repeat(VK_OEM_4) || is_key_down_or_repeat(VK_OEM_MINUS) ||
						         is_key_down_or_repeat(VK_NEXT) || is_key_down_or_repeat(VK_SUBTRACT))
						{
							g_cfg.largeScreenX1 += 0.003f;
							g_cfg.largeScreenX2 -= 0.003f;
							g_cfg.largeScreenY1 += 0.003f;
							g_cfg.largeScreenY2 += 0.003f;
							boundsAdjusted = true;
						}

						if (boundsAdjusted)
						{
							g_cfg.boundsHighlightTimer = GetTickCount() + 3000;
							compositor::set_phone_rect(g_cfg.largeScreenX1, g_cfg.largeScreenY1, g_cfg.largeScreenX2, g_cfg.largeScreenY2);
							save_cfg();
							char boundsMsg[128];
							snprintf(boundsMsg, sizeof(boundsMsg), "Large Input: X=[~y~%.3f..%.3f~s~] Y=[~y~%.3f..%.3f~s~]",
								g_cfg.largeScreenX1, g_cfg.largeScreenX2, g_cfg.largeScreenY1, g_cfg.largeScreenY2);
							natives::Notify(boundsMsg);
						}
					}
					else
					{
						// Standard Arrow Keys & [ / ]: Move phone model and adjust scale
						bool adjusted = false;

						// Scale: [ / ], = / -, PageUp / PageDown, or Numpad +/-
						if (is_key_down_or_repeat(VK_OEM_6) || is_key_down_or_repeat(VK_OEM_PLUS) ||
						    is_key_down_or_repeat(VK_PRIOR) || is_key_down_or_repeat(VK_ADD))
						{
							g_cfg.largeScale += 15.0f;
							if (g_cfg.largeScale > 1100.0f) g_cfg.largeScale = 1100.0f;
							adjusted = true;
						}
						else if (is_key_down_or_repeat(VK_OEM_4) || is_key_down_or_repeat(VK_OEM_MINUS) ||
						         is_key_down_or_repeat(VK_NEXT) || is_key_down_or_repeat(VK_SUBTRACT))
						{
							g_cfg.largeScale -= 15.0f;
							if (g_cfg.largeScale < 400.0f) g_cfg.largeScale = 400.0f;
							adjusted = true;
						}

						// Up / Down: move phone vertically on screen (PosY)
						if (is_key_down_or_repeat(VK_UP) || is_key_down_or_repeat(VK_NUMPAD8))
						{
							g_cfg.largePosY += 1.0f;
							adjusted = true;
						}
						else if (is_key_down_or_repeat(VK_DOWN) || is_key_down_or_repeat(VK_NUMPAD2))
						{
							g_cfg.largePosY -= 1.0f;
							adjusted = true;
						}
						// Left / Right: move phone horizontally on screen (PosX)
						else if (is_key_down_or_repeat(VK_LEFT) || is_key_down_or_repeat(VK_NUMPAD4))
						{
							g_cfg.largePosX -= 2.0f;
							adjusted = true;
						}
						else if (is_key_down_or_repeat(VK_RIGHT) || is_key_down_or_repeat(VK_NUMPAD6))
						{
							g_cfg.largePosX += 2.0f;
							adjusted = true;
						}

						if (adjusted)
						{
							recompute_large_phone_bounds();
							g_cfg.boundsHighlightTimer = GetTickCount() + 2500;

							int curCamMode = get_current_cam_view_mode();
							apply_phone_view_mode(curCamMode);
							save_cfg();

							char posMsg[128];
							snprintf(posMsg, sizeof(posMsg), "Large Phone: Scale=~b~%.0f~s~ Pos=(X:~y~%.1f~s~, Y:~y~%.1f~s~)",
								g_cfg.largeScale, g_cfg.largePosX, g_cfg.largePosY);
							natives::Notify(posMsg);
						}
					}
				}
				else if (g_currentMode == 0 && curCamMode == 4 && ctrlDown) // Mode 0 First-Person Quad Calibration
				{
					bool fpAdjusted = false;
					if (is_key_down_or_repeat(VK_LEFT) || is_key_down_or_repeat(VK_NUMPAD4))
					{
						g_cfg.fpTL_X -= 0.002f; g_cfg.fpTR_X -= 0.002f;
						g_cfg.fpBR_X -= 0.002f; g_cfg.fpBL_X -= 0.002f;
						fpAdjusted = true;
					}
					else if (is_key_down_or_repeat(VK_RIGHT) || is_key_down_or_repeat(VK_NUMPAD6))
					{
						g_cfg.fpTL_X += 0.002f; g_cfg.fpTR_X += 0.002f;
						g_cfg.fpBR_X += 0.002f; g_cfg.fpBL_X += 0.002f;
						fpAdjusted = true;
					}
					else if (is_key_down_or_repeat(VK_UP) || is_key_down_or_repeat(VK_NUMPAD8))
					{
						g_cfg.fpTL_Y -= 0.002f; g_cfg.fpTR_Y -= 0.002f;
						g_cfg.fpBR_Y -= 0.002f; g_cfg.fpBL_Y -= 0.002f;
						fpAdjusted = true;
					}
					else if (is_key_down_or_repeat(VK_DOWN) || is_key_down_or_repeat(VK_NUMPAD2))
					{
						g_cfg.fpTL_Y += 0.002f; g_cfg.fpTR_Y += 0.002f;
						g_cfg.fpBR_Y += 0.002f; g_cfg.fpBL_Y += 0.002f;
						fpAdjusted = true;
					}
					else if (is_key_down_or_repeat(VK_OEM_6) || is_key_down_or_repeat(VK_OEM_PLUS) ||
					         is_key_down_or_repeat(VK_PRIOR) || is_key_down_or_repeat(VK_ADD))
					{
						float cx = (g_cfg.fpTL_X + g_cfg.fpTR_X + g_cfg.fpBR_X + g_cfg.fpBL_X) * 0.25f;
						float cy = (g_cfg.fpTL_Y + g_cfg.fpTR_Y + g_cfg.fpBR_Y + g_cfg.fpBL_Y) * 0.25f;
						float factor = 1.01f;
						g_cfg.fpTL_X = cx + (g_cfg.fpTL_X - cx) * factor;
						g_cfg.fpTL_Y = cy + (g_cfg.fpTL_Y - cy) * factor;
						g_cfg.fpTR_X = cx + (g_cfg.fpTR_X - cx) * factor;
						g_cfg.fpTR_Y = cy + (g_cfg.fpTR_Y - cy) * factor;
						g_cfg.fpBR_X = cx + (g_cfg.fpBR_X - cx) * factor;
						g_cfg.fpBR_Y = cy + (g_cfg.fpBR_Y - cy) * factor;
						g_cfg.fpBL_X = cx + (g_cfg.fpBL_X - cx) * factor;
						g_cfg.fpBL_Y = cy + (g_cfg.fpBL_Y - cy) * factor;
						fpAdjusted = true;
					}
					else if (is_key_down_or_repeat(VK_OEM_4) || is_key_down_or_repeat(VK_OEM_MINUS) ||
					         is_key_down_or_repeat(VK_NEXT) || is_key_down_or_repeat(VK_SUBTRACT))
					{
						float cx = (g_cfg.fpTL_X + g_cfg.fpTR_X + g_cfg.fpBR_X + g_cfg.fpBL_X) * 0.25f;
						float cy = (g_cfg.fpTL_Y + g_cfg.fpTR_Y + g_cfg.fpBR_Y + g_cfg.fpBL_Y) * 0.25f;
						float factor = 0.99f;
						g_cfg.fpTL_X = cx + (g_cfg.fpTL_X - cx) * factor;
						g_cfg.fpTL_Y = cy + (g_cfg.fpTL_Y - cy) * factor;
						g_cfg.fpTR_X = cx + (g_cfg.fpTR_X - cx) * factor;
						g_cfg.fpTR_Y = cy + (g_cfg.fpTR_Y - cy) * factor;
						g_cfg.fpBR_X = cx + (g_cfg.fpBR_X - cx) * factor;
						g_cfg.fpBR_Y = cy + (g_cfg.fpBR_Y - cy) * factor;
						g_cfg.fpBL_X = cx + (g_cfg.fpBL_X - cx) * factor;
						g_cfg.fpBL_Y = cy + (g_cfg.fpBL_Y - cy) * factor;
						fpAdjusted = true;
					}

					if (fpAdjusted)
					{
						g_fpHomography.compute(g_cfg.fpTL_X, g_cfg.fpTL_Y,
						                      g_cfg.fpTR_X, g_cfg.fpTR_Y,
						                      g_cfg.fpBR_X, g_cfg.fpBR_Y,
						                      g_cfg.fpBL_X, g_cfg.fpBL_Y);
						compositor::set_phone_rect(g_cfg.fpTL_X, g_cfg.fpTL_Y, g_cfg.fpTR_X, g_cfg.fpBR_Y);
						g_cfg.boundsHighlightTimer = GetTickCount() + 3000;
						save_cfg();
						natives::Notify("Balatro: ~g~1st-Person Quad Calibrated~s~");
					}
				}
				else if (g_currentMode == 0 && curCamMode != 4 && ctrlDown) // Mode 0 Third-Person HUD Calibration
				{
					bool hudAdjusted = false;
					if (is_key_down_or_repeat(VK_LEFT) || is_key_down_or_repeat(VK_NUMPAD4))
					{
						g_cfg.hudX1 -= 0.002f; g_cfg.hudX2 -= 0.002f;
						hudAdjusted = true;
					}
					else if (is_key_down_or_repeat(VK_RIGHT) || is_key_down_or_repeat(VK_NUMPAD6))
					{
						g_cfg.hudX1 += 0.002f; g_cfg.hudX2 += 0.002f;
						hudAdjusted = true;
					}
					else if (is_key_down_or_repeat(VK_UP) || is_key_down_or_repeat(VK_NUMPAD8))
					{
						g_cfg.hudY1 -= 0.002f; g_cfg.hudY2 -= 0.002f;
						hudAdjusted = true;
					}
					else if (is_key_down_or_repeat(VK_DOWN) || is_key_down_or_repeat(VK_NUMPAD2))
					{
						g_cfg.hudY1 += 0.002f; g_cfg.hudY2 += 0.002f;
						hudAdjusted = true;
					}
					else if (is_key_down_or_repeat(VK_OEM_6) || is_key_down_or_repeat(VK_OEM_PLUS) ||
					         is_key_down_or_repeat(VK_PRIOR) || is_key_down_or_repeat(VK_ADD))
					{
						g_cfg.hudX1 -= 0.002f; g_cfg.hudX2 += 0.002f;
						g_cfg.hudY1 -= 0.002f; g_cfg.hudY2 += 0.002f;
						hudAdjusted = true;
					}
					else if (is_key_down_or_repeat(VK_OEM_4) || is_key_down_or_repeat(VK_OEM_MINUS) ||
					         is_key_down_or_repeat(VK_NEXT) || is_key_down_or_repeat(VK_SUBTRACT))
					{
						g_cfg.hudX1 += 0.002f; g_cfg.hudX2 -= 0.002f;
						g_cfg.hudY1 += 0.002f; g_cfg.hudY2 -= 0.002f;
						hudAdjusted = true;
					}

					if (hudAdjusted)
					{
						compositor::set_phone_rect(g_cfg.hudX1, g_cfg.hudY1, g_cfg.hudX2, g_cfg.hudY2);
						g_cfg.boundsHighlightTimer = GetTickCount() + 3000;
						save_cfg();
						char boundsMsg[128];
						snprintf(boundsMsg, sizeof(boundsMsg), "3rd HUD Input: X=[~y~%.3f..%.3f~s~] Y=[~y~%.3f..%.3f~s~]",
							g_cfg.hudX1, g_cfg.hudX2, g_cfg.hudY1, g_cfg.hudY2);
						natives::Notify(boundsMsg);
					}
				}

				update_input();
			}

			WAIT(0);
		}
	}
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD dwReason, LPVOID)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hModule);
		g_module = hModule;
		compositor::try_register(hModule);
		scriptRegister(hModule, ScriptMain);
		break;
	case DLL_PROCESS_DETACH:
		scriptUnregister(hModule);
		compositor::unregister(hModule);
		break;
	}
	return TRUE;
}

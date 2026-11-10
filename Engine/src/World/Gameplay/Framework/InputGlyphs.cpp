#include "wldpch.h"
#include "World/Gameplay/Framework/InputGlyphs.h"

namespace World::Gameplay
{
	std::string InputGlyphs::GetGlyphText(InputDevice device, int code)
	{
		if (device == InputDevice::Key)
		{
			if (code >= 65 && code <= 90) return std::string(1, static_cast<char>(code));
			if (code >= 48 && code <= 57) return std::string(1, static_cast<char>(code));
			if (code == 32) return "Space";
			if (code == 256) return "Esc";
			if (code == 257) return "Enter";
			if (code == 258) return "Tab";
			if (code == 259) return "Backspace";
			if (code == 340 || code == 344) return "Shift";
			if (code == 341 || code == 345) return "Ctrl";
			if (code == 342 || code == 346) return "Alt";
			return "Key " + std::to_string(code);
		}
		else if (device == InputDevice::Mouse)
		{
			if (code == 0) return "LMB";
			if (code == 1) return "RMB";
			if (code == 2) return "MMB";
			return "Mouse " + std::to_string(code);
		}
		else if (device == InputDevice::Gamepad)
		{
			if (code == 0) return "A";
			if (code == 1) return "B";
			if (code == 2) return "X";
			if (code == 3) return "Y";
			if (code == 4) return "LB";
			if (code == 5) return "RB";
			if (code == 6) return "Back";
			if (code == 7) return "Start";
			return "Pad " + std::to_string(code);
		}
		return "Unknown";
	}

	std::string InputGlyphs::GetGlyphIconPath(InputDevice device, int code)
	{
		std::string prefix = "textures/glyphs/";
		if (device == InputDevice::Gamepad)
			return prefix + "pad_" + std::to_string(code) + ".png";
		if (device == InputDevice::Mouse)
			return prefix + "mouse_" + std::to_string(code) + ".png";
		return prefix + "key_" + std::to_string(code) + ".png";
	}
}

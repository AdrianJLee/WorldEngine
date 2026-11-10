#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/Framework/InputTypes.h"
#include <string>

namespace World::Gameplay
{
	class WLD_API InputGlyphs
	{
	public:
		static std::string GetGlyphText(InputDevice device, int code);
		static std::string GetGlyphIconPath(InputDevice device, int code);
	};
}

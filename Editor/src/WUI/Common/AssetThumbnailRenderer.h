#pragma once

#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiWidgets.h"
#include "World/Asset/AssetThumbnailCache.h"

#include <string>

namespace World::Editor
{
	class AssetThumbnailRenderer
	{
	public:
		static void DrawThumbnail(Wui::WuiContext& ctx, const Wui::WuiRect& rect,
			const std::string& logicalPath, AssetCategory category, const Wui::WuiTheme& theme);
	};
}

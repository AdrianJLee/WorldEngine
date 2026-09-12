#pragma once

#include "World/WUI/WuiBackend.h"

struct ImFont;

namespace World::Wui
{
	// Backend v1:ImGui 只作为绘制与输入来源;不含任何业务/布局逻辑。
	class WuiImGuiBackend final : public WuiBackend
	{
	public:
		void SetFonts(ImFont* regular, ImFont* bold, ImFont* cjk);

		bool BeginFrame(WuiInputState& input) override;
		void Render(const std::vector<WuiDrawCommand>& commands) override;
		void EndFrame(WuiCursor cursor) override;

	private:
		ImFont* m_Regular = nullptr;
		ImFont* m_Bold = nullptr;
		ImFont* m_Cjk = nullptr;
		WuiCursor m_PendingCursor = WuiCursor::Arrow;
	};
}

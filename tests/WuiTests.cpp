#include "World/WUI/WuiCore.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace World::Wui;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	bool Near(float a, float b)
	{
		const float delta = a - b;
		return delta > -0.001f && delta < 0.001f;
	}
}

int main()
{
	try
	{
		// 1. Flex 行布局:等 grow 平分剩余空间
		{
			const WuiFlexResult result = SolveFlex(
				{ WuiDirection::Row, WuiAlign::Start, WuiAlign::Start, 0 },
				{ { 50, 100, 0, 20, 1 }, { 50, 100, 0, 20, 1 } },
				{ 0, 0, 300, 20 });
			CHECK(result.Rects.size() == 2);
			CHECK(Near(result.Rects[0].X, 0) && Near(result.Rects[0].W, 150));
			CHECK(Near(result.Rects[1].X, 150) && Near(result.Rects[1].W, 150));
			CHECK(Near(result.MainSize, 300));
			CHECK(Near(result.CrossSize, 20));
		}

		// 2. Gap 参与可用空间计算
		{
			const WuiFlexResult result = SolveFlex(
				{ WuiDirection::Row, WuiAlign::Start, WuiAlign::Start, 10 },
				{ { 50, 100, 0, 20, 1 }, { 50, 100, 0, 20, 1 } },
				{ 0, 0, 300, 20 });
			CHECK(Near(result.Rects[1].X, 155));
			CHECK(Near(result.MainSize, 300));
		}

		// 3. 无界容器取内容大小;列方向交换主轴
		{
			const WuiFlexResult result = SolveFlex(
				{ WuiDirection::Column, WuiAlign::Start, WuiAlign::Start, 4 },
				{ { 20, 40, 10, 30, 0 }, { 30, 60, 10, 30, 0 } },
				{ 0, 0, 100, 1e30f });
			CHECK(result.Rects.size() == 2);
			CHECK(Near(result.Rects[0].Y, 0) && Near(result.Rects[0].H, 40));
			CHECK(Near(result.Rects[1].Y, 44) && Near(result.Rects[1].H, 60));
			CHECK(Near(result.MainSize, 104));
			CHECK(Near(result.CrossSize, 100)); // 有界交叉轴以容器为准
		}

		// 4. 交叉轴对齐:Center / Stretch
		{
			WuiFlexResult result = SolveFlex(
				{ WuiDirection::Row, WuiAlign::Start, WuiAlign::Center, 0 },
				{ { 0, 80, 0, 30, 0 } },
				{ 0, 0, 200, 60 });
			CHECK(Near(result.Rects[0].Y, 15));

			result = SolveFlex(
				{ WuiDirection::Row, WuiAlign::Start, WuiAlign::Stretch, 0 },
				{ { 0, 80, 0, 30, 0 } },
				{ 0, 0, 200, 60 });
			CHECK(Near(result.Rects[0].Y, 0) && Near(result.Rects[0].H, 60));
		}

		// 5. 样式级联:后规则覆盖、类选择器、父样式继承
		{
			WuiStyleSheet sheet;
			sheet.AddRule("Panel", { {}, WuiColor { 0.1f, 0.2f, 0.3f, 1 }, {}, 12.0f, 4.0f, 4.0f });
			sheet.AddRule(".primary", { {}, WuiColor { 0.9f, 0.1f, 0.1f, 1 } });
			const WuiStyle base;
			const WuiStyle regular = sheet.Resolve(base, "Panel", {});
			CHECK(regular.FillColor.has_value() && Near(regular.FillColor->B, 0.3f));
			CHECK(regular.FontSize.has_value() && Near(*regular.FontSize, 12.0f));
			const WuiStyle primary = sheet.Resolve(base, "Panel", { "primary" });
			CHECK(primary.FillColor.has_value() && Near(primary.FillColor->R, 0.9f));
			CHECK(primary.FontSize.has_value() && Near(*primary.FontSize, 12.0f));
		}

		// 6. 命中测试
		{
			const WuiRect rect { 10, 10, 100, 50 };
			CHECK(HitTest(rect, { 10, 10 }));
			CHECK(HitTest(rect, { 109.9f, 59.9f }));
			CHECK(!HitTest(rect, { 110.1f, 60.1f }));
			CHECK(!HitTest(rect, { 9.9f, 30 }));
		}

		// 7. 焦点遍历:顺序、回绕、未知 id
		{
			const std::vector<WuiId> order = { 1, 2, 3 };
			CHECK(NextFocus(order, 1, false).value() == 2);
			CHECK(NextFocus(order, 3, false).value() == 1);
			CHECK(NextFocus(order, 1, true).value() == 3);
			CHECK(NextFocus(order, 99, false).value() == 1);
			CHECK(!NextFocus({}, 1, false).has_value());
		}

		std::printf("World.Wui: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Wui: FAILED: %s\n", error.what());
		return 1;
	}
}

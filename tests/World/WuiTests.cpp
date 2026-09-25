#include "World/WUI/WuiCore.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiComponentRegistry.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiOperationLog.h"
#include "World/WUI/WuiUndoStack.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/WuiDock.h"
#include "World/WUI/WuiJson.h"
#include "World/WUI/WuiLayoutStore.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiScriptedInput.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/WuiCodeEditor.h"
#include "World/WUI/Widgets/WuiControls.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/Core/KeyCodes.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

	// 码点序列字面量(CHECK 是宏,直接写 vector{...} 的花括号不会保护其中的逗号)。
	std::vector<uint32_t> Codepoints(std::initializer_list<uint32_t> values)
	{
		return std::vector<uint32_t>(values);
	}

	bool Near(float a, float b)
	{
		const float delta = a - b;
		return delta > -0.001f && delta < 0.001f;
	}

	// WUI-P1.5a:命令流哈希 —— "画布像素哈希"的 headless 等价物:逐命令把影响像素的字段喂进
	// FNV-1a,任何一处颜色/矩形/字号/粗体/文本变化都会改哈希(真实像素证据由探针 matrix 阶段给)。
	uint64_t CommandStreamHash(const std::vector<WuiDrawCommand>& commands)
	{
		uint32_t hash = 2166136261u;
		const auto feed = [&hash](const void* data, size_t size)
		{
			const uint8_t* bytes = static_cast<const uint8_t*>(data);
			for (size_t index = 0; index < size; ++index)
				hash = (hash ^ bytes[index]) * 16777619u;
		};
		for (const WuiDrawCommand& command : commands)
		{
			feed(&command.Kind, sizeof(command.Kind));
			feed(&command.Rect, sizeof(command.Rect));
			feed(&command.Color, sizeof(command.Color));
			feed(&command.Rounding, sizeof(command.Rounding));
			feed(&command.Thickness, sizeof(command.Thickness));
			feed(command.Text.data(), command.Text.size());
			feed(&command.FontSize, sizeof(command.FontSize));
			feed(&command.Bold, sizeof(command.Bold));
			feed(&command.Image, sizeof(command.Image));
		}
		return hash;
	}

	// WUI-P1.5a2:把一条按钮命令流压成"逐字段可比"的投影 —— 保留模式 WuiButton 与立即模式 Button
	// 的差异清单就是这些字段(而不是"看起来差不多");任何新增的意外差异都会让 §28 失败。
	struct ButtonPaintFields
	{
		size_t FillCount = 0;
		WuiColor Fill {};
		float FillRounding = 0.0f;
		bool BorderDrawn = false;
		WuiColor Border {};
		float BorderRounding = 0.0f;
		float BorderThickness = 0.0f;
		bool TextDrawn = false;
		WuiColor Text {};
		float TextX = 0.0f;
		float TextY = 0.0f;
		float FontSize = 0.0f;
		bool Bold = false;
		bool RingDrawn = false;
		WuiColor Ring {};
		float RingRounding = 0.0f;
	};

	bool SameColor(const WuiColor& a, const WuiColor& b)
	{
		return Near(a.R, b.R) && Near(a.G, b.G) && Near(a.B, b.B) && Near(a.A, b.A);
	}

	// MAT-UI3a:焦点环从"不透明硬描边"改成"基色 × 低透明度"的两笔(主环 alpha × 0.72
	// + 外发光 alpha × 0.16,见 WuiWidgets.h DrawFocusRing)。这里的换算必须与控件同一口径,
	// 否则"两面共用同一份 DrawFocusRing"的断言会被颜色漂移掩盖。
	WuiColor DimmedRing(const WuiColor& base, float alphaScale)
	{
		return WuiColor { base.R, base.G, base.B, base.A * alphaScale };
	}

	ButtonPaintFields ProjectButtonPaint(const std::vector<WuiDrawCommand>& commands,
		const std::vector<WuiDrawCommand>& overlay)
	{
		ButtonPaintFields fields;
		for (const WuiDrawCommand& command : commands)
		{
			if (command.Kind == WuiDrawKind::Rect)
			{
				if (fields.FillCount == 0)
				{
					fields.Fill = command.Color;
					fields.FillRounding = command.Rounding;
				}
				++fields.FillCount;
			}
			else if (command.Kind == WuiDrawKind::RectOutline)
			{
				fields.BorderDrawn = true;
				fields.Border = command.Color;
				fields.BorderRounding = command.Rounding;
				fields.BorderThickness = command.Thickness;
			}
			else if (command.Kind == WuiDrawKind::Text)
			{
				fields.TextDrawn = true;
				fields.Text = command.Color;
				fields.TextX = command.Rect.X;
				fields.TextY = command.Rect.Y;
				fields.FontSize = command.FontSize;
				fields.Bold = command.Bold;
			}
		}
		// 焦点环走 overlay 层(与 tooltip 同一机制)。
		for (const WuiDrawCommand& command : overlay)
		{
			if (command.Kind != WuiDrawKind::RectOutline)
				continue;
			fields.RingDrawn = true;
			fields.Ring = command.Color;
			fields.RingRounding = command.Rounding;
			break;
		}
		return fields;
	}

	std::vector<std::string> DiffButtonPaint(const ButtonPaintFields& retained, const ButtonPaintFields& immediate)
	{
		std::vector<std::string> deltas;
		const auto note = [&deltas](bool differs, const char* name)
		{
			if (differs)
				deltas.emplace_back(name);
		};
		note(retained.FillCount != immediate.FillCount, "fill.count");
		note(!SameColor(retained.Fill, immediate.Fill), "fill.color");
		note(!Near(retained.FillRounding, immediate.FillRounding), "fill.rounding");
		note(retained.BorderDrawn != immediate.BorderDrawn, "border.drawn");
		const bool borders = retained.BorderDrawn && immediate.BorderDrawn;
		note(borders && !SameColor(retained.Border, immediate.Border), "border.color");
		note(borders && !Near(retained.BorderRounding, immediate.BorderRounding), "border.rounding");
		note(borders && !Near(retained.BorderThickness, immediate.BorderThickness), "border.thickness");
		note(retained.TextDrawn != immediate.TextDrawn, "text.drawn");
		note(!SameColor(retained.Text, immediate.Text), "text.color");
		note(!Near(retained.TextX, immediate.TextX), "text.x");
		note(!Near(retained.TextY, immediate.TextY), "text.y");
		note(!Near(retained.FontSize, immediate.FontSize), "fontSize");
		note(retained.Bold != immediate.Bold, "bold");
		note(retained.RingDrawn != immediate.RingDrawn, "ring.drawn");
		const bool rings = retained.RingDrawn && immediate.RingDrawn;
		note(rings && !SameColor(retained.Ring, immediate.Ring), "ring.color");
		note(rings && !Near(retained.RingRounding, immediate.RingRounding), "ring.rounding");
		return deltas;
	}

	// 差异清单不符时把**实际**清单打出来(只看 CHECK 表达式看不出来差在哪)。
	void ExpectDeltas(const std::vector<std::string>& actual, const std::vector<std::string>& expected, int line)
	{
		if (actual == expected)
			return;
		std::string message = "line " + std::to_string(line) + ": two-face style deltas = [";
		for (size_t index = 0; index < actual.size(); ++index)
			message += (index == 0 ? "" : ",") + actual[index];
		message += "] expected [";
		for (size_t index = 0; index < expected.size(); ++index)
			message += (index == 0 ? "" : ",") + expected[index];
		message += "]";
		throw std::runtime_error(message);
	}

	// "除圆角外逐字节相同"的判据:同 CommandStreamHash,但不喂 Rounding(圆角是两面唯一记在案的残差);
	// 圆角差异单独计数,避免"把残差也哈希进去"导致判据永远不成立。
	uint64_t CommandStreamHashNoRounding(const std::vector<WuiDrawCommand>& commands)
	{
		uint32_t hash = 2166136261u;
		const auto feed = [&hash](const void* data, size_t size)
		{
			const uint8_t* bytes = static_cast<const uint8_t*>(data);
			for (size_t index = 0; index < size; ++index)
				hash = (hash ^ bytes[index]) * 16777619u;
		};
		for (const WuiDrawCommand& command : commands)
		{
			feed(&command.Kind, sizeof(command.Kind));
			feed(&command.Rect, sizeof(command.Rect));
			feed(&command.Color, sizeof(command.Color));
			feed(&command.Thickness, sizeof(command.Thickness));
			feed(command.Text.data(), command.Text.size());
			feed(&command.FontSize, sizeof(command.FontSize));
			feed(&command.Bold, sizeof(command.Bold));
			feed(&command.Image, sizeof(command.Image));
		}
		return hash;
	}

	size_t CountRoundingDiffs(const std::vector<WuiDrawCommand>& a, const std::vector<WuiDrawCommand>& b)
	{
		if (a.size() != b.size())
			return static_cast<size_t>(-1);
		size_t count = 0;
		for (size_t index = 0; index < a.size(); ++index)
			if (!Near(a[index].Rounding, b[index].Rounding))
				++count;
		return count;
	}

	// P1a:登记的 TypeName 必须真的能在 WUI 头文件里找到声明 —— 保留模式控件是 struct/class
	// (如 "WuiButton"),纯立即模式控件是头文件里的入口函数(如 "Segmented")。名字不能是编出来的。
	// 头文件文本从仓库根读(ctest 的 WORKING_DIRECTORY 就是 ${CMAKE_SOURCE_DIR});读不到时再按
	// __FILE__ 反推,兼容直接从 build 目录手跑。
	std::vector<std::string> LoadWuiHeaderTexts()
	{
		std::vector<std::string> texts;
		const std::filesystem::path candidates[] = {
			std::filesystem::current_path() / "Engine/src/World/WUI",
			std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "Engine/src/World/WUI",
		};
		for (const std::filesystem::path& dir : candidates)
		{
			if (!std::filesystem::exists(dir))
				continue;
			for (const std::filesystem::directory_entry& file : std::filesystem::recursive_directory_iterator(dir))
			{
				if (!file.is_regular_file() || file.path().extension() != ".h")
					continue;
				std::ifstream stream(file.path(), std::ios::binary);
				if (!stream)
					continue;
				std::stringstream buffer;
				buffer << stream.rdbuf();
				texts.push_back(buffer.str());
			}
			if (!texts.empty())
				break;
		}
		return texts;
	}

	bool DeclaredInWuiHeaders(const std::vector<std::string>& texts, const std::string& name)
	{
		const auto isWordChar = [](char ch)
		{
			return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
		};
		const auto isBlank = [](char ch) { return ch == ' ' || ch == '\t'; };
		for (const std::string& text : texts)
		{
			size_t at = text.find(name);
			while (at != std::string::npos)
			{
				const size_t end = at + name.size();
				const bool leftOk = at == 0 || !isWordChar(text[at - 1]);
				const bool rightOk = end >= text.size() || !isWordChar(text[end]);
				if (leftOk && rightOk)
				{
					size_t before = at;
					while (before > 0 && isBlank(text[before - 1]))
						--before;
					const bool asType = (before >= 6 && text.compare(before - 6, 6, "struct") == 0)
						|| (before >= 5 && text.compare(before - 5, 5, "class") == 0);
					size_t after = end;
					while (after < text.size() && isBlank(text[after]))
						++after;
					const bool asEntry = after < text.size() && text[after] == '(';
					if (asType || asEntry)
						return true;
				}
				at = text.find(name, at + 1);
			}
		}
		return false;
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

		// 8. 停靠布局:AddTab 形状、RemoveTab 塌缩、序列化往返
		{
			DockLayout layout = DockLayout::Default({ "hierarchy", "properties", "content_browser", "view", "stats", "memory" });
			CHECK(layout.Contains("view"));
			CHECK(layout.AddTab("inspector", "view", DropZone::Center));
			CHECK(layout.IsActive("inspector"));
			CHECK(layout.AddTab("console", "view", DropZone::Right));
			CHECK(layout.Contains("console"));

			// 序列化往返内容一致
			const std::string serialized = layout.Serialize();
			DockLayout restored;
			std::string error;
			CHECK(DockLayout::Deserialize(serialized, &restored, &error));
			CHECK(restored.Serialize() == serialized);
			CHECK(restored.Contains("inspector") && restored.Contains("console"));

			// 删除最后一个 tab 后空组塌缩;重复删除失败
			DockLayout single = DockLayout::Default({ "view" });
			CHECK(single.RemoveTab("view"));
			CHECK(!single.Contains("view"));
			CHECK(!single.RemoveTab("view"));

			// 移动已存在面板:摘除 → 重新挂载到目标组
			DockLayout moving = DockLayout::Default({ "hierarchy", "properties", "view" });
			CHECK(moving.FindSibling("hierarchy") == "properties");
			CHECK(moving.FirstPanel() == "hierarchy");
			CHECK(moving.MoveTab("view", "hierarchy", DropZone::Center));
			CHECK(moving.IsActive("view"));
			CHECK(moving.FindSibling("view") == "hierarchy" || moving.FindSibling("view") == "properties");
		}

		// 9. 布局文件持久化:写读往返与缺失回退
		{
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "worldengine-wui-layout.json";
			DockLayout layout = DockLayout::Default({ "view", "stats" });
			std::string error;
			CHECK(WuiLayoutStore::Save(path, layout, &error));
			DockLayout loaded;
			CHECK(WuiLayoutStore::Load(path, layout, &loaded, &error));
			CHECK(loaded.Contains("view") && loaded.Contains("stats"));
			std::error_code ignored;
			std::filesystem::remove(path, ignored);

			DockLayout fallback = DockLayout::Default({ "fallback_only" });
			DockLayout missing;
			CHECK(WuiLayoutStore::Load(path, fallback, &missing, &error));
			CHECK(missing.Contains("fallback_only"));
		}

		// 9b. 停靠矩形切分:两级 split 按比例分区
		{
			DockLayout layout = DockLayout::Default({ "hierarchy", "properties", "content_browser", "view", "stats", "memory" });
			std::vector<std::pair<PanelId, WuiRect>> rects;
			layout.ComputeRects({ 0, 0, 1000, 800 }, &rects);
			CHECK(rects.size() == 6);
			WuiRect hierarchy, view, stats;
			for (const auto& [panel, rect] : rects)
			{
				if (panel == "hierarchy") hierarchy = rect;
				if (panel == "view") view = rect;
				if (panel == "stats") stats = rect;
			}
			CHECK(Near(hierarchy.X, 0) && Near(hierarchy.W, 240));
			CHECK(Near(hierarchy.H, 800 * 0.58f));
			CHECK(Near(view.X, 240) && Near(view.W, 760));
			CHECK(Near(view.H, 800 * 0.82f));
			CHECK(Near(stats.Y, 800 * 0.82f));
		}

		// 10. JSON:字符串转义与解析错误
		{
			std::string error;
			auto parsed = JsonValue::Parse("{\"title\":\"a\\n\\\"b\\\"\",\"count\":3}", &error);
			CHECK(parsed.has_value());
			CHECK(parsed->Find("title")->AsString() == "a\n\"b\"");
			CHECK(parsed->Find("count")->AsNumber() == 3);
			CHECK(!JsonValue::Parse("{bad", &error).has_value());
			CHECK(!error.empty());
		}

		// 11. 拖放状态机:按下→移动→武装落点→释放→AcceptDrop 拿到完整 payload
		{
			WuiContext ctx;
			WuiInputState input;
			input.MousePos = { 10, 10 };
			input.MouseDown[0] = true;
			input.MouseClicked[0] = true;
			ctx.BeginFrame(input);
			ctx.BeginDrag(HashId("d"), "panel:view");
			ctx.EndFrame();
			CHECK(!ctx.IsDragActive(nullptr));

			WuiInputState move;
			move.MousePos = { 60, 60 };
			move.MouseDown[0] = true;
			ctx.BeginFrame(move);
			ctx.EndFrame(); // 移动超过阈值,帧末进入拖拽态
			std::string kind;
			CHECK(ctx.IsDragActive(&kind));
			CHECK(kind == "panel:view");
			ctx.DropTarget({ 50, 50, 40, 40 }, "file:"); // 前缀不匹配,不应武装

			WuiInputState release;
			release.MousePos = { 60, 60 };
			release.MouseReleased[0] = true;
			ctx.BeginFrame(release);
			ctx.DropTarget({ 50, 50, 40, 40 }, "panel:");
			ctx.EndFrame();

			std::string payload;
			CHECK(!ctx.AcceptDrop(&payload, "file:")); // 类型不匹配:不消费,留给 panel 消费者
			CHECK(ctx.AcceptDrop(&payload, "panel:"));
			CHECK(payload == "panel:view");
			CHECK(!ctx.AcceptDrop(&payload, "panel:")); // 只消费一次

			// 释放时无任何武装落点 → 不接受
			WuiInputState press2;
			press2.MousePos = { 10, 10 };
			press2.MouseDown[0] = true;
			press2.MouseClicked[0] = true;
			ctx.BeginFrame(press2);
			ctx.BeginDrag(HashId("d2"), "panel:stats");
			ctx.EndFrame();
			WuiInputState move2;
			move2.MousePos = { 90, 90 };
			move2.MouseDown[0] = true;
			ctx.BeginFrame(move2);
			ctx.EndFrame();
			WuiInputState release2;
			release2.MousePos = { 90, 90 };
			release2.MouseReleased[0] = true;
			ctx.BeginFrame(release2);
			ctx.EndFrame();
			CHECK(!ctx.AcceptDrop(&payload));
		}

		// 12. 操作记录与撤销栈
		{
			WuiOperationLog log;
			log.Record(1, 0, "dock", "drop", "view", "");
			log.Record(2, 0, "browser", "move", "a.png", "dir");
			CHECK(log.Count() == 2);
			CHECK(log.Records().front().Category == "dock");
			CHECK(log.Records().back().Target == "a.png");

			WuiUndoStack stack;
			int value = 0;
			stack.Push("inc", [&] { --value; }, [&] { ++value; });
			value = 5;
			CHECK(stack.Undo() && value == 4);
			CHECK(stack.Redo() && value == 5);
			// 新 Push 截断重做分支
			CHECK(stack.Undo() && value == 4);
			stack.Push("set", [&] { value = 0; }, [&] { value = 4; });
			CHECK(!stack.CanRedo());
			CHECK(stack.Undo() && value == 0);
			stack.Clear();
			CHECK(!stack.CanUndo() && !stack.CanRedo());
		}

		// 13. 布局不变量:任意 MoveTab 序列后无重复/缺失/空组/单子 split
		{
			const std::vector<PanelId> all = { "hierarchy", "properties", "content_browser", "view", "stats", "memory", "operations" };
			const std::vector<DropZone> zones = { DropZone::Center, DropZone::Left, DropZone::Right, DropZone::Top, DropZone::Bottom };
			std::function<void(const DockNode&, std::set<PanelId>&, bool&, const std::string&)> walk =
				[&](const DockNode& node, std::set<PanelId>& seen, bool& ok, const std::string& path)
				{
					if (!ok) return;
					if (node.IsTabs())
					{
						if (node.Panels.empty()) { ok = false; return; }
						if (node.Active >= node.Panels.size()) { ok = false; return; }
						for (const PanelId& panel : node.Panels)
							if (!seen.insert(panel).second) { ok = false; return; }
						return;
					}
					if (node.Children.size() < 2) { ok = false; return; }
					for (const DockNode& child : node.Children)
						walk(child, seen, ok, path + "/");
				};

			for (const PanelId& panel : all)
			{
				for (const PanelId& target : all)
				{
					if (target == panel) continue;
					for (const DropZone& zone : zones)
					{
						DockLayout layout = DockLayout::Default(all);
						const bool moved = layout.MoveTab(panel, target, zone);
						CHECK(moved);
						std::set<PanelId> seen;
						bool ok = true;
						walk(layout.Root, seen, ok, "");
						CHECK(ok);
						CHECK(seen.size() == all.size());
						CHECK(layout.Contains(panel));
					}
				}
			}
		}

		// 14. 弹出菜单:开启当帧的点击不关闭,后续帧点击外部才关闭
		{
			WuiContext ctx;
			const WuiId popup = HashId("menu");
			WuiInputState open;
			open.MousePos = { 100, 5 };
			open.MouseClicked[0] = true;
			ctx.BeginFrame(open);
			ctx.OpenPopup(popup);
			ctx.ClosePopupsOnOutsideClick({ popup }, { 0, 20, 200, 100 });
			CHECK(ctx.IsPopupOpen(popup)); // 同帧打开,不应被这次点击关闭
			ctx.EndFrame();

			WuiInputState outside;
			outside.MousePos = { 100, 5 };
			outside.MouseClicked[0] = true;
			ctx.BeginFrame(outside);
			ctx.ClosePopupsOnOutsideClick({ popup }, { 0, 20, 200, 100 });
			CHECK(!ctx.IsPopupOpen(popup)); // 后续帧点击外部 → 关闭
			ctx.EndFrame();
		}

		// 15. 菜单栏交互:点击头部 → 弹层打开并包含条目,下一帧仍打开
		{
			WuiContext ctx;
			const WuiTheme theme;
			const WuiId menuId = HashId("menu.window");
			const WuiRect header { 70, 2, 76, 22 };
			WuiInputState click;
			click.MousePos = { 80, 12 };
			click.MouseClicked[0] = true;
			click.MouseDown[0] = true;
			ctx.BeginFrame(click);
			CHECK(BeginMenu(ctx, menuId, header, "Window", theme));
			ctx.PushOverlay();
			DrawPanelSurface(ctx, { header.X, 24, 240, 60 }, theme);
			const WuiRect item { header.X + 4, 28, 200, 22 };
			MenuItem(ctx, HashId("w.hierarchy"), item, "Scene Hierarchy", true, true, theme);
			EndMenu(ctx, menuId, { header.X, 24, 240, 60 }, theme);
			ctx.PopOverlay();
			CHECK(ctx.IsPopupOpen(menuId));
			const size_t overlayCount = ctx.OverlayCommands().size();
			CHECK(overlayCount >= 3); // 面板背景 + 边框 + 条目文字
			ctx.EndFrame();

			WuiInputState idle;
			idle.MousePos = { 100, 40 };
			ctx.BeginFrame(idle);
			CHECK(BeginMenu(ctx, menuId, header, "Window", theme));
			ctx.PushOverlay();
			DrawPanelSurface(ctx, { header.X, 24, 240, 60 }, theme);
			MenuItem(ctx, HashId("w.hierarchy"), item, "Scene Hierarchy", true, true, theme);
			EndMenu(ctx, menuId, { header.X, 24, 240, 60 }, theme);
			ctx.PopOverlay();
			CHECK(ctx.IsPopupOpen(menuId));
			ctx.EndFrame();
		}

		// 保留模式 widget 树:列布局、grow 与命中顺序
		{
			const auto root = std::make_shared<WuiBox>();
			root->Direction = WuiDirection::Column;
			root->Gap = 4;
			root->AlignCross = WuiAlign::Stretch;

			const auto top = std::make_shared<WuiLabel>();
			top->Text = "Header";
			top->FixedHeight = 24;

			const auto row = std::make_shared<WuiBox>();
			row->Direction = WuiDirection::Row;
			row->AlignCross = WuiAlign::Stretch;
			row->Gap = 4;

			const auto left = std::make_shared<WuiButton>();
			left->Label = "Left";
			left->SetId(HashId("btn.left"));
			const auto right = std::make_shared<WuiButton>();
			right->Label = "Right";
			right->SetId(HashId("btn.right"));
			row->Add(left, { 0, 1e30f, 0, 1e30f, 1 });
			row->Add(right, { 0, 1e30f, 0, 1e30f, 1 });

			root->Add(top);
			root->Add(row, { 0, 1e30f, 0, 1e30f, 1 });

			LayoutWidgetTree(root, { 10, 10, 200, 100 });
			CHECK(Near(root->Rect().X, 10) && Near(root->Rect().W, 200));
			CHECK(Near(top->Rect().H, 24));
			CHECK(Near(row->Rect().Y, 10 + 24 + 4));
			CHECK(Near(left->Rect().W, 98) && Near(right->Rect().W, 98));

			// 命中:右侧按钮优先命中其自身,空白处回退到容器。
			CHECK(root->HitTest({ 160, 60 }) == right);
			CHECK(root->HitTest({ 60, 60 }) == left);
			CHECK(root->HitTest({ -5, -5 }) == nullptr);

			// 脏标记:无变化时布局结果被缓存,再次 arrange 不改变矩形。
			left->Invalidate();
			CHECK(root->IsDirty());
			LayoutWidgetTree(root, { 10, 10, 200, 100 });
			CHECK(!root->IsDirty());

			// 绘制输出命令。
			WuiContext ctx;
			WuiInputState input;
			ctx.BeginFrame(input);
			WuiPaintContext paint(ctx);
			root->Paint(paint);
			CHECK(ctx.Commands().size() >= 5);
			ctx.EndFrame();
		}

		// M2 控件:TextField/DragInt/Combo/ScrollArea 的布局与命令输出
		{
			WuiContext ctx;
			WuiInputState input;
			ctx.BeginFrame(input);

			std::string buffer = "hello";
			const auto field = std::make_shared<WuiTextField>();
			field->Buffer = &buffer;
			LayoutWidgetTree(field, { 0, 0, 200, 24 });
			CHECK(Near(field->Rect().W, 200) && Near(field->Rect().H, 24));
			WuiPaintContext paint(ctx);
			field->Paint(paint);
			CHECK(ctx.Commands().size() >= 2);

			int64_t integer = 3;
			const auto drag = std::make_shared<WuiDragInt>();
			drag->Value = &integer;
			drag->SetId(HashId("drag.i"));
			LayoutWidgetTree(drag, { 0, 0, 100, 22 });
			drag->Paint(paint);
			CHECK(integer == 3);

			std::vector<std::string> options = { "A", "B" };
			int selected = 1;
			const auto combo = std::make_shared<WuiCombo>();
			combo->Options = &options;
			combo->Selected = &selected;
			combo->SetId(HashId("combo"));
			LayoutWidgetTree(combo, { 0, 0, 120, 24 });
			combo->Paint(paint);
			CHECK(selected == 1);

			const auto scroll = std::make_shared<WuiScrollArea>();
			const auto content = std::make_shared<WuiLabel>();
			content->Text = "content";
			content->FixedHeight = 1000;
			scroll->ContentHeight = 1000;
			scroll->Child = content;
			LayoutWidgetTree(scroll, { 0, 0, 100, 100 });
			scroll->Paint(paint);
			CHECK(scroll->HitTest({ 50, 50 }) == content);
			CHECK(scroll->HitTest({ 500, 500 }) == nullptr);

			ctx.EndFrame();
		}

		// 组件库:Toggle / Slider / Tabs / ListItem / TreeItem / MenuButton / Tooltip
		{
			WuiContext ctx;
			WuiInputState input;
			ctx.BeginFrame(input);

			bool toggleValue = false;
			auto toggle = std::make_shared<WuiToggle>();
			toggle->Value = &toggleValue;
			toggle->SetId(HashId("gallery.toggle"));
			LayoutWidgetTree(toggle, { 0, 0, 160, 22 });
			{
				WuiPaintContext paint(ctx);
				input.MousePos = { 150, 11 };
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				toggle->Paint(paint);
				CHECK(toggleValue);
				input.MouseClicked[0] = false;
			}

			float sliderValue = 0.0f;
			auto slider = std::make_shared<WuiSlider>();
			slider->Value = &sliderValue;
			slider->SetId(HashId("gallery.slider"));
			LayoutWidgetTree(slider, { 0, 0, 200, 22 });
			{
				WuiPaintContext paint(ctx);
				input.MousePos = { 100, 11 }; // 轨道中点
				input.MouseDown[0] = true;
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				slider->Paint(paint);
				CHECK(sliderValue > 0.45f && sliderValue < 0.55f);
				input.MouseClicked[0] = false;
				input.MouseDown[0] = false;
				ctx.BeginFrame(input);
				slider->Paint(paint);
			}

			int selectedTab = 0;
			int tabChanges = 0;
			auto tabs = std::make_shared<WuiTabs>();
			tabs->Labels = { "One", "Two", "Three" };
			tabs->Selected = &selectedTab;
			tabs->OnChanged = [&](int) { ++tabChanges; };
			tabs->SetId(HashId("gallery.tabs"));
			LayoutWidgetTree(tabs, { 0, 0, 300, 26 });
			{
				WuiPaintContext paint(ctx);
				input.MousePos = { 160, 13 }; // 第二格
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				tabs->Paint(paint);
				CHECK(selectedTab == 1);
				CHECK(tabChanges == 1);
				input.MouseClicked[0] = false;
			}

			int listClicks = 0;
			auto item = std::make_shared<WuiListItem>();
			item->Label = "Row";
			item->Selected = true;
			item->OnSelect = [&] { ++listClicks; };
			item->SetId(HashId("gallery.item"));
			LayoutWidgetTree(item, { 0, 0, 180, 22 });
			{
				WuiPaintContext paint(ctx);
				input.MousePos = { 90, 11 };
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				item->Paint(paint);
				CHECK(listClicks == 1);
				CHECK(!ctx.Commands().empty());
				input.MouseClicked[0] = false;
			}

			bool expanded = false;
			int treeToggles = 0;
			auto treeItem = std::make_shared<WuiTreeItem>();
			treeItem->Label = "Node";
			treeItem->Expanded = &expanded;
			treeItem->OnToggle = [&] { ++treeToggles; };
			treeItem->SetId(HashId("gallery.tree"));
			LayoutWidgetTree(treeItem, { 0, 0, 180, 22 });
			{
				WuiPaintContext paint(ctx);
				input.MousePos = { 14, 11 }; // 箭头区域
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				treeItem->Paint(paint);
				CHECK(expanded);
				CHECK(treeToggles == 1);
				input.MouseClicked[0] = false;
			}

			int menuChoice = -1;
			auto menuButton = std::make_shared<WuiMenuButton>();
			menuButton->Label = "Menu";
			menuButton->Items = { "Alpha", "Beta" };
			menuButton->OnSelect = [&](int index) { menuChoice = index; };
			menuButton->SetId(HashId("gallery.menu"));
			LayoutWidgetTree(menuButton, { 0, 0, 120, 24 });
			{
				WuiPaintContext paint(ctx);
				// 按下菜单按钮:只展开菜单,项不触发(P4-U30:菜单项改成 release 确认)。
				input.MousePos = { 60, 12 };
				input.MouseDown[0] = true;
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				CHECK(ctx.IsPopupOpen(menuButton->Id()));
				CHECK(menuChoice == -1);
				input.MouseClicked[0] = false;

				// 展开帧:菜单条目绘制到 Overlay 层。
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				CHECK(!ctx.OverlayCommands().empty());

				// 经典路径:按住菜单按钮 → 滑到第二项 → 松开 = 触发一次且菜单关闭。
				const glm::vec2 beta { 60, 24 + 2 + 4 + 22 + 11 }; // 第二个条目中心
				input.MousePos = beta;
				input.MouseDown[0] = false;
				input.MouseReleased[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				CHECK(menuChoice == 1);
				CHECK(!ctx.IsPopupOpen(menuButton->Id()));
				input.MouseReleased[0] = false;
			}

			// P4-U30①:菜单项在 release 帧触发 —— press 落在项上只登记归属。
			{
				WuiPaintContext paint(ctx);
				input.MousePos = { 60, 12 };
				input.MouseDown[0] = true;
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				input.MouseClicked[0] = false;
				input.MouseDown[0] = false;
				input.MouseReleased[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				CHECK(ctx.IsPopupOpen(menuButton->Id())); // 松开在按钮上 = 菜单仍开着
				input.MouseReleased[0] = false;

				const glm::vec2 beta { 60, 24 + 2 + 4 + 22 + 11 };
				menuChoice = -1;
				input.MousePos = beta;
				input.MouseDown[0] = true;
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				CHECK(menuChoice == -1); // press 不触发
				CHECK(ctx.IsPopupOpen(menuButton->Id()));
				input.MouseClicked[0] = false;

				input.MouseDown[0] = false;
				input.MouseReleased[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				CHECK(menuChoice == 1); // release 落在同一项上才触发
				CHECK(!ctx.IsPopupOpen(menuButton->Id()));
				input.MouseReleased[0] = false;
			}

			// P4-U30②:在项上按下 → 拖走 → 松开:不触发(菜单保持打开,松开不算点击)。
			{
				WuiPaintContext paint(ctx);
				menuChoice = -1;
				input.MousePos = { 60, 12 };
				input.MouseDown[0] = true;
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				input.MouseClicked[0] = false;
				input.MouseDown[0] = false;
				input.MouseReleased[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				input.MouseReleased[0] = false;

				const glm::vec2 beta { 60, 24 + 2 + 4 + 22 + 11 };
				input.MousePos = beta;
				input.MouseDown[0] = true;
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				input.MouseClicked[0] = false;

				input.MousePos = { 320, 300 }; // 拖到菜单外
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				input.MouseDown[0] = false;
				input.MouseReleased[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				CHECK(menuChoice == -1);
				CHECK(ctx.IsPopupOpen(menuButton->Id()));
				input.MouseReleased[0] = false;

				// 收尾:外部按下关掉菜单(下一段从"菜单关闭"起步)。
				input.MousePos = { 320, 300 };
				input.MouseDown[0] = true;
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				CHECK(!ctx.IsPopupOpen(menuButton->Id()));
				input.MouseClicked[0] = false;
				input.MouseDown[0] = false;
			}

			// P4-U30③:在菜单面板里按下(项以外)→ 滑到项上松开 = 触发(press 在面板范围内)。
			{
				WuiPaintContext paint(ctx);
				menuChoice = -1;
				// 先把菜单打开(点按钮后松开在按钮上)。
				input.MousePos = { 60, 12 };
				input.MouseDown[0] = true;
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				input.MouseClicked[0] = false;
				input.MouseDown[0] = false;
				input.MouseReleased[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				CHECK(ctx.IsPopupOpen(menuButton->Id()));
				input.MouseReleased[0] = false;

				const glm::vec2 panelPadding { 70, 26 + 2 }; // 面板上边缘的空白,不在任何项里
				input.MousePos = panelPadding;
				input.MouseDown[0] = true;
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				CHECK(menuChoice == -1);
				CHECK(ctx.IsPopupOpen(menuButton->Id()));
				input.MouseClicked[0] = false;

				input.MousePos = { 60, 24 + 2 + 4 + 11 }; // 第一个条目中心
				input.MouseDown[0] = false;
				input.MouseReleased[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				CHECK(menuChoice == 0);
				CHECK(!ctx.IsPopupOpen(menuButton->Id()));
				input.MouseReleased[0] = false;
			}

			// P4-U30④:在菜单外按下 → 拖到菜单项上松开:不触发(菜单已被外部按下关掉)。
			{
				WuiPaintContext paint(ctx);
				menuChoice = -1;
				input.MousePos = { 60, 12 };
				input.MouseDown[0] = true;
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				input.MouseClicked[0] = false;
				input.MouseDown[0] = false;
				input.MouseReleased[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				input.MouseReleased[0] = false;
				CHECK(ctx.IsPopupOpen(menuButton->Id()));

				input.MousePos = { 320, 300 }; // 菜单之外(视口位置)
				input.MouseDown[0] = true;
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				CHECK(!ctx.IsPopupOpen(menuButton->Id()));
				input.MouseClicked[0] = false;

				input.MousePos = { 60, 24 + 2 + 4 + 11 }; // 滑到(已关闭的)菜单项位置
				input.MouseDown[0] = false;
				input.MouseReleased[0] = true;
				ctx.BeginFrame(input);
				menuButton->Paint(paint);
				CHECK(menuChoice == -1);
				CHECK(!ctx.IsPopupOpen(menuButton->Id()));
				input.MouseReleased[0] = false;
			}

			auto tooltip = std::make_shared<WuiTooltip>();
			tooltip->Text = "hint";
			tooltip->Anchor = { 0, 0, 100, 20 };
			tooltip->SetId(HashId("gallery.tooltip"));
			{
				WuiPaintContext paint(ctx);
				input.MousePos = { 50, 10 };
				ctx.BeginFrame(input);
				tooltip->Paint(paint);
				CHECK(!ctx.OverlayCommands().empty());
				CHECK(ctx.Commands().empty());
			}

			// 分隔线:只输出一条线命令。
			auto separator = std::make_shared<WuiSeparator>();
			LayoutWidgetTree(separator, { 0, 40, 120, 1 });
			{
				ctx.BeginFrame(input);
				WuiPaintContext paint(ctx);
				separator->Paint(paint);
				CHECK(ctx.Commands().size() == 1);
			}

			ctx.EndFrame();
		}

		// 浮动面板:拖出停靠、序列化往返、回停靠与关闭
		{
			DockLayout layout = DockLayout::Default({ "a", "b", "c" });
			CHECK(layout.Contains("a"));
			CHECK(layout.Float("a", { 100, 120, 300, 200 }));
			CHECK(!layout.Contains("a"));
			CHECK(layout.IsFloating("a"));
			const DockFloat* floating = layout.FindFloat("a");
			CHECK(floating != nullptr);
			CHECK(Near(floating->Rect.X, 100) && Near(floating->Rect.W, 300));

			const std::string json = layout.Serialize();
			DockLayout restored;
			std::string error;
			CHECK(DockLayout::Deserialize(json, &restored, &error));
			CHECK(restored.IsFloating("a") && !restored.Contains("a"));
			CHECK(restored.FindFloat("a") != nullptr && Near(restored.FindFloat("a")->Rect.Y, 120));

			// 回停靠到 b,再确认浮动记录被移除。
			CHECK(restored.DockFloating("a", "b", DropZone::Center));
			CHECK(restored.Contains("a") && !restored.IsFloating("a"));

			// 旧版(v1,无 floating 字段)布局仍可读取。
			DockLayout legacy;
			CHECK(DockLayout::Deserialize(
				"{\"version\":1,\"root\":{\"type\":\"tabs\",\"panels\":[\"x\"],\"active\":0}}", &legacy, &error));
			CHECK(legacy.Contains("x"));
			CHECK(legacy.Floating.empty());

			// 关闭浮动面板只移除浮动记录,不影响停靠树。
			CHECK(legacy.Float("x", { 40, 60, 240, 160 }));
			CHECK(legacy.IsFloating("x"));
			CHECK(legacy.CloseFloating("x"));
			CHECK(!legacy.IsFloating("x"));

			// 置顶:最后绘制 = 最后入列。
			DockLayout order = DockLayout::Default({ "one", "two" });
			CHECK(order.Float("one", { 10, 10, 200, 150 }));
			CHECK(order.Float("two", { 40, 40, 200, 150 }));
			order.BringFloatToFront("one");
			CHECK(order.Floating.back().Panel == "one");
		}

		// 16. Chrome 基础组件:每条命令的输出与命中/悬停语义
		{
			WuiContext ctx;
			WuiTheme theme;
			WuiInputState input;
			input.MousePos = { 1000, 1000 };
			ctx.BeginFrame(input);

			PanelBackground(ctx, { 0, 0, 10, 10 }, theme.PanelBg, 2.0f);
			CHECK(ctx.Commands().size() == 1);
			CHECK(ctx.Commands().back().Kind == WuiDrawKind::Rect);

			BarSurface(ctx, { 0, 0, 100, 24 }, theme.PanelHeader, theme.Border);
			CHECK(ctx.Commands().size() == 3); // 填充 + 底边线
			CHECK(Near(ctx.Commands().back().Rect.Y, 23.0f) && Near(ctx.Commands().back().Rect.H, 1.0f));

			HighlightOutline(ctx, { 0, 0, 20, 20 }, theme.Accent);
			CHECK(ctx.Commands().size() == 4);
			CHECK(ctx.Commands().back().Kind == WuiDrawKind::RectOutline);

			DropZoneOverlay(ctx, { 0, 0, 20, 20 });
			CHECK(ctx.Commands().size() == 6); // 半透明填充 + 高亮描边

			// 悬停行:未悬停/未选中不绘制,悬停或选中各输出一条底色。
			CHECK(!HoverRow(ctx, { 0, 0, 50, 20 }, false, false, theme));
			CHECK(ctx.Commands().size() == 6);
			CHECK(HoverRow(ctx, { 0, 0, 50, 20 }, true, false, theme));
			CHECK(ctx.Commands().size() == 7);
			HoverRow(ctx, { 0, 0, 50, 20 }, false, true, theme);
			CHECK(ctx.Commands().size() == 8);

			// 分节标题:文字 + 分隔线。
			SectionHeader(ctx, { 0, 0, 120, 24 }, "Section", theme.Accent, theme);
			CHECK(ctx.Commands().size() == 10);
			ctx.EndFrame();
		}

		// 17. 挂靠标签 chip:普通标签点击、活动底色、关闭按钮命中
		{
			const WuiRect tag { 0, 0, 140, 20 };
			WuiTheme theme;
			{
				WuiContext ctx;
				WuiInputState input;
				input.MousePos = { 20, 10 };
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				const AttachTagResult result = AttachTag(ctx, tag, "Widget", false, false, theme);
				CHECK(result.Hovered && result.Clicked && !result.CloseClicked);
				CHECK(ctx.Commands().size() == 2); // 悬停底色 + 标题
				ctx.EndFrame();
			}
			{
				WuiContext ctx;
				WuiInputState input;
				input.MousePos = { 20, 10 };
				ctx.BeginFrame(input);
				const AttachTagResult result = AttachTag(ctx, tag, "Widget", true, false, theme);
				CHECK(result.Hovered && !result.Clicked);
				CHECK(ctx.Commands().size() == 2); // 活动底色 + 标题
				ctx.EndFrame();
			}
			{
				WuiContext ctx;
				WuiInputState input;
				input.MousePos = { tag.X + tag.W - 12.0f, tag.Y + tag.H * 0.5f };
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				const AttachTagResult result = AttachTag(ctx, tag, "Widget", false, true, theme);
				CHECK(result.CloseHovered && result.CloseClicked && !result.Clicked);
				CHECK(result.CloseRect.W > 0.0f);
				CHECK(result.CloseRect.X + result.CloseRect.W <= tag.X + tag.W);
				ctx.EndFrame();
			}
		}

		// D3:可搜索下拉(材质/贴图路径选择)的交互回归:
		// 打开 → 在搜索框里点击/输入/退格 → 过滤 → 选中 → 回车确认 → Esc 关闭。
		// 之前这里踩过"点击弹层里的搜索框被当成外部点击把弹层关掉"的坑。
		{
			WuiContext ctx;
			WuiTheme theme;
			const WuiRect rect { 100, 100, 240, 22 };
			const std::vector<std::string> options {
				"materials/glass_red.wmat", "materials/quadrants.wmat", "materials/satin_blue.wmat" };
			const WuiId id = HashId("test.combo");
			int selected = 0;

			// 最小验证:先单独驱动 TextField 输入(隔离是否 SearchableCombo 的问题)。
			{
				std::string buffer = "abc";
				WuiInputState input;
				input.MousePos = { rect.X + 20, rect.Y + 10 };
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				TextField(ctx, HashId("test.textfield"), rect, buffer, theme);
				ctx.EndFrame();
				input.MouseClicked[0] = false;
				input.TextInput = { 's', 'a', 't' };
				ctx.BeginFrame(input);
				TextField(ctx, HashId("test.textfield"), rect, buffer, theme);
				ctx.EndFrame();
				CHECK(buffer.find("sat") != std::string::npos);
			}

			// 1) 点击下拉本体:弹层打开。
			{
				WuiInputState input;
				input.MousePos = { rect.X + 20, rect.Y + rect.H * 0.5f };
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				SearchableCombo(ctx, id, rect, "", options, selected, theme);
				ctx.EndFrame();
			}
			CHECK(ctx.IsPopupOpen(id));

			// 2) 点击弹层里的搜索框:弹层必须仍然打开(这一条曾经失败)。
			{
				WuiInputState input;
				input.MousePos = { rect.X + 20, rect.Y + rect.H + 16.0f };
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				SearchableCombo(ctx, id, rect, "", options, selected, theme);
				ctx.EndFrame();
			}
			CHECK(ctx.IsPopupOpen(id));

			// 3) 输入过滤词 "sat":应命中 satin_blue。
			{
				WuiInputState input;
				input.MousePos = { rect.X + 20, rect.Y + rect.H + 16.0f };
				input.TextInput = { 's', 'a', 't' };
				ctx.BeginFrame(input);
				SearchableCombo(ctx, id, rect, "", options, selected, theme);
				ctx.EndFrame();
			}
			CHECK(ctx.IsPopupOpen(id));

			// 4) 回车确认:选中过滤后的第一项,弹层关闭。
			{
				WuiInputState input;
				input.MousePos = { rect.X + 20, rect.Y + rect.H + 16.0f };
				input.KeyDown = { World::KeyCodes::Enter };
				ctx.BeginFrame(input);
				SearchableCombo(ctx, id, rect, "", options, selected, theme);
				ctx.EndFrame();
			}
			CHECK(!ctx.IsPopupOpen(id));
			CHECK(selected == 2);

			// 5) 重新打开:过滤器应已重置(显示全部),Esc 关闭。
			{
				WuiInputState input;
				input.MousePos = { rect.X + 20, rect.Y + rect.H * 0.5f };
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				SearchableCombo(ctx, id, rect, "", options, selected, theme);
				ctx.EndFrame();
			}
			CHECK(ctx.IsPopupOpen(id));
			{
				WuiInputState input;
				input.MousePos = { rect.X + 20, rect.Y + rect.H + 16.0f };
				input.KeyDown = { World::KeyCodes::Escape };
				ctx.BeginFrame(input);
				SearchableCombo(ctx, id, rect, "", options, selected, theme);
				ctx.EndFrame();
			}
			CHECK(!ctx.IsPopupOpen(id));
		}

		// 18. W9-3 脚本化文本注入:点击聚焦(两帧)→ 逐行注入字符。
		{
			WuiScriptedInput& scripted = WuiScriptedInput::Get();
			WuiInputState input;

			// 只有文本待注入的窗口(没有点击):下一帧直接写 TextInput,中文按码点写入。
			scripted.QueueType("main", u8"你好");
			CHECK(scripted.HasPending());
			scripted.Apply("main", input);
			CHECK(input.TextInput == Codepoints({ 0x4F60u, 0x597Du }));
			CHECK(input.KeyDown.empty());
			CHECK(!input.MouseClicked[0] && !input.MouseReleased[0]);
			CHECK(!scripted.HasPending());
			input = WuiInputState {};
			scripted.Apply("main", input);
			CHECK(input.TextInput.empty()); // 消费完即清空:没有注入时零行为变化

			// 点击 + 多行文本:第 1 帧 press、第 2 帧 release、第 3 帧起每帧一行,
			// 第 2 行起该帧先注入 Enter 键再写该行码点(多行内容走控件的换行路径)。
			input = WuiInputState {};
			scripted.QueueClick("main", { 40.0f, 50.0f });
			scripted.QueueType("main", u8"ab\ncd\tef");
			CHECK(scripted.HasPending());
			scripted.Apply("main", input);
			CHECK(input.MouseDown[0] && input.MouseClicked[0] && !input.MouseReleased[0]);
			CHECK(Near(input.MousePos.x, 40.0f) && Near(input.MousePos.y, 50.0f));
			CHECK(input.TextInput.empty()); // 文本不和点击同帧
			input = WuiInputState {};
			scripted.Apply("main", input);
			CHECK(!input.MouseDown[0] && input.MouseReleased[0]);
			CHECK(input.TextInput.empty());
			CHECK(scripted.HasPending());
			input = WuiInputState {};
			scripted.Apply("main", input);
			CHECK(input.TextInput == Codepoints({ 'a', 'b' }));
			CHECK(input.KeyDown.empty() && input.KeyPressed.empty()); // 第一行不注入 Enter
			CHECK(scripted.HasPending());
			input = WuiInputState {};
			scripted.Apply("main", input);
			CHECK(input.TextInput == Codepoints({ 'c', 'd', '\t', 'e', 'f' }));
			CHECK(input.KeyDown == Codepoints({ World::KeyCodes::Enter }));
			CHECK(input.KeyPressed == Codepoints({ World::KeyCodes::Enter })); // 沿:编辑器按"一次动作"消费
			CHECK(!scripted.HasPending());
			input = WuiInputState {};
			scripted.Apply("main", input);
			CHECK(input.TextInput.empty() && input.KeyDown.empty());

			// CRLF:'\r' 并入换行、不写进 TextInput;别的窗口没有待注入内容时不受影响。
			input = WuiInputState {};
			scripted.QueueType("panel", u8"x\r\ny");
			scripted.Apply("other", input);
			CHECK(input.TextInput.empty());
			CHECK(scripted.HasPending());
			scripted.Apply("panel", input);
			CHECK(input.TextInput == Codepoints({ 'x' }));
			input = WuiInputState {};
			scripted.Apply("panel", input);
			CHECK(input.KeyDown == Codepoints({ World::KeyCodes::Enter }));
			CHECK(input.KeyPressed == Codepoints({ World::KeyCodes::Enter }));
			CHECK(input.TextInput == Codepoints({ 'y' }));
			CHECK(!scripted.HasPending());

			// 既有 click 逐帧语义不变(单独 QueueClick):press → release → 清空。
			input = WuiInputState {};
			scripted.QueueClick("win", { 1.0f, 2.0f });
			scripted.Apply("win", input);
			CHECK(input.MouseClicked[0] && !input.MouseReleased[0]);
			input = WuiInputState {};
			scripted.Apply("win", input);
			CHECK(input.MouseReleased[0] && !input.MouseClicked[0]);
			CHECK(!scripted.HasPending());
			input = WuiInputState {};
			scripted.Apply("win", input);
			CHECK(!input.MouseClicked[0] && !input.MouseReleased[0] && input.TextInput.empty());
		}

		// 19. WUI 组件登记表(P0-1/P0-2):结构不变量 + 排序稳定 + "重复登记=覆盖"
		{
			const std::vector<WuiComponentDesc>& all = WuiComponentRegistry::All();
			CHECK(all.size() >= 20);
			CHECK(WuiComponentRegistry::Count() == all.size());
			// P1a:TypeName = 该件在**面板侧**的控件入口名(保留模式类名,或立即模式控件入口名)。
			// 头文件文本读一次,循环里逐条断言"名字真的存在",挡住编出来的名字。
			const std::vector<std::string> wuiHeaders = LoadWuiHeaderTexts();
			CHECK(!wuiHeaders.empty());
			std::set<std::string> ids;
			std::set<std::string> typeNames;
			for (const WuiComponentDesc& desc : all)
			{
				CHECK(!desc.Id.empty());
				CHECK(ids.insert(desc.Id).second);      // id 唯一
				CHECK(!desc.TypeName.empty());          // 面板侧控件入口名必须声明
				CHECK(DeclaredInWuiHeaders(wuiHeaders, desc.TypeName));
				typeNames.insert(desc.TypeName);
				CHECK(!desc.DisplayName.empty());
				CHECK(!desc.Category.empty());
				CHECK(!desc.SourceFile.empty());
				CHECK(!desc.A11yNotes.empty());
				CHECK(!desc.SizeNotes.empty());
				CHECK(!desc.Properties.empty());        // 工作台属性编辑器至少有一项可调
				CHECK(desc.Showcase != nullptr);        // 必须是真实控件路径
				bool hasDefault = false;
				for (const WuiComponentState& state : desc.States)
					if (state.Id == "default")
						hasDefault = true;
				CHECK(hasDefault);
			}
			// 面板源码里实际引用的控件类型(Editor/src/WUI/Panels/** 的 `Wui::WuiXxx`)必须都能在
			// 登记表的 TypeName 里找到 —— 门禁 tools/agents/check-ui-components.ps1 从面板源码推导
			// 同一张名单;这里是它的运行时版本,同时钉住"这 10 件不被顺手删掉"。
			for (const char* panelType : { "WuiBox", "WuiButton", "WuiImage", "WuiImageButton", "WuiLabel",
					 "WuiListRow", "WuiProgress", "WuiScrollArea", "WuiSpacer", "WuiTextField" })
				CHECK(typeNames.count(panelType) == 1);
			// All() 稳定排序:连调两次逐条一致,且按 Category → DisplayName → Id 有序。
			const std::vector<WuiComponentDesc>& again = WuiComponentRegistry::All();
			CHECK(again.size() == all.size());
			for (size_t i = 0; i < all.size(); ++i)
			{
				CHECK(again[i].Id == all[i].Id);
				if (i == 0)
					continue;
				const WuiComponentDesc& previous = all[i - 1];
				const WuiComponentDesc& current = all[i];
				const bool ordered = previous.Category < current.Category
					|| (previous.Category == current.Category && previous.DisplayName < current.DisplayName)
					|| (previous.Category == current.Category && previous.DisplayName == current.DisplayName
						&& previous.Id < current.Id);
				CHECK(ordered);
			}
			// Find 与重复登记语义:同 id = 覆盖(不新增条目),空 id 被拒绝。
			CHECK(WuiComponentRegistry::Find("button") != nullptr);
			CHECK(WuiComponentRegistry::Find("no.such.component") == nullptr);
			const WuiComponentDesc original = *WuiComponentRegistry::Find("button");
			WuiComponentDesc probe = original;
			probe.DisplayName = "Button (override probe)";
			WuiComponentRegistry::Register(probe);
			CHECK(WuiComponentRegistry::Count() == all.size());
			CHECK(WuiComponentRegistry::Find("button")->DisplayName == "Button (override probe)");
			WuiComponentRegistry::Register(original);   // 还原:后续用例仍看到原生条目
			CHECK(WuiComponentRegistry::Find("button")->DisplayName == original.DisplayName);
			const size_t beforeEmpty = WuiComponentRegistry::Count();
			WuiComponentRegistry::Register(WuiComponentDesc {});
			CHECK(WuiComponentRegistry::Count() == beforeEmpty);
		}

		// 20. 每件 showcase:headless 走一遍真实控件路径 —— 至少一条绘制命令 + 稳定 a11y 锚点;
		//     属性塞非法值/未知名字、状态塞不存在的值时都必须按 default 处理,不崩。
		{
			WuiAccessibility& accessibility = WuiAccessibility::Get();
			accessibility.SetEnabled(true);
			WuiContext ctx;
			size_t drawn = 0;
			for (const WuiComponentDesc& desc : WuiComponentRegistry::All())
			{
				std::vector<std::string> states;
				for (const WuiComponentState& state : desc.States)
					states.push_back(state.Id);
				states.push_back("__unknown_state__");
				for (const std::string& state : states)
				{
					accessibility.BeginFrame("main", { 640.0f, 480.0f });
					accessibility.SetPanel("showcase");
					WuiInputState input;
					input.ViewportSize = { 640.0f, 480.0f };
					ctx.BeginFrame(input);
					WuiComponentDraw draw;
					draw.Context = &ctx;
					draw.Theme = &CurrentTheme();
					draw.Rect = { 12.0f, 12.0f, 420.0f, 160.0f };
					draw.State = state;
					draw.Properties = { { "value", "not-a-number" }, { "min", "NaN" }, { "bogus", "x" } };
					draw.UiScale = 1.0f;
					draw.Density = 1.0f;
					draw.Locale = "en";
					desc.Showcase(draw);
					CHECK(ctx.Commands().size() + ctx.OverlayCommands().size() > 0);
					ctx.EndFrame();
					for (const std::string& id : desc.ExtraA11yIds)
						CHECK(accessibility.Find(HashId(id.c_str())) != nullptr);
					++drawn;
				}
			}
			CHECK(drawn >= 20);
			accessibility.SetEnabled(false);
			accessibility.Clear();
		}

		// 21. 本地化分层加载(S1):多层叠加/来源诊断、层内重复记账、$ 元数据跳过、
		//     结构化条目前向兼容、SetLocalizationDirectory 兼容壳、英文内联回退、ReloadLocalization 重扫。
		//     夹具 = 临时目录(root 下每用例一个子目录),最后统一删除;用例结束恢复全局态。
		{
			namespace fs = std::filesystem;
			const fs::path root = fs::temp_directory_path() / "wld-wui-tests-localization";
			{
				std::error_code reset;
				fs::remove_all(root, reset);
			}
			const auto WriteText = [](const fs::path& path, const std::string& text)
			{
				fs::create_directories(path.parent_path());
				std::ofstream stream(path, std::ios::binary | std::ios::trunc);
				CHECK(static_cast<bool>(stream));
				stream << text;
				stream.close();
				CHECK(fs::exists(path));
			};
			const auto HasEntry = [](const std::vector<std::string>& entries, const std::string& wanted)
			{
				return std::find(entries.begin(), entries.end(), wanted) != entries.end();
			};

			// 21a. 三层叠加:priority 高者覆盖、缺键回退低层、来源可诊断、跨层同键不算冲突。
			{
				const fs::path dir = root / "layers";
				WriteText(dir / "engine" / "zh-CN" / "base.json",
					"{ \"a\": \"engine-a\", \"b\": \"engine-b\", \"$owns\": [\"a\", \"b\"] }");
				WriteText(dir / "editor" / "zh-CN" / "panel.json", "{ \"a\": \"editor-a\" }");
				WriteText(dir / "project" / "zh-CN" / "override.json", "{ \"a\": \"project-a\" }");
				ClearLocalizationLayers();
				RegisterLocalizationLayer("project", dir / "project", 20);   // 故意乱序注册:排序只看 priority
				RegisterLocalizationLayer("engine", dir / "engine", 0);
				RegisterLocalizationLayer("editor", dir / "editor", 10);
				SetLanguage("zh-CN");
				ReloadLocalization();
				CHECK(GetLanguage() == "zh-CN");
				CHECK(Tr("a", "fallback") == "project-a");               // 高优先级胜
				CHECK(Tr("b", "fallback") == "engine-b");                // 层没定义 → 用低层,不是 fallback
				CHECK(LocalizationSource("a") == "project/override.json");
				CHECK(LocalizationSource("b") == "engine/base.json");
				CHECK(LocalizationSource("no.such.key").empty());
				CHECK(LocalizationConflicts().empty());                  // 跨层同键 = 正常覆盖
				CHECK(Tr("no.such.key", "fallback") == "fallback");
				CHECK(HasEntry(MissingLocalizationKeys(), "no.such.key"));
			}

			// 21b. 同 priority:按注册顺序,后注册者覆盖。
			{
				const fs::path dir = root / "tie";
				WriteText(dir / "first" / "zh-CN" / "a.json", "{ \"tie\": \"first\" }");
				WriteText(dir / "second" / "zh-CN" / "a.json", "{ \"tie\": \"second\" }");
				ClearLocalizationLayers();
				RegisterLocalizationLayer("first", dir / "first", 5);
				RegisterLocalizationLayer("second", dir / "second", 5);
				SetLanguage("zh-CN");
				ReloadLocalization();
				CHECK(Tr("tie", "fallback") == "second");
				CHECK(LocalizationSource("tie") == "second/a.json");
			}

			// 21c. 层内重复:文件名序先出现者生效,后出现的记冲突;非 .json 文件不参与扫描;
			//      `$` 前缀 = 文件元数据,不作文案。
			{
				const fs::path dir = root / "dup";
				WriteText(dir / "dup" / "zh-CN" / "aa.json",
					"{ \"k\": \"first\", \"$format\": \"wld-localization/1\" }");
				WriteText(dir / "dup" / "zh-CN" / "bb.json", "{ \"k\": \"second\", \"k2\": \"ok\" }");
				WriteText(dir / "dup" / "zh-CN" / "cc.json", "{ \"j\": \"one\", \"j\": \"two\" }");
				WriteText(dir / "dup" / "zh-CN" / "zzz.txt", "{ \"k\": \"third\" }");
				ClearLocalizationLayers();
				RegisterLocalizationLayer("dup", dir / "dup", 0);
				SetLanguage("zh-CN");
				ReloadLocalization();
				CHECK(Tr("k", "fallback") == "first");                   // 文件名序:aa.json 胜 bb.json
				CHECK(Tr("k2", "fallback") == "ok");
				CHECK(Tr("j", "fallback") == "one");                     // 同一文件里重复:先出现者胜
				CHECK(Tr("$format", "meta-fallback") == "meta-fallback");
				CHECK(LocalizationSource("$format").empty());
				CHECK(LocalizationSource("k") == "dup/aa.json");
				const std::vector<std::string> conflicts = LocalizationConflicts();
				CHECK(conflicts.size() == 2);                            // zzz.txt 未被读 → 不是 3 条
				CHECK(HasEntry(conflicts, "dup/bb.json:k"));
				CHECK(HasEntry(conflicts, "dup/cc.json:j"));
			}

			// 21d. 结构化条目前向兼容:`{"text": …}` 可读、未知字段忽略、非文本值不进表。
			{
				const fs::path dir = root / "structured";
				WriteText(dir / "structured" / "zh-CN" / "entry.json",
					"{ \"obj\": { \"text\": \"structured-text\", \"context\": \"probe\", \"status\": \"reviewed\","
					" \"maxLength\": 32 }, \"num\": 5, \"arr\": [1, 2], \"none\": null, \"flag\": true }");
				ClearLocalizationLayers();
				RegisterLocalizationLayer("structured", dir / "structured", 0);
				SetLanguage("zh-CN");
				ReloadLocalization();
				CHECK(Tr("obj", "fallback") == "structured-text");
				CHECK(LocalizationSource("obj") == "structured/entry.json");
				CHECK(Tr("num", "fallback") == "fallback");
				CHECK(Tr("none", "fallback") == "fallback");
				CHECK(HasEntry(MissingLocalizationKeys(), "num"));
			}

			// 21e. SetLocalizationDirectory 兼容壳 = 清空后注册单层("default", 0);
			//      GetLocalizationDirectory 报第一层目录;再加一层即覆盖。
			{
				const fs::path dir = root / "compat";
				WriteText(dir / "compat" / "zh-CN" / "only.json", "{ \"compat.key\": \"compat-v\" }");
				WriteText(dir / "compat-override" / "zh-CN" / "only.json", "{ \"compat.key\": \"override-v\" }");
				SetLocalizationDirectory(dir / "compat");
				CHECK(GetLocalizationDirectory() == dir / "compat");
				SetLanguage("zh-CN");
				ReloadLocalization();
				CHECK(Tr("compat.key", "fallback") == "compat-v");
				CHECK(LocalizationSource("compat.key") == "default/only.json");
				RegisterLocalizationLayer("override", dir / "compat-override", 5);
				ReloadLocalization();
				CHECK(Tr("compat.key", "fallback") == "override-v");
				CHECK(GetLocalizationDirectory() == dir / "compat");
			}

			// 21f. 默认语言(en / en-US / 空)= 源码内联文案:层已注册也不查表。
			{
				const fs::path dir = root / "inline";
				WriteText(dir / "inline" / "zh-CN" / "a.json", "{ \"a\": \"zh-a\" }");
				ClearLocalizationLayers();
				RegisterLocalizationLayer("inline", dir / "inline", 0);
				SetLanguage("en");
				ReloadLocalization();
				CHECK(Tr("a", "inline-a") == "inline-a");
				CHECK(LocalizationSource("a").empty());
				CHECK(LocalizationConflicts().empty());
				SetLanguage("en-US");
				ReloadLocalization();
				CHECK(Tr("a", "inline-a") == "inline-a");
				SetLanguage("");
				ReloadLocalization();
				CHECK(GetLanguage().empty());
				CHECK(Tr("a", "inline-a") == "inline-a");
				SetLanguage("zh-CN");
				ReloadLocalization();
				CHECK(Tr("a", "inline-a") == "zh-a");                    // 同层换语言 → 查表
			}

			// 21g. ReloadLocalization:重扫全部层 + Generation++;改盘上的语言包不重启即生效(热重载地基)。
			{
				const fs::path dir = root / "reload";
				const fs::path file = dir / "hot" / "zh-CN" / "a.json";
				WriteText(file, "{ \"hot.key\": \"v1\" }");
				ClearLocalizationLayers();
				RegisterLocalizationLayer("hot", dir / "hot", 0);
				SetLanguage("zh-CN");
				ReloadLocalization();
				CHECK(Tr("hot.key", "fallback") == "v1");
				const uint32_t generation = LocalizationGeneration();
				WriteText(file, "{ \"hot.key\": \"v2\" }");
				CHECK(Tr("hot.key", "fallback") == "v1");                // 未重载 = 不重读盘
				ReloadLocalization();
				CHECK(Tr("hot.key", "fallback") == "v2");
				CHECK(LocalizationGeneration() > generation);
			}

			// 21h. 递归扫描:语言包按域分文件夹(`panels/`、`shell/`…)时子目录里的文件照样被加载;
			//      层内排序键 = 相对 `<lang>` 的路径(POSIX 分隔符,逐字节),
			//      因此"先出现者生效"跨目录也按路径序判,而不是按目录迭代顺序。
			{
				const fs::path dir = root / "recursive";
				WriteText(dir / "rec" / "zh-CN" / "zz-top.json", "{ \"dup.key\": \"top\" }");
				WriteText(dir / "rec" / "zh-CN" / "panels" / "a.json",
					"{ \"dup.key\": \"sub\", \"panel.one\": \"panel-v\" }");
				WriteText(dir / "rec" / "zh-CN" / "panels" / "nested" / "deep.json", "{ \"deep.key\": \"deep-v\" }");
				ClearLocalizationLayers();
				RegisterLocalizationLayer("rec", dir / "rec", 0);
				SetLanguage("zh-CN");
				ReloadLocalization();
				CHECK(Tr("panel.one", "fallback") == "panel-v");              // 子目录文件被扫到
				CHECK(LocalizationSource("panel.one") == "rec/panels/a.json");
				CHECK(Tr("deep.key", "fallback") == "deep-v");                // 递归到二级子目录
				CHECK(LocalizationSource("deep.key") == "rec/panels/nested/deep.json");
				CHECK(Tr("dup.key", "fallback") == "sub");                    // "panels/a.json" < "zz-top.json"
				CHECK(LocalizationSource("dup.key") == "rec/panels/a.json");
				const std::vector<std::string> recursiveConflicts = LocalizationConflicts();
				CHECK(recursiveConflicts.size() == 1);
				CHECK(HasEntry(recursiveConflicts, "rec/zz-top.json:dup.key"));   // 记被忽略的那次定义
			}

			// 21i. S2 回退链候选:`zh-CN` → {`zh-CN`, `zh`}(去重);英文族/空 = 空表;
			//      逐层按候选顺序找 `<dir>/<candidate>/`,先命中的候选生效(层之间独立解析)。
			{
				const fs::path dir = root / "fallback";
				WriteText(dir / "fb" / "zh" / "main.json", "{ \"fb.key\": \"zh-main\" }");
				WriteText(dir / "fb" / "zh-TW" / "main.json", "{ \"fb.key\": \"zh-TW-main\" }");
				ClearLocalizationLayers();
				RegisterLocalizationLayer("fb", dir / "fb", 0);
				SetLanguage("zh");
				ReloadLocalization();
				const std::vector<std::string> single = LocalizationLanguageCandidates();
				CHECK(single.size() == 1 && single[0] == "zh");              // 主标签 = 自身,不重复
				CHECK(Tr("fb.key", "fallback") == "zh-main");
				SetLanguage("zh-CN");
				ReloadLocalization();
				const std::vector<std::string> chain = LocalizationLanguageCandidates();
				CHECK(chain.size() == 2 && chain[0] == "zh-CN" && chain[1] == "zh");
				CHECK(Tr("fb.key", "fallback") == "zh-main");                // zh-CN 无包 → 用主标签 zh
				SetLanguage("zh-TW");
				ReloadLocalization();
				CHECK(Tr("fb.key", "fallback") == "zh-TW-main");             // 命中 zh-TW 就不再看 zh
				CHECK(LocalizationLanguageCandidates().size() == 2);
				SetLanguage("en");
				CHECK(LocalizationLanguageCandidates().empty());             // 英文族 = 内联默认,无候选
				CHECK(!LocalizationFilesChanged());
				SetLanguage("en-US");
				CHECK(LocalizationLanguageCandidates().empty());
				SetLanguage("");
				CHECK(LocalizationLanguageCandidates().empty());
				CHECK(Tr("fb.key", "inline") == "inline");                   // 空语言 = 内联默认
			}

			// 21j. S2 编译产物:层语言目录里的 `catalog.json` 且 `$format` 命中 → 该层该语言**只读它**
			//      (域文件被跳过;`$layers/$source_hash` 仅元数据);`$format` 不匹配 → 维持 S1 域文件扫描。
			{
				const fs::path dir = root / "artifact";
				const fs::path domain = dir / "prod" / "zh-CN" / "panels" / "a.json";
				WriteText(domain, "{ \"art.key\": \"domain-value\" }");
				WriteText(dir / "prod" / "zh-CN" / "catalog.json",
					"{ \"$format\": \"wld-localization-catalog/1\", \"$language\": \"zh-CN\","
					" \"$layers\": [\"engine\", \"editor\"], \"$source_hash\": \"deadbeef\","
					" \"art.key\": \"artifact-value\","
					" \"art.obj\": { \"text\": \"artifact-obj\", \"status\": \"reviewed\", \"maxLength\": 8 },"
					" \"art.num\": 5 }");
				ClearLocalizationLayers();
				RegisterLocalizationLayer("prod", dir / "prod", 0);
				SetLanguage("zh-CN");
				ReloadLocalization();
				CHECK(Tr("art.key", "fallback") == "artifact-value");        // 产物优先:域文件同键被跳过
				CHECK(Tr("art.obj", "fallback") == "artifact-obj");         // 结构化条目照读
				CHECK(Tr("art.num", "fallback") == "fallback");              // 非文本值不进表
				CHECK(LocalizationSource("art.key") == "prod/catalog.json");
				CHECK(Tr("$source_hash", "meta-fallback") == "meta-fallback");
				CHECK(Tr("$layers", "meta-fallback") == "meta-fallback");    // 产物元数据不作文案
				CHECK(LocalizationSource("$layers").empty());
				WriteText(domain, "{ \"art.key\": \"domain-value-2\" }");
				CHECK(!LocalizationFilesChanged());                          // 只跟踪产物本身(域文件不参与)
				ReloadLocalization();
				CHECK(Tr("art.key", "fallback") == "artifact-value");        // 域文件改了也不进表
				WriteText(dir / "prod" / "zh-CN" / "catalog.json",
					"{ \"$format\": \"wld-localization-catalog/1\", \"$language\": \"zh-CN\","
					" \"art.key\": \"artifact-value-2-longer\","
					" \"art.obj\": { \"text\": \"artifact-obj\" } }");
				CHECK(LocalizationFilesChanged());                           // 产物改动 → 真
				ReloadLocalization();
				CHECK(!LocalizationFilesChanged());
				CHECK(Tr("art.key", "fallback") == "artifact-value-2-longer");
				// `$format` 不匹配(旧格式/普通 JSON 恰好叫 catalog.json)→ 该文件回到域文件扫描里。
				const fs::path plain = root / "artifact-plain";
				WriteText(plain / "plain" / "zh-CN" / "catalog.json",
					"{ \"$format\": \"wld-localization/1\", \"plain.key\": \"catalog-as-domain\" }");
				WriteText(plain / "plain" / "zh-CN" / "other.json", "{ \"plain.other\": \"other-value\" }");
				ClearLocalizationLayers();
				RegisterLocalizationLayer("plain", plain / "plain", 0);
				SetLanguage("zh-CN");
				ReloadLocalization();
				CHECK(Tr("plain.key", "fallback") == "catalog-as-domain");
				CHECK(Tr("plain.other", "fallback") == "other-value");
				CHECK(!LocalizationFilesChanged());
			}

			// 21k. S2 热重载戳:未加载过 = false;加载后不变 = false;改 / 增 / 删 = true;
			//      `true` 只表示"盘面变了",不自动重载(宿主调 `ReloadLocalization`)。
			{
				const fs::path dir = root / "stamp";
				const fs::path a = dir / "st" / "zh-CN" / "a.json";
				const fs::path b = dir / "st" / "zh-CN" / "b.json";
				const fs::path c = dir / "st" / "zh-CN" / "panels" / "c.json";
				WriteText(a, "{ \"st.a\": \"a1\" }");
				WriteText(b, "{ \"st.b\": \"b1\" }");
				ClearLocalizationLayers();
				RegisterLocalizationLayer("st", dir / "st", 0);
				CHECK(!LocalizationFilesChanged());                          // 未加载过 → false
				SetLanguage("zh-CN");
				ReloadLocalization();
				CHECK(!LocalizationFilesChanged());                          // 盘面未变
				WriteText(a, "{ \"st.a\": \"a1-longer\" }");
				CHECK(LocalizationFilesChanged());                           // 改
				CHECK(Tr("st.a", "fallback") == "a1");                       // 不自动重载
				ReloadLocalization();
				CHECK(!LocalizationFilesChanged());
				CHECK(Tr("st.a", "fallback") == "a1-longer");
				WriteText(c, "{ \"st.c\": \"c1\" }");                        // 新增(含子目录)
				CHECK(LocalizationFilesChanged());
				ReloadLocalization();
				CHECK(!LocalizationFilesChanged());
				CHECK(Tr("st.c", "fallback") == "c1");
				std::error_code removeError;
				CHECK(fs::remove(b, removeError));                           // 删除
				CHECK(LocalizationFilesChanged());
				ReloadLocalization();
				CHECK(!LocalizationFilesChanged());
				CHECK(Tr("st.b", "fallback") == "fallback");
			}

			// 21l. S3 占位符:`TrFormat` 先查表(含结构化条目)再替换 `{name}`;未知占位符原样保留;
			//      `{{`/`}}` 转义;未命中 = fallback(同样替换/转义)+ 缺键记账;`Tr` 不做替换。
			{
				const fs::path dir = root / "format";
				WriteText(dir / "fmt" / "zh-CN" / "f.json",
					"{ \"fmt.obj\": { \"text\": \"deleted {count} of {name}\" },"
					" \"fmt.esc\": \"{{literal}} {name}\","
					" \"fmt.unknown\": \"keep {missing} tail\" }");
				ClearLocalizationLayers();
				RegisterLocalizationLayer("fmt", dir / "fmt", 0);
				SetLanguage("zh-CN");
				ReloadLocalization();
				const std::vector<std::pair<std::string_view, std::string_view>> args{
					{ "count", "5" }, { "name", "3" } };
				CHECK(TrFormat("fmt.obj", "fallback", args) == "deleted 5 of 3");
				CHECK(TrFormat("fmt.esc", "fallback", args) == "{literal} 3");
				CHECK(TrFormat("fmt.unknown", "fallback", args) == "keep {missing} tail");
				CHECK(TrFormat("fmt.miss", "raw {name}/{unknown} {{x}}", args) == "raw 3/{unknown} {x}");
				CHECK(HasEntry(MissingLocalizationKeys(), "fmt.miss"));
				CHECK(Tr("fmt.obj", "fallback") == "deleted {count} of {name}");   // Tr 原样透传
			}

			// 21m. S3 复数:按语言类别选 `plural` 变体(en: one/other;ru: one/few/many;
			//      zh/ja/ko 与其它语言: other;缺类别回退 other),再替换 `{count}`;
			//      没有 `plural` 的条目 = 文本 + `{count}`;未命中 = fallback + `{count}`。
			{
				const fs::path dir = root / "plural";
				const std::string all =
					"{ \"p.all\": { \"plural\": { \"one\": \"one:{count}\", \"few\": \"few:{count}\","
					" \"many\": \"many:{count}\", \"other\": \"other:{count}\" } },"
					" \"p.only\": { \"plural\": { \"other\": \"only-other:{count}\" } },"
					" \"p.plain\": \"plain:{count}\" }";
				WriteText(dir / "pl" / "ru" / "p.json", all);
				WriteText(dir / "pl" / "zh-CN" / "p.json", all);
				WriteText(dir / "pl" / "de" / "p.json", all);
				ClearLocalizationLayers();
				RegisterLocalizationLayer("pl", dir / "pl", 0);
				SetLanguage("ru");
				ReloadLocalization();
				CHECK(TrPlural("p.all", "fb", 1) == "one:1");
				CHECK(TrPlural("p.all", "fb", 2) == "few:2");
				CHECK(TrPlural("p.all", "fb", 5) == "many:5");
				CHECK(TrPlural("p.all", "fb", 11) == "many:11");
				CHECK(TrPlural("p.all", "fb", 21) == "one:21");
				CHECK(TrPlural("p.all", "fb", 22) == "few:22");
				CHECK(TrPlural("p.all", "fb", 0) == "many:0");
				CHECK(TrPlural("p.only", "fb", 3) == "only-other:3");        // 缺该类别 → other
				CHECK(TrPlural("p.plain", "fb", 4) == "plain:4");            // 没有 plural:文本 + {count}
				CHECK(Tr("p.only", "fb") == "only-other:{count}");           // 只有 plural.other → 当条目文本
				CHECK(TrPlural("p.miss", "miss {count}", 2) == "miss 2");    // 未命中:fallback + {count}
				CHECK(HasEntry(MissingLocalizationKeys(), "p.miss"));
				SetLanguage("zh-CN");
				ReloadLocalization();
				CHECK(TrPlural("p.all", "fb", 1) == "other:1");              // zh:任意数量 → other
				CHECK(TrPlural("p.all", "fb", 5) == "other:5");
				SetLanguage("de");
				ReloadLocalization();
				CHECK(TrPlural("p.all", "fb", 1) == "other:1");              // 未列出的语言 → other
				CHECK(TrPlural("p.all", "fb", 2) == "other:2");
				// 英文族按口径(候选 = 空表)不读语言包,所以 en 类别规则只能在"有词典"时观察:
				// 用单文件入口给一份 en 词典,验证 one/other(真实调用点的英文文案来自内联 fallback)。
				const fs::path enCatalog = dir / "en-catalog.json";
				WriteText(enCatalog, all);
				ClearLocalizationLayers();
				SetLanguage("en");
				CHECK(LoadLocalizationCatalog(enCatalog));
				CHECK(TrPlural("p.all", "fb", 1) == "one:1");
				CHECK(TrPlural("p.all", "fb", 2) == "other:2");
				CHECK(TrPlural("p.all", "fb", 0) == "other:0");
			}

			// 收尾:恢复全局态(清层 + 英文内联),删除夹具目录。
			ClearLocalizationLayers();
			SetLanguage("en");
			CHECK(Tr("panel.hierarchy.title", "Hierarchy") == "Hierarchy");
			std::error_code cleanup;
			fs::remove_all(root, cleanup);
			CHECK(!fs::exists(root));
		}

		// 22. P1c-a:①声明了交互状态的件必须给出 interactive=true 的 a11y 节点(E1 契约);
		//     ②帧内被临时改写过鼠标位置时,按下归属要重新落到**真实位置**上的控件 —— 回归:
		//       工作台伪状态把 MousePos 挪到组件中心并让组件先认领按下,把下拉弹层的逐条
		//       点击整下吃掉(第 3 条起点不动)。
		{
			// ---- ① 七件的交互节点契约 ----
			WuiAccessibility& accessibility = WuiAccessibility::Get();
			accessibility.SetEnabled(true);
			for (const char* componentId : { "button.icon", "segmented", "breadcrumb", "listview",
				"table.header", "tabs", "codeeditor" })
			{
				const WuiComponentDesc* desc = WuiComponentRegistry::Find(componentId);
				CHECK(desc != nullptr);
				if (desc == nullptr)
					continue;
				accessibility.BeginFrame("main", { 640.0f, 480.0f });
				accessibility.SetPanel("showcase");
				WuiContext ctx;
				WuiInputState input;
				input.ViewportSize = { 640.0f, 480.0f };
				ctx.BeginFrame(input);
				WuiComponentDraw draw;
				draw.Context = &ctx;
				draw.Theme = &CurrentTheme();
				draw.Rect = { 12.0f, 12.0f, 420.0f, 160.0f };
				draw.State = "default";
				draw.UiScale = 1.0f;
				draw.Density = 1.0f;
				draw.Locale = "en";
				desc->Showcase(draw);
				ctx.EndFrame();
				bool interactive = false;
				for (const std::string& id : desc->ExtraA11yIds)
				{
					const WuiAccessNode* node = accessibility.Find(HashId(id.c_str()));
					interactive = interactive || (node != nullptr && node->Interactive && node->Visible);
				}
				CHECK(interactive);
			}
			accessibility.SetEnabled(false);
			accessibility.Clear();

			// ---- ② 6 条弹层逐条命中(逐条 press/release 两帧;press 帧里先有一次"假位置认领") ----
			WuiTheme theme;
			const WuiRect rect { 100, 100, 200, 22 };
			const std::vector<std::string> options { "Low", "Medium", "High", "Ultra", "Extreme", "Max" };
			const WuiId id = HashId("test.combo.p1ca");
			const float itemH = 22.0f;
			int selected = 0;
			WuiContext ctx;
			const auto ClickTrigger = [&]()
			{
				WuiInputState input;
				input.MousePos = { rect.X + 10.0f, rect.Y + rect.H * 0.5f };
				input.MouseClicked[0] = true;
				ctx.BeginFrame(input);
				Combo(ctx, id, rect, "Quality", options, selected, theme);
				ctx.EndFrame();
			};
			const auto ItemCenter = [&](size_t index)
			{
				// 弹层条目矩形 = {panel.X+4, panel.Y+4+22*i, panel.W-8, 22},panel 在触发器正下方 2px。
				return glm::vec2 { rect.X + rect.W * 0.5f,
					rect.Y + rect.H + 2.0f + 4.0f + itemH * static_cast<float>(index) + itemH * 0.5f };
			};
			ClickTrigger();
			CHECK(ctx.IsPopupOpen(id));
			for (size_t index = 0; index < options.size(); ++index)
			{
				const glm::vec2 point = ItemCenter(index);
				{
					// press 帧:一个"帧内临时改写输入"的控件先在假鼠标位置上按下了(id=0 的归属),
					// 之后恢复真实输入再画 Combo —— 与工作台 PseudoState 同一条路径。
					WuiInputState input;
					input.MousePos = point;
					input.MouseClicked[0] = true;
					input.MouseDown[0] = true;
					ctx.BeginFrame(input);
					const WuiRect fake { 400, 400, 60, 24 };
					const WuiInputState real = ctx.Input();
					ctx.Input().MousePos = { fake.X + fake.W * 0.5f, fake.Y + fake.H * 0.5f };
					(void)ctx.IsClicked(fake, 0);
					ctx.Input() = real;
					Combo(ctx, id, rect, "Quality", options, selected, theme);
					ctx.EndFrame();
				}
				CHECK(ctx.IsPopupOpen(id));
				{
					WuiInputState input;
					input.MousePos = point;
					input.MouseReleased[0] = true;
					ctx.BeginFrame(input);
					Combo(ctx, id, rect, "Quality", options, selected, theme);
					ctx.EndFrame();
				}
				CHECK(selected == static_cast<int>(index));
				CHECK(!ctx.IsPopupOpen(id));
				if (index + 1 < options.size())
				{
					ClickTrigger();
					CHECK(ctx.IsPopupOpen(id));
				}
			}
		}

		// 23. P1c-E4:键盘可达性契约(引擎侧)。每件都按同一条两帧节奏验证:
		//     ①先画一帧(登记焦点表)→ ②下一帧注入 Tab(以及该件的契约键)→
		//     Tab 必须停到它的焦点 id,且该 id 的 a11y 节点带 focused=true、可见、可读;
		//     按键报告/状态变化由各段自己的回调记录。覆盖 7 件"必须改引擎"
		//     (button.icon / searchfield / codeeditor / listrow / listview / breadcrumb / scrollarea)
		//     加上 segmented/tabs 的子焦点暴露与 treeview 容器节点。
		{
			WuiAccessibility& accessibility = WuiAccessibility::Get();
			accessibility.SetEnabled(true);
			const WuiTheme& theme = CurrentTheme();
			// keys 除 Tab 外的键在**同一帧**注入(控件在焦点已落到自己身上后处理它们)。
			const auto ContractNode = [&](WuiId widgetId, WuiId focusId, std::initializer_list<uint32_t> keys,
				const std::function<void(WuiContext&, WuiId)>& paint) -> const WuiAccessNode*
			{
				WuiContext ctx;
				WuiInputState idle;
				idle.ViewportSize = { 640.0f, 480.0f };
				accessibility.BeginFrame("main", idle.ViewportSize);
				accessibility.SetPanel("test");
				ctx.BeginFrame(idle);
				paint(ctx, widgetId);
				ctx.EndFrame();

				WuiInputState press = idle;
				press.KeyPressed = { static_cast<uint32_t>(World::KeyCodes::Tab) };
				for (uint32_t key : keys)
					press.KeyPressed.push_back(key);
				accessibility.BeginFrame("main", idle.ViewportSize);
				accessibility.SetPanel("test");
				ctx.BeginFrame(press);
				paint(ctx, widgetId);
				ctx.EndFrame();
				CHECK(ctx.Focus() == focusId);
				return accessibility.Find(focusId);
			};

			// ① button.icon:ToolbarIconButton 进焦点表 + Enter/Space 激活(以前从不 RegisterFocusable)。
			{
				const WuiId id = HashId("test.e4.icon-button");
				const WuiRect button { 20.0f, 20.0f, 32.0f, 24.0f };
				bool activated = false;
				const WuiAccessNode* node = ContractNode(id, id,
					{ static_cast<uint32_t>(World::KeyCodes::Space) },
					[&](WuiContext& ctx, WuiId widgetId)
					{
						activated = ToolbarIconButton(ctx, widgetId, button, 0, { 0, 0, 1, 1 }, "Save", theme, true);
					});
				CHECK(node != nullptr);
				CHECK(node != nullptr && node->Kind == "button" && node->Focused && node->Interactive && node->Visible);
				CHECK(activated);
			}

			// ② searchfield:无条件登记焦点/节点(旧实现把焦点登记藏在"已聚焦"分支里 → 先有鸡后有蛋)。
			{
				const WuiId id = HashId("test.e4.search");
				const WuiRect search { 20.0f, 20.0f, 200.0f, 24.0f };
				std::string buffer;
				const WuiAccessNode* node = ContractNode(id, id, {},
					[&](WuiContext& ctx, WuiId widgetId)
					{
						SearchField(ctx, widgetId, search, buffer, "Search assets", theme);
					});
				CHECK(node != nullptr && node->Kind == "text-field" && node->Focused && node->Interactive);
				CHECK(node != nullptr && node->Label == "Search assets" && node->Value == "Search assets");
			}

			// ③ codeeditor:补焦点表入口(节点与 focused 本来就有,Tab 进不来)。
			{
				const WuiId id = HashId("test.e4.code-editor");
				const WuiRect editor { 20.0f, 20.0f, 240.0f, 80.0f };
				WuiTextBuffer buffer;
				buffer.SetText("local x = 1\n");
				WuiCodeEditorOptions options;
				const WuiAccessNode* node = ContractNode(id, id, {},
					[&](WuiContext& ctx, WuiId widgetId)
					{
						CodeEditor(ctx, widgetId, editor, buffer, options);
					});
				CHECK(node != nullptr && node->Kind == "code-editor" && node->Focused);
			}

			// ④ listrow(WuiListRow):行进焦点表,Enter 与点击走同一个 OnClick。
			{
				const WuiId id = HashId("test.e4.list-row");
				int clicks = 0;
				auto row = std::make_shared<WuiListRow>();
				row->Text = "Row";
				row->SetId(id);
				row->OnClick = [&clicks]() { ++clicks; };
				LayoutWidgetTree(row, { 20.0f, 20.0f, 200.0f, 22.0f });
				const WuiAccessNode* node = ContractNode(id, id,
					{ static_cast<uint32_t>(World::KeyCodes::Enter) },
					[&](WuiContext& ctx, WuiId)
					{
						WuiPaintContext paint(ctx);
						row->Paint(paint);
					});
				CHECK(node != nullptr && node->Kind == "list-row" && node->Focused);
				CHECK(clicks == 1);
			}

			// ⑤ listview:容器节点 + 键盘光标(↑/↓ 只报告 KeyMoveTo,改选中归调用方)。
			{
				const WuiId id = HashId("test.e4.list");
				std::vector<ListViewItem> items(3);
				items[0].Id = HashId("test.e4.list.0");
				items[0].Label = "One";
				items[0].Selected = true;
				items[1].Id = HashId("test.e4.list.1");
				items[1].Label = "Two";
				items[2].Id = HashId("test.e4.list.2");
				items[2].Label = "Three";
				items[2].Disabled = true;   // 末行禁用:↓ 不许停到它
				const WuiRect list { 20.0f, 20.0f, 200.0f, 96.0f };
				float scroll = 0.0f;
				int moveTo = -1;
				int activate = -1;
				const WuiAccessNode* node = ContractNode(id, id,
					{ static_cast<uint32_t>(World::KeyCodes::Down) },
					[&](WuiContext& ctx, WuiId widgetId)
					{
						const ListViewResult result = ListView(ctx, list, items, 24.0f, scroll, theme, widgetId);
						moveTo = result.KeyMoveTo;
						activate = result.KeyActivate;
					});
				CHECK(node != nullptr && node->Kind == "list" && node->Focused && node->Interactive);
				CHECK(moveTo == 1 && activate == -1);
				const WuiAccessNode* current = accessibility.Find(items[0].Id);
				CHECK(current != nullptr && current->Focused);   // 当前行的焦点位可读
			}

			// ⑥ treeview:容器节点 kind="tree" + 焦点位(行节点同时在焦点于树上时标 focused)。
			{
				const WuiId id = HashId("test.e4.tree");
				std::vector<TreeViewItem> items(3);
				items[0] = { HashId("test.e4.tree.0"), "Assets", 0, true, true, true, false };
				items[1] = { HashId("test.e4.tree.1"), "Textures", 1, false, false, false, false };
				items[2] = { HashId("test.e4.tree.2"), "Materials", 1, false, false, false, false };
				const WuiRect treeArea { 20.0f, 20.0f, 200.0f, 96.0f };
				float scroll = 0.0f;
				int toggle = -1;
				const WuiAccessNode* node = ContractNode(id, id,
					{ static_cast<uint32_t>(World::KeyCodes::Right) },
					[&](WuiContext& ctx, WuiId widgetId)
					{
						const TreeViewResult result = TreeView(ctx, treeArea, items, 22.0f, scroll, theme, widgetId);
						toggle = result.KeyToggleExpand;
					});
				CHECK(node != nullptr && node->Kind == "tree" && node->Focused && node->Interactive);
				CHECK(toggle == 0);   // 当前项(有子节点)收到 ←/→ 的折叠报告
				const WuiAccessNode* current = accessibility.Find(items[0].Id);
				CHECK(current != nullptr && current->Focused);
			}

			// ⑦ breadcrumb:id 下沉后由控件自己登记节点并进焦点表;←/→ 移段光标、Enter 激活它。
			{
				const WuiId id = HashId("test.e4.breadcrumb");
				const WuiRect crumb { 20.0f, 20.0f, 230.0f, 20.0f };
				int picked = -1;
				const WuiAccessNode* node = ContractNode(id, id,
					{ static_cast<uint32_t>(World::KeyCodes::Right), static_cast<uint32_t>(World::KeyCodes::Enter) },
					[&](WuiContext& ctx, WuiId widgetId)
					{
						const int clicked = Breadcrumb(ctx, crumb, "assets/textures/icon.png", theme, widgetId);
						if (clicked >= 0)
							picked = clicked;
					});
				CHECK(node != nullptr && node->Kind == "breadcrumb" && node->Focused && node->Interactive);
				CHECK(picked == 1);   // 段光标从第 0 段推到第 1 段,Enter 激活它
			}

			// ⑧ scrollarea:id 下沉后进焦点表 + 键盘滚动(以前只有"悬停 + 滚轮")。
			{
				const WuiId id = HashId("test.e4.scroll");
				const WuiRect viewport { 20.0f, 20.0f, 200.0f, 96.0f };
				float scroll = 0.0f;
				const WuiAccessNode* node = ContractNode(id, id,
					{ static_cast<uint32_t>(World::KeyCodes::Down) },
					[&](WuiContext& ctx, WuiId widgetId)
					{
						BeginScrollArea(ctx, viewport, 400.0f, scroll, theme, widgetId);
						EndScrollArea(ctx);
					});
				CHECK(node != nullptr && node->Kind == "scroll-area" && node->Focused);
				CHECK(node != nullptr && !node->Interactive);   // 容器自己不是点击目标
				CHECK(Near(scroll, 40.0f));                     // ↓ = 40px
			}

			// ⑨ segmented / tabs:焦点停在**子项**上 —— 组节点按"组内有焦点"报 focused,
			//    子节点按"焦点在这一格/这一页"报 focused(以前子节点恒 false,AI 读不到焦点位)。
			{
				const WuiId segmentedId = HashId("test.e4.segmented");
				const WuiId segmentChild = HashId((std::to_string(segmentedId) + ".segment.0").c_str());
				const std::vector<std::string> options { "Light", "Medium", "Heavy" };
				int selected = 0;
				const WuiRect bar { 20.0f, 20.0f, 210.0f, 24.0f };
				ContractNode(segmentedId, segmentChild, {},
					[&](WuiContext& ctx, WuiId widgetId)
					{
						Segmented(ctx, widgetId, bar, options, selected, theme);
					});
				const WuiAccessNode* group = accessibility.Find(segmentedId);
				const WuiAccessNode* option = accessibility.Find(segmentChild);
				CHECK(group != nullptr && group->Kind == "segmented" && group->Focused);
				CHECK(option != nullptr && option->Kind == "segmented-option" && option->Focused);

				const WuiId tabsId = HashId("test.e4.tabs");
				const WuiId tabChild = HashId((std::to_string(tabsId) + ".tab.0").c_str());
				int active = 0;
				ContractNode(tabsId, tabChild, {},
					[&](WuiContext& ctx, WuiId widgetId)
					{
						TabBar(ctx, widgetId, bar, options, active, theme, nullptr);
					});
				const WuiAccessNode* tabBar = accessibility.Find(tabsId);
				const WuiAccessNode* tab = accessibility.Find(tabChild);
				CHECK(tabBar != nullptr && tabBar->Kind == "tab-bar" && tabBar->Focused);
				CHECK(tab != nullptr && tab->Kind == "tab" && tab->Focused);
			}

			accessibility.SetEnabled(false);
			accessibility.Clear();
		}

		// 24. P1c-E4-fix:①单行文本框按 Tab 交还焦点(ReleaseOnTab 标记)—— 工作台搜索框这类
		//     单行框不再把正向 Tab 吞掉;②多行 CodeEditor 不带标记 ⇒ Tab 仍归编辑器(缩进语义不变);
		//     ③WuiSeparator 登记进组件表,showcase 走保留模式控件的真实绘制路径。
		{
			WuiAccessibility& accessibility = WuiAccessibility::Get();
			accessibility.SetEnabled(true);
			const WuiTheme& theme = CurrentTheme();
			const WuiRect fieldRect { 20.0f, 20.0f, 200.0f, 24.0f };
			const WuiRect buttonRect { 20.0f, 60.0f, 96.0f, 24.0f };
			const WuiId fieldId = HashId("test.e4f.single-line");
			const WuiId buttonId = HashId("test.e4f.button");
			WuiInputState idle;
			idle.ViewportSize = { 640.0f, 480.0f };

			// ① 单行 TextField:第 1 帧聚焦 → 登记 ReleaseOnTab;第 2 帧 Tab → 焦点交给表里的下一个。
			{
				WuiContext ctx;
				std::string buffer = "abc";
				const auto paint = [&](WuiContext& target)
				{
					TextField(target, fieldId, fieldRect, buffer, theme);
					Button(target, buttonId, buttonRect, "Next", theme);
				};
				accessibility.BeginFrame("main", idle.ViewportSize);
				accessibility.SetPanel("test");
				ctx.BeginFrame(idle);
				ctx.SetFocus(fieldId);
				paint(ctx);
				ctx.EndFrame();
				CHECK(ctx.Focus() == fieldId);
				CHECK(WuiTextFocus::Get().Active());
				CHECK(WuiTextFocus::Get().ReleaseOnTab());   // 单行框带 Tab 交还标记

				WuiInputState tab = idle;
				tab.KeyPressed = { static_cast<uint32_t>(World::KeyCodes::Tab) };
				accessibility.BeginFrame("main", idle.ViewportSize);
				accessibility.SetPanel("test");
				ctx.BeginFrame(tab);
				paint(ctx);
				ctx.EndFrame();
				CHECK(ctx.Focus() == buttonId);            // Tab 交还 → 焦点落到表里的下一个控件
				CHECK(!WuiTextFocus::Get().Active());       // 单行框不再持焦 ⇒ 文本焦点登记也清掉
			}

			// ② 多行 CodeEditor:同一条 Tab 注入,焦点仍留在编辑器(未带标记 ⇒ Tab=缩进,焦点表不抢)。
			{
				const WuiId editorId = HashId("test.e4f.code-editor");
				const WuiRect editorRect { 20.0f, 20.0f, 240.0f, 80.0f };
				WuiContext ctx;
				WuiTextBuffer buffer;
				buffer.SetText("local x = 1\n");
				WuiCodeEditorOptions options;
				const auto paint = [&](WuiContext& target)
				{
					CodeEditor(target, editorId, editorRect, buffer, options);
					Button(target, buttonId, buttonRect, "Next", theme);
				};
				ctx.BeginFrame(idle);
				ctx.SetFocus(editorId);
				paint(ctx);
				ctx.EndFrame();
				CHECK(WuiTextFocus::Get().Active());
				CHECK(!WuiTextFocus::Get().ReleaseOnTab());    // 多行编辑器不带标记

				WuiInputState tab = idle;
				tab.KeyPressed = { static_cast<uint32_t>(World::KeyCodes::Tab) };
				ctx.BeginFrame(tab);
				paint(ctx);
				ctx.EndFrame();
				CHECK(ctx.Focus() == editorId);                // Tab 没被焦点表抢走
				CHECK(WuiTextFocus::Get().Active());
			}

			// ③ WuiSeparator:登记表条目存在,showcase 画出的就是控件自己的那一条线(1px/220 宽),
			//    外壳锚点 kind=component-root、interactive=false(分隔线不是输入目标)。
			{
				const WuiComponentDesc* separator = WuiComponentRegistry::Find("separator");
				CHECK(separator != nullptr);
				CHECK(separator != nullptr && separator->TypeName == "WuiSeparator");
				WuiContext ctx;
				accessibility.BeginFrame("main", idle.ViewportSize);
				accessibility.SetPanel("showcase");
				ctx.BeginFrame(idle);
				WuiComponentDraw draw;
				draw.Context = &ctx;
				draw.Theme = &theme;
				draw.Rect = { 12.0f, 12.0f, 420.0f, 160.0f };
				draw.State = "default";
				draw.UiScale = 1.0f;
				draw.Density = 1.0f;
				draw.Locale = "en";
				separator->Showcase(draw);
				ctx.EndFrame();
				const std::vector<WuiDrawCommand>& commands = ctx.Commands();
				CHECK(commands.size() == 1);
				CHECK(commands[0].Kind == WuiDrawKind::Rect);
				CHECK(Near(commands[0].Rect.W, 220.0f));
				CHECK(Near(commands[0].Rect.H, 1.0f));         // 默认 thickness=1
				const WuiAccessNode* anchor = accessibility.Find(HashId("showcase.separator"));
				CHECK(anchor != nullptr && anchor->Kind == "component-root" && !anchor->Interactive);
			}

			WuiTextFocus::Get().Clear();
			accessibility.SetEnabled(false);
			accessibility.Clear();
		}

		// 24. P1c-LIB1:徽标 / 染色图标 / 即时裁剪作用域 —— 三条硬口径:
		//     ① 登记条目可解析(id/TypeName/Category;通用字段由 §19 覆盖);
		//     ② showcase 走真实库入口:badge 同一帧画"默认 + 变色"两枚、字形默认粗体;
		//        icon 的空图(textureId=0)走兜底占位,有纹理时只发一条带 tint 的 Image 命令;
		//     ③ ClipScope 与手写的 ClipPush/ClipPop 命令逐字段等价,且裁剪栈同进同出
		//        (ClipAllows 在作用域内/外、嵌套时的判据都正确)。
		{
			WuiAccessibility& accessibility = WuiAccessibility::Get();
			accessibility.SetEnabled(true);
			const WuiTheme theme = CurrentTheme();

			// ---- ① badge:登记 + "默认 + 变色"两枚 + 粗体字标 + 外壳锚点 ----
			{
				const WuiComponentDesc* badge = WuiComponentRegistry::Find("badge");
				CHECK(badge != nullptr);
				if (badge != nullptr)
				{
					CHECK(badge->TypeName == "Badge");
					CHECK(badge->Category == "Chrome");
					WuiContext ctx;
					WuiInputState input;
					input.ViewportSize = { 640.0f, 480.0f };
					accessibility.BeginFrame("main", input.ViewportSize);
					accessibility.SetPanel("showcase");
					ctx.BeginFrame(input);
					WuiComponentDraw draw;
					draw.Context = &ctx;
					draw.Theme = &theme;
					draw.Rect = { 12.0f, 12.0f, 420.0f, 160.0f };
					draw.State = "default";
					draw.UiScale = 1.0f;
					draw.Density = 1.0f;
					draw.Locale = "en";
					badge->Showcase(draw);
					ctx.EndFrame();
					const std::vector<WuiDrawCommand> commands = ctx.Commands();
					int rects = 0;
					int boldTexts = 0;
					std::set<std::string> fills;
					for (const WuiDrawCommand& command : commands)
					{
						if (command.Kind == WuiDrawKind::Rect)
						{
							++rects;
							fills.insert(std::to_string(command.Color.R) + "," + std::to_string(command.Color.G)
								+ "," + std::to_string(command.Color.B));
						}
						else if (command.Kind == WuiDrawKind::Text && command.Bold)
							++boldTexts;
					}
					CHECK(rects >= 2);        // 默认底 + 变色底
					CHECK(fills.size() >= 2); // "默认 + 变色"确实是两种底色
					CHECK(boldTexts >= 2);    // 两枚字形都走粗体
					const WuiAccessNode* anchor = accessibility.Find(HashId("showcase.badge"));
					CHECK(anchor != nullptr && anchor->Kind == "component-root" && !anchor->Interactive);
				}
			}

			// ---- ② icon:空图兜底占位 / 有纹理时 tint 逐字段透传 ----
			{
				const WuiComponentDesc* icon = WuiComponentRegistry::Find("icon");
				CHECK(icon != nullptr);
				if (icon != nullptr)
				{
					CHECK(icon->TypeName == "Icon");
					// 默认属性(textureId=0):底 + 2×2 棋盘 + 描边,且不发 Image 命令。
					{
						WuiContext ctx;
						WuiInputState input;
						input.ViewportSize = { 640.0f, 480.0f };
						accessibility.BeginFrame("main", input.ViewportSize);
						accessibility.SetPanel("showcase");
						ctx.BeginFrame(input);
						WuiComponentDraw draw;
						draw.Context = &ctx;
						draw.Theme = &theme;
						draw.Rect = { 12.0f, 12.0f, 420.0f, 160.0f };
						draw.State = "default";
						draw.UiScale = 1.0f;
						draw.Density = 1.0f;
						draw.Locale = "en";
						icon->Showcase(draw);
						ctx.EndFrame();
						const std::vector<WuiDrawCommand> commands = ctx.Commands();
						int rects = 0;
						int outlines = 0;
						int images = 0;
						for (const WuiDrawCommand& command : commands)
						{
							if (command.Kind == WuiDrawKind::Rect) ++rects;
							if (command.Kind == WuiDrawKind::RectOutline) ++outlines;
							if (command.Kind == WuiDrawKind::Image) ++images;
						}
						CHECK(rects >= 3);    // 底 + 两块棋盘
						CHECK(outlines >= 1); // 描边
						CHECK(images == 0);   // 空图不画 Image
						const WuiAccessNode* anchor = accessibility.Find(HashId("showcase.icon"));
						CHECK(anchor != nullptr && anchor->Kind == "component-root" && !anchor->Interactive);
					}
					// 有纹理 + tint:只发一条 Image 命令,tint / textureId / UV 逐字段透传。
					{
						WuiContext ctx;
						WuiInputState input;
						input.ViewportSize = { 640.0f, 480.0f };
						accessibility.BeginFrame("main", input.ViewportSize);
						accessibility.SetPanel("showcase");
						ctx.BeginFrame(input);
						WuiComponentDraw draw;
						draw.Context = &ctx;
						draw.Theme = &theme;
						draw.Rect = { 12.0f, 12.0f, 420.0f, 160.0f };
						draw.State = "default";
						draw.UiScale = 1.0f;
						draw.Density = 1.0f;
						draw.Locale = "en";
						draw.Properties = { { "textureId", "1234" }, { "tint", "#FF0000" } };
						icon->Showcase(draw);
						ctx.EndFrame();
						const std::vector<WuiDrawCommand> commands = ctx.Commands();
						CHECK(commands.size() == 1);
						if (!commands.empty())
						{
							const WuiDrawCommand& image = commands[0];
							CHECK(image.Kind == WuiDrawKind::Image);
							CHECK(image.Image == 1234u);
							CHECK(Near(image.Color.R, 1.0f) && Near(image.Color.G, 0.0f) && Near(image.Color.B, 0.0f));
							CHECK(Near(image.Uv.W, 1.0f) && Near(image.Uv.H, 1.0f));
						}
					}
				}
			}

			// ---- ③ ClipScope:与手写 ClipPush/ClipPop 逐字段等价 + 裁剪栈同进同出 ----
			{
				WuiContext ctx;
				WuiInputState input;
				input.ViewportSize = { 640.0f, 480.0f };
				ctx.BeginFrame(input);
				const WuiRect clipRect { 10.0f, 20.0f, 100.0f, 50.0f };
				// 手写版(与面板里现存的裸写法逐字段相同):ClipPush + PushClipRect … PopClipRect + ClipPop。
				ctx.Commands().push_back({ WuiDrawKind::ClipPush, clipRect });
				ctx.PushClipRect(clipRect);
				ctx.Commands().push_back({ WuiDrawKind::ClipPop });
				ctx.PopClipRect();
				{
					ClipScope clip(ctx, clipRect);
					(void)clip;
					CHECK(ctx.ClipAllows({ 30.0f, 30.0f, 10.0f, 10.0f }));
					CHECK(!ctx.ClipAllows({ 200.0f, 30.0f, 10.0f, 10.0f }));
					const WuiDrawCommand& push = ctx.Commands().back();
					CHECK(push.Kind == WuiDrawKind::ClipPush);
					CHECK(Near(push.Rect.X, clipRect.X) && Near(push.Rect.Y, clipRect.Y)
						&& Near(push.Rect.W, clipRect.W) && Near(push.Rect.H, clipRect.H));
				}
				CHECK(ctx.ClipAllows({ 200.0f, 30.0f, 10.0f, 10.0f })); // 出作用域 = 不裁剪
				CHECK(ctx.Commands().size() == 4);
				CHECK(ctx.Commands()[0].Kind == WuiDrawKind::ClipPush && ctx.Commands()[1].Kind == WuiDrawKind::ClipPop);
				CHECK(ctx.Commands()[2].Kind == WuiDrawKind::ClipPush && ctx.Commands()[3].Kind == WuiDrawKind::ClipPop);
				CHECK(Near(ctx.Commands()[2].Rect.X, clipRect.X) && Near(ctx.Commands()[2].Rect.W, clipRect.W));
				ctx.EndFrame();
			}
			// 嵌套 = LIFO:内层裁剪生效、出内层恢复外层(与 PushClipRect/PopClipRect 同语义)。
			{
				WuiContext ctx;
				WuiInputState input;
				input.ViewportSize = { 640.0f, 480.0f };
				ctx.BeginFrame(input);
				{
					ClipScope outer(ctx, { 0.0f, 0.0f, 100.0f, 100.0f });
					(void)outer;
					CHECK(ctx.ClipAllows({ 10.0f, 10.0f, 10.0f, 10.0f }));
					{
						ClipScope inner(ctx, { 0.0f, 0.0f, 20.0f, 20.0f });
						(void)inner;
						CHECK(ctx.ClipAllows({ 10.0f, 10.0f, 10.0f, 10.0f }));
						CHECK(!ctx.ClipAllows({ 50.0f, 50.0f, 10.0f, 10.0f }));
					}
					CHECK(ctx.ClipAllows({ 50.0f, 50.0f, 10.0f, 10.0f }));
				}
				CHECK(ctx.ClipAllows({ 500.0f, 500.0f, 10.0f, 10.0f }));
				CHECK(ctx.Commands().size() == 4); // 两次 push + 两次 pop,无残留
				ctx.EndFrame();
			}

			accessibility.SetEnabled(false);
			accessibility.Clear();
		}

		// 25. P1c-LIB2:渐变填充 / 可折叠分区标题 / 禁用+理由按钮 / 有状态滚动条 —— 四条硬口径:
		//     ① GradientFill 与面板手写的 Gradient 命令逐字段等价(矩形 + 四角颜色/顺序),
		//        双色便捷版的两个方向各自等价于把 from/to 复制到对应两角;
		//     ② CollapsibleHeader 点击/键盘(Enter)都切换 open,稳定 a11y id 的 value 跟着 open/closed;
		//     ③ ButtonEx 禁用时点击不触发、节点 Enabled=false 且 Value/Tooltip 都带理由;
		//        启用时点击与键盘激活都能触发;
		//     ④ ScrollBar 的值字符串读得出("scroll=<y>/<max> ratio=…",与 BeginScrollArea 同前缀),
		//        拖动/点击轨道/键盘都能设值,内容装得下时节点 Enabled=false。
		{
			WuiAccessibility& accessibility = WuiAccessibility::Get();
			accessibility.SetEnabled(true);
			const WuiTheme theme = CurrentTheme();
			WuiInputState base;
			base.ViewportSize = { 640.0f, 480.0f };

			// ---- ① GradientFill:单条命令 + 四角顺序 + 双色方向 ----
			{
				WuiContext ctx;
				ctx.BeginFrame(base);
				const WuiRect rect { 10.0f, 20.0f, 120.0f, 60.0f };
				const WuiColor tl { 1, 0, 0, 1 };
				const WuiColor tr { 0, 1, 0, 1 };
				const WuiColor br { 0, 0, 1, 1 };
				const WuiColor bl { 1, 1, 1, 1 };
				GradientFill(ctx, rect, tl, tr, br, bl);
				CHECK(ctx.Commands().size() == 1);
				const WuiDrawCommand& command = ctx.Commands()[0];
				CHECK(command.Kind == WuiDrawKind::Gradient);
				CHECK(Near(command.Rect.X, rect.X) && Near(command.Rect.Y, rect.Y)
					&& Near(command.Rect.W, rect.W) && Near(command.Rect.H, rect.H));
				CHECK(Near(command.Corners[0].R, 1.0f) && Near(command.Corners[0].G, 0.0f));
				CHECK(Near(command.Corners[1].G, 1.0f) && Near(command.Corners[1].R, 0.0f));
				CHECK(Near(command.Corners[2].B, 1.0f) && Near(command.Corners[2].R, 0.0f));
				CHECK(Near(command.Corners[3].R, 1.0f) && Near(command.Corners[3].G, 1.0f)
					&& Near(command.Corners[3].B, 1.0f));
				// 纵向双色:上排两角 = from、下排两角 = to;横向双色:左列 = from、右列 = to。
				const WuiColor from { 1, 0, 0, 1 };
				const WuiColor to { 0, 0, 1, 1 };
				GradientFill(ctx, rect, from, to);
				const WuiDrawCommand& vertical = ctx.Commands().back();
				CHECK(Near(vertical.Corners[0].R, 1.0f) && Near(vertical.Corners[1].R, 1.0f)
					&& Near(vertical.Corners[2].B, 1.0f) && Near(vertical.Corners[3].B, 1.0f));
				GradientFill(ctx, rect, from, to, false);
				const WuiDrawCommand& horizontal = ctx.Commands().back();
				CHECK(Near(horizontal.Corners[0].R, 1.0f) && Near(horizontal.Corners[3].R, 1.0f)
					&& Near(horizontal.Corners[1].B, 1.0f) && Near(horizontal.Corners[2].B, 1.0f));
				ctx.EndFrame();
			}

			// ---- ② CollapsibleHeader:点击 / 键盘切换 + 稳定 a11y id 的 open/closed ----
			{
				const WuiId id = HashId("test.lib2.section");
				const WuiRect rect { 20.0f, 20.0f, 200.0f, 24.0f };
				bool open = true;
				WuiContext ctx;
				{
					WuiInputState click = base;
					click.MousePos = { rect.X + 40.0f, rect.Y + rect.H * 0.5f };
					click.MouseClicked[0] = true;
					accessibility.BeginFrame("main", click.ViewportSize);
					accessibility.SetPanel("showcase");
					ctx.BeginFrame(click);
					CHECK(CollapsibleHeader(ctx, id, rect, "Transform", open, theme));
					ctx.EndFrame();
					CHECK(!open);
					const WuiAccessNode* node = accessibility.Find(id);
					CHECK(node != nullptr);
					if (node != nullptr)
					{
						CHECK(node->Kind == "button" && node->Label == "Transform");
						CHECK(node->Value == "closed" && node->Enabled && node->Interactive);
						CHECK(Near(node->Rect.W, rect.W));   // 节点矩形就是传入的整行
					}
				}
				{
					WuiInputState key = base;
					key.KeyPressed = { static_cast<uint32_t>(World::KeyCodes::Enter) };
					accessibility.BeginFrame("main", key.ViewportSize);
					accessibility.SetPanel("showcase");
					ctx.BeginFrame(key);
					ctx.SetFocus(id);
					CHECK(CollapsibleHeader(ctx, id, rect, "Transform", open, theme));
					ctx.EndFrame();
					CHECK(open);
					const WuiAccessNode* node = accessibility.Find(id);
					CHECK(node != nullptr && node->Value == "open");
				}
				// 展开/折叠 = 两种主题底色(ActiveBg / PanelHeader),且不画 SectionHeader 那条底部分隔线。
				{
					WuiContext openCtx;
					openCtx.BeginFrame(base);
					bool openState = true;
					CollapsibleHeader(openCtx, id, rect, "Transform", openState, theme);
					openCtx.EndFrame();
					WuiContext closedCtx;
					closedCtx.BeginFrame(base);
					bool closedState = false;
					CollapsibleHeader(closedCtx, id, rect, "Transform", closedState, theme);
					closedCtx.EndFrame();
					CHECK(openCtx.Commands().size() == closedCtx.Commands().size());
					CHECK(Near(openCtx.Commands()[0].Color.R, theme.ActiveBg.R));
					CHECK(Near(closedCtx.Commands()[0].Color.R, theme.PanelHeader.R));
					CHECK(!Near(theme.ActiveBg.R, theme.PanelHeader.R));
				}
			}

			// ---- ③ ButtonEx:禁用理由进 a11y,启用态点击/键盘都能触发 ----
			{
				const WuiId id = HashId("test.lib2.button");
				const WuiRect rect { 20.0f, 20.0f, 140.0f, 24.0f };
				const std::string reason = "Select a material instance first";
				{
					WuiContext ctx;
					WuiInputState click = base;
					click.MousePos = { rect.X + 10.0f, rect.Y + rect.H * 0.5f };
					click.MouseClicked[0] = true;
					accessibility.BeginFrame("main", click.ViewportSize);
					accessibility.SetPanel("showcase");
					ctx.BeginFrame(click);
					CHECK(!ButtonEx(ctx, id, rect, "Assign", theme, false, false, reason));
					ctx.EndFrame();
					const WuiAccessNode* node = accessibility.Find(id);
					CHECK(node != nullptr);
					if (node != nullptr)
					{
						CHECK(node->Kind == "button" && node->Label == "Assign");
						CHECK(!node->Enabled && node->Interactive);
						CHECK(node->Value == reason && node->Tooltip == reason);
					}
				}
				{
					WuiContext ctx;
					WuiInputState click = base;
					click.MousePos = { rect.X + 10.0f, rect.Y + rect.H * 0.5f };
					click.MouseClicked[0] = true;
					accessibility.BeginFrame("main", click.ViewportSize);
					accessibility.SetPanel("showcase");
					ctx.BeginFrame(click);
					CHECK(ButtonEx(ctx, id, rect, "Assign", theme, true, false, reason));
					ctx.EndFrame();
					const WuiAccessNode* node = accessibility.Find(id);
					CHECK(node != nullptr && node->Enabled);
				}
				{
					WuiContext ctx;
					WuiInputState key = base;
					key.KeyPressed = { static_cast<uint32_t>(World::KeyCodes::Enter) };
					ctx.BeginFrame(key);
					ctx.SetFocus(id);
					CHECK(ButtonEx(ctx, id, rect, "Assign", theme));
					ctx.EndFrame();
				}
			}

			// ---- ④ ScrollBar:值可读 + 拖动/轨道点击/键盘可设 + 无滚动量时禁用 ----
			{
				const WuiId id = HashId("test.lib2.scrollbar");
				const WuiRect rect { 500.0f, 20.0f, 12.0f, 168.0f };
				const float content = 400.0f;
				const float viewport = 200.0f;
				float scroll = 60.0f;
				WuiContext ctx;
				const auto frame = [&](WuiContext& target, const WuiInputState& input)
				{
					accessibility.BeginFrame("main", input.ViewportSize);
					accessibility.SetPanel("showcase");
					target.BeginFrame(input);
					const bool changed = ScrollBar(target, id, rect, content, viewport, scroll, theme);
					target.EndFrame();
					return changed;
				};
				// 默认帧:值字符串与 BeginScrollArea 同前缀(位置/比例都读得到)。
				{
					WuiInputState idle = base;
					frame(ctx, idle);
					const WuiAccessNode* node = accessibility.Find(id);
					CHECK(node != nullptr);
					if (node != nullptr)
					{
						CHECK(node->Kind == "scrollbar");
						CHECK(node->Value == "scroll=60/200 ratio=0.30");
						CHECK(node->Enabled && node->Interactive);
					}
				}
				// 按下滑块(y≈100 落在滑块内)→ 下一帧拖到 y=114 = 往下滚。
				{
					WuiInputState press = base;
					press.MousePos = { rect.X + 6.0f, 100.0f };
					press.MouseClicked[0] = true;
					press.MouseDown[0] = true;
					frame(ctx, press);
					CHECK(Near(scroll, 60.0f));   // 按下滑块不跳值
					WuiInputState drag = base;
					drag.MousePos = { rect.X + 6.0f, 105.0f };
					drag.MouseDown[0] = true;
					CHECK(frame(ctx, drag));
					CHECK(scroll > 60.0f && scroll < 200.0f);
					WuiInputState release = base;
					release.MousePos = drag.MousePos;
					release.MouseReleased[0] = true;
					frame(ctx, release);
				}
				// 点轨道空白(滑块之外)= 直接定位(脚本可用的"设值"入口)。
				{
					scroll = 0.0f;
					WuiInputState jump = base;
					jump.MousePos = { rect.X + 6.0f, 154.0f };
					jump.MouseClicked[0] = true;
					jump.MouseDown[0] = true;
					CHECK(frame(ctx, jump));
					CHECK(Near(scroll, 200.0f));   // 轨道空白 = 直接定位;越界夹取到两端
					WuiInputState release = base;
					release.MousePos = jump.MousePos;
					release.MouseReleased[0] = true;
					frame(ctx, release);
				}
				// 键盘:焦点在滚动条上时 Home/End = 两端(与 BeginScrollArea 同键位)。
				{
					WuiInputState home = base;
					home.KeyPressed = { static_cast<uint32_t>(World::KeyCodes::Home) };
					ctx.BeginFrame(home);
					ctx.SetFocus(id);
					ScrollBar(ctx, id, rect, content, viewport, scroll, theme);
					ctx.EndFrame();
					CHECK(Near(scroll, 0.0f));
					WuiInputState end = base;
					end.KeyPressed = { static_cast<uint32_t>(World::KeyCodes::End) };
					ctx.BeginFrame(end);
					ctx.SetFocus(id);
					ScrollBar(ctx, id, rect, content, viewport, scroll, theme);
					ctx.EndFrame();
					CHECK(Near(scroll, 200.0f));
					const WuiAccessNode* node = accessibility.Find(id);
					CHECK(node != nullptr && node->Value == "scroll=200/200 ratio=1.00");
				}
				// 内容装得下(没有滚动量):节点 Enabled/Interactive=false,值仍是可读的 0/0。
				{
					float none = 0.0f;
					WuiInputState idle = base;
					accessibility.BeginFrame("main", idle.ViewportSize);
					accessibility.SetPanel("showcase");
					ctx.BeginFrame(idle);
					CHECK(!ScrollBar(ctx, id, rect, 160.0f, 200.0f, none, theme));
					ctx.EndFrame();
					const WuiAccessNode* node = accessibility.Find(id);
					CHECK(node != nullptr);
					if (node != nullptr)
					{
						CHECK(!node->Enabled && !node->Interactive);
						CHECK(node->Value == "scroll=0/0 ratio=0.00");
					}
				}
			}

			// ---- ⑤ 登记表:四条新库件都在登记表里(showcase 的真实控件路径由 §20 的通用循环跑) ----
			{
				const WuiComponentDesc* gradient = WuiComponentRegistry::Find("gradient");
				const WuiComponentDesc* collapsible = WuiComponentRegistry::Find("collapsible");
				const WuiComponentDesc* buttonEx = WuiComponentRegistry::Find("button.disabled");
				const WuiComponentDesc* scrollbar = WuiComponentRegistry::Find("scrollbar");
				CHECK(gradient != nullptr && gradient->TypeName == "GradientFill");
				CHECK(collapsible != nullptr && collapsible->TypeName == "CollapsibleHeader");
				CHECK(buttonEx != nullptr && buttonEx->TypeName == "ButtonEx");
				CHECK(scrollbar != nullptr && scrollbar->TypeName == "ScrollBar");
				CHECK(gradient != nullptr && gradient->Category == "Chrome");
				CHECK(collapsible != nullptr && collapsible->Category == "Containers");
				CHECK(scrollbar != nullptr && scrollbar->Category == "Containers");
			}

			accessibility.SetEnabled(false);
			accessibility.Clear();
		}

		// 26. P1c-LIB3:① 线段原语 LineSegment 与 ViewportPanel 手写的"投影线段四边形"逐字段等价
		//     (Kind/Color/Vertices 三项 + 退化阈值 0.5 + 其它字段保持默认);
		//     ② ButtonEx 禁用态不进 Tab 焦点链(焦点步骤里一次都不出现),a11y 仍可读
		//     (Enabled=false + Value/Tooltip=理由);启用态与普通 Button 同一条登记路径。
		{
			WuiAccessibility& accessibility = WuiAccessibility::Get();
			accessibility.SetEnabled(true);
			const WuiTheme theme = CurrentTheme();
			WuiInputState base;
			base.ViewportSize = { 640.0f, 480.0f };
			const WuiColor color { 0.9f, 0.75f, 0.5f, 1.0f };

			// ---- ① LineSegment:一条 Quad 命令 + 顶点公式(斜线用字面量钉死)----
			{
				WuiContext ctx;
				ctx.BeginFrame(base);
				CHECK(ctx.Commands().empty());
				// 斜线:from(0,0)→to(30,40),|d|=50,厚度 6 ⇒ 法线=(-40,30)/50,偏移=法线*3=(-2.4,1.8)。
				LineSegment(ctx, { 0.0f, 0.0f }, { 30.0f, 40.0f }, color, 6.0f);
				CHECK(ctx.Commands().size() == 1);
				const WuiDrawCommand& oblique = ctx.Commands()[0];
				CHECK(oblique.Kind == WuiDrawKind::Quad);
				CHECK(Near(oblique.Color.R, color.R) && Near(oblique.Color.G, color.G)
					&& Near(oblique.Color.B, color.B) && Near(oblique.Color.A, color.A));
				// 顶点顺序 = {from+n, to+n, to-n, from-n}(与面板手写一致)。
				CHECK(Near(oblique.Vertices[0].x, -2.4f) && Near(oblique.Vertices[0].y, 1.8f));
				CHECK(Near(oblique.Vertices[1].x, 27.6f) && Near(oblique.Vertices[1].y, 41.8f));
				CHECK(Near(oblique.Vertices[2].x, 32.4f) && Near(oblique.Vertices[2].y, 38.2f));
				CHECK(Near(oblique.Vertices[3].x, 2.4f) && Near(oblique.Vertices[3].y, -1.8f));
				// 其余字段 = WuiDrawCommand 默认(与面板"默认构造 + 只设三项"逐字段等价)。
				CHECK(oblique.Text.empty() && oblique.Image == 0);
				CHECK(Near(oblique.Rounding, 0.0f) && Near(oblique.Thickness, 1.0f) && !oblique.Bold);
				CHECK(Near(oblique.Rect.X, 0.0f) && Near(oblique.Rect.W, 0.0f)
					&& Near(oblique.Uv.W, 1.0f));
				// 水平线:法线 = (0,1) ⇒ 只在 y 上 ±thickness/2(不产生 x 位移)。
				LineSegment(ctx, { 10.0f, 50.0f }, { 110.0f, 50.0f }, color, 4.0f);
				const WuiDrawCommand& flat = ctx.Commands()[1];
				CHECK(Near(flat.Vertices[0].x, 10.0f) && Near(flat.Vertices[0].y, 52.0f));
				CHECK(Near(flat.Vertices[1].x, 110.0f) && Near(flat.Vertices[1].y, 52.0f));
				CHECK(Near(flat.Vertices[2].x, 110.0f) && Near(flat.Vertices[2].y, 48.0f));
				CHECK(Near(flat.Vertices[3].x, 10.0f) && Near(flat.Vertices[3].y, 48.0f));
				// 退化阈值 = 0.5px:|d| < 0.5 不产出命令,|d| == 0.5 照常产出(与面板同一判据)。
				const size_t before = ctx.Commands().size();
				LineSegment(ctx, { 20.0f, 20.0f }, { 20.3f, 20.2f }, color, 4.0f);   // |d|≈0.361
				LineSegment(ctx, { 20.0f, 20.0f }, { 20.0f, 20.0f }, color, 4.0f);
				CHECK(ctx.Commands().size() == before);
				LineSegment(ctx, { 20.0f, 20.0f }, { 20.5f, 20.0f }, color, 4.0f);
				CHECK(ctx.Commands().size() == before + 1);
				ctx.EndFrame();
			}

			// ---- ② ButtonEx 禁用态:Tab 焦点步骤 0 → A → B → 回卷(Space 也不能激活它)----
			{
				const WuiId idA = HashId("test.lib3.a");
				const WuiId idDisabled = HashId("test.lib3.disabled");
				const WuiId idB = HashId("test.lib3.b");
				const WuiRect rectA { 20.0f, 20.0f, 120.0f, 24.0f };
				const WuiRect rectDisabled { 20.0f, 48.0f, 120.0f, 24.0f };
				const WuiRect rectB { 20.0f, 76.0f, 120.0f, 24.0f };
				const std::string reason = "Select a material instance first";
				WuiContext ctx;
				bool disabledActivated = false;
				const auto frame = [&](WuiContext& target, const WuiInputState& input)
				{
					accessibility.BeginFrame("main", input.ViewportSize);
					accessibility.SetPanel("showcase");
					target.BeginFrame(input);
					Button(target, idA, rectA, "A", theme);
					disabledActivated = ButtonEx(target, idDisabled, rectDisabled, "Assign", theme,
						false, false, reason)
						|| disabledActivated;
					Button(target, idB, rectB, "B", theme);
					target.EndFrame();
				};
				WuiInputState idle = base;
				frame(ctx, idle);
				// a11y 仍可读:禁用 + 理由都在节点上(Interactive 保持 true = "可读",但点了不动作)。
				const WuiAccessNode* node = accessibility.Find(idDisabled);
				CHECK(node != nullptr);
				if (node != nullptr)
				{
					CHECK(node->Kind == "button" && node->Label == "Assign");
					CHECK(!node->Enabled && node->Interactive);
					CHECK(node->Value == reason && node->Tooltip == reason);
					CHECK(!node->Focused);
				}
				// Tab 焦点步骤:0 → A → B(跳过禁用件)→ 回卷 A → B。禁用件 1 次都不出现。
				std::vector<WuiId> focusSteps;
				for (int step = 0; step < 4; ++step)
				{
					WuiInputState tab = base;
					tab.KeyPressed = { static_cast<uint32_t>(World::KeyCodes::Tab) };
					frame(ctx, tab);
					focusSteps.push_back(ctx.Focus());
				}
				CHECK(focusSteps[0] == idA);
				CHECK(focusSteps[1] == idB);
				CHECK(focusSteps[2] == idA);
				CHECK(focusSteps[3] == idB);
				for (WuiId focused : focusSteps)
					CHECK(focused != idDisabled);
				// "改前"对照(同一二进制内可复现):手工把禁用件登记进焦点表 —— 那正是改动前
				// ButtonEx 无条件 RegisterFocusable 的那一行;同一 Tab 序列立刻变成 A → 禁用件 ⇒
				// "禁用不进 Tab"确实来自这一次登记,而不是别处(比如 a11y 或绘制)顺带造成的。
				{
					WuiContext beforeCtx;
					const auto beforeFrame = [&](WuiContext& target, const WuiInputState& input)
					{
						accessibility.BeginFrame("main", input.ViewportSize);
						accessibility.SetPanel("showcase");
						target.BeginFrame(input);
						Button(target, idA, rectA, "A", theme);
						ButtonEx(target, idDisabled, rectDisabled, "Assign", theme, false, false, reason);
						target.RegisterFocusable(idDisabled, rectDisabled);   // 改前口径(现由 if (enabled) 拦掉)
						Button(target, idB, rectB, "B", theme);
						target.EndFrame();
					};
					beforeFrame(beforeCtx, idle);
					WuiInputState tab = base;
					tab.KeyPressed = { static_cast<uint32_t>(World::KeyCodes::Tab) };
					beforeFrame(beforeCtx, tab);
					CHECK(beforeCtx.Focus() == idA);
					beforeFrame(beforeCtx, tab);
					CHECK(beforeCtx.Focus() == idDisabled);   // 改前:Tab 第 2 步就停在禁用件上
				}
				// 焦点正好落在禁用件上时按 Space:不激活(焦点链变化之外的"不可点"仍成立)。
				{
					WuiContext forced;
					WuiInputState space = base;
					space.KeyPressed = { static_cast<uint32_t>(World::KeyCodes::Space) };
					accessibility.BeginFrame("main", space.ViewportSize);
					accessibility.SetPanel("showcase");
					forced.BeginFrame(space);
					forced.SetFocus(idDisabled);
					CHECK(!ButtonEx(forced, idDisabled, rectDisabled, "Assign", theme, false, false, reason));
					forced.EndFrame();
					CHECK(!disabledActivated);
				}
				// 下一帧仍禁用 ⇒ "消失即失焦"把它从焦点位清掉(不留幽灵焦点)。
				{
					WuiContext cleared;
					WuiInputState first = base;
					accessibility.BeginFrame("main", first.ViewportSize);
					accessibility.SetPanel("showcase");
					cleared.BeginFrame(first);
					ButtonEx(cleared, idDisabled, rectDisabled, "Assign", theme, true, false, reason);
					cleared.SetFocus(idDisabled);
					cleared.EndFrame();
					CHECK(cleared.Focus() == idDisabled);
					accessibility.BeginFrame("main", first.ViewportSize);
					accessibility.SetPanel("showcase");
					cleared.BeginFrame(first);
					ButtonEx(cleared, idDisabled, rectDisabled, "Assign", theme, false, false, reason);
					cleared.EndFrame();
					CHECK(cleared.Focus() == 0);
				}
				// 启用态仍进焦点链(本改动只作用于 enabled=false):第一帧登记,第二帧 Tab 到达。
				{
					WuiContext enabledCtx;
					accessibility.BeginFrame("main", idle.ViewportSize);
					accessibility.SetPanel("showcase");
					enabledCtx.BeginFrame(idle);
					ButtonEx(enabledCtx, idDisabled, rectDisabled, "Assign", theme, true, false, reason);
					enabledCtx.EndFrame();
					WuiInputState tab = base;
					tab.KeyPressed = { static_cast<uint32_t>(World::KeyCodes::Tab) };
					accessibility.BeginFrame("main", tab.ViewportSize);
					accessibility.SetPanel("showcase");
					enabledCtx.BeginFrame(tab);
					ButtonEx(enabledCtx, idDisabled, rectDisabled, "Assign", theme, true, false, reason);
					enabledCtx.EndFrame();
					CHECK(enabledCtx.Focus() == idDisabled);
				}
			}

			// ---- ③ 登记表:line-segment 已登记(showcase 真实路径由 §20 的通用循环跑)----
			{
				const WuiComponentDesc* line = WuiComponentRegistry::Find("line-segment");
				CHECK(line != nullptr);
				if (line != nullptr)
				{
					CHECK(line->TypeName == "LineSegment");
					CHECK(line->Category == "Chrome");
					CHECK(!line->Properties.empty());
					bool hasDefault = false;
					for (const WuiComponentState& state : line->States)
						if (state.Id == "default")
							hasDefault = true;
					CHECK(hasDefault);
				}
			}

			accessibility.SetEnabled(false);
			accessibility.Clear();
		}

		// 27. WUI-P1.5a:属性 schema 扩展 + 交互契约 + Button style 化 —— 五段硬口径:
		//     ① 值编码协议:颜色 #RRGGBB[AA] / 尺寸 WxH 可解析、规范写法可回写、坏文本不抛不改值;
		//     ② 登记表:button 补齐 Content/Style/Layout/Behavior 四组属性 + 交互契约(hover/click/key),
		//        静态件保持空契约;
		//     ③ style 覆盖**真实生效**:5 态颜色只作用于自己那一态,字号/粗细/内边距改的是画布命令,
		//        未覆盖 = 旧硬编码/主题令牌(命令流逐字节相同 = 旧基线不动);
		//     ④ Layout:preferred("WxH")改画布槽位(宽/高/竖直居中),坏值忽略;
		//     ⑤ 微基准:1000 帧 button 命令流的 CPU 时间(数字进报告,供 P1.5B 对比)。
		{
			// ---- ① 值编码协议 ----
			{
				WuiColor color {};
				CHECK(ParseComponentColor("#4C8DFF", color));
				CHECK(Near(color.R, 0x4C / 255.0f) && Near(color.G, 0x8D / 255.0f)
					&& Near(color.B, 0xFF / 255.0f));
				CHECK(Near(color.A, 1.0f));                       // 6 位 = 不透明
				CHECK(ParseComponentColor("2b313899", color));     // 可省 '#'、大小写不限
				CHECK(Near(color.A, 0x99 / 255.0f));
				CHECK(!ParseComponentColor("#12345", color));      // 5 位 = 非法
				CHECK(!ParseComponentColor("rgba(1,2,3)", color)); // 认不出的写法 = 没给
				CHECK(Near(color.A, 0x99 / 255.0f));               // 失败不改 out(调用方保留当前值)
				CHECK(FormatComponentColor(WuiColor { 0x22 / 255.0f, 0x27 / 255.0f, 0x2F / 255.0f, 1.0f })
					== "#22272F");
				CHECK(FormatComponentColor(WuiColor { 0.0f, 0.0f, 0.0f, 0.6f }) == "#00000099");

				float width = 0.0f;
				float height = 0.0f;
				CHECK(ParseComponentSize("200x32", width, height) && Near(width, 200.0f)
					&& Near(height, 32.0f));
				CHECK(ParseComponentSize(" 128 X 24 ", width, height) && Near(width, 128.0f)
					&& Near(height, 24.0f));
				CHECK(ParseComponentSize("96*28", width, height) && Near(height, 28.0f));
				CHECK(!ParseComponentSize("128", width, height));        // 缺分隔符
				CHECK(!ParseComponentSize("12px", width, height));       // 残留字符
				CHECK(!ParseComponentSize("128x24x2", width, height));   // 多一段
				CHECK(Near(width, 96.0f));                               // 失败不改 out
				CHECK(FormatComponentSize(128.0f, 24.5f) == "128x24.5");
			}

			// ---- ② 登记表:四组属性 + 交互契约 ----
			const WuiComponentDesc* button = WuiComponentRegistry::Find("button");
			CHECK(button != nullptr);
			if (button != nullptr)
			{
				size_t content = 0;
				size_t style = 0;
				size_t layout = 0;
				size_t behavior = 0;
				size_t stateScopedColors = 0;
				bool labelIsText = false;
				bool fontSizeOk = false;
				bool preferredOk = false;
				bool hoverColorOk = false;
				for (const WuiComponentProperty& property : button->Properties)
				{
					switch (property.Group)
					{
					case WuiComponentPropertyGroup::Content:
						++content;
						labelIsText = labelIsText
							|| (property.Name == "label" && property.Type == WuiComponentProperty::Kind::Text
								&& !property.DefaultText.empty());
						break;
					case WuiComponentPropertyGroup::Style:
						++style;
						if (property.Type == WuiComponentProperty::Kind::Color && property.StateScoped)
							++stateScopedColors;
						fontSizeOk = fontSizeOk
							|| (property.Name == "fontSize" && property.Type == WuiComponentProperty::Kind::Float
								&& Near(property.DefaultNumber, 15.0f) && property.Unit == "px"
								&& !property.Doc.empty());
						hoverColorOk = hoverColorOk
							|| (property.Name == "bg.hover" && property.Type == WuiComponentProperty::Kind::Color
								&& property.StateScoped && !property.DefaultText.empty());
						break;
					case WuiComponentPropertyGroup::Layout:
						++layout;
						preferredOk = preferredOk
							|| (property.Name == "preferred" && property.Type == WuiComponentProperty::Kind::Size2
								&& Near(property.DefaultSize.x, 128.0f) && Near(property.DefaultSize.y, 24.0f)
								&& property.DefaultText == "128x24" && !property.Doc.empty());
						break;
					case WuiComponentPropertyGroup::Behavior:
						++behavior;
						break;
					}
				}
				CHECK(content == 1 && labelIsText);          // Content = 文本(label)
				CHECK(style == 18);                          // Style = 字号/粗细/内边距 + 5 态 × 3 通道颜色
				CHECK(stateScopedColors == 15);              // 15 条颜色都按状态分槽
				CHECK(layout == 1 && preferredOk);           // Layout = preferred size
				CHECK(behavior == 1);                        // Behavior = disabled
				CHECK(fontSizeOk && hoverColorOk);

				// 交互契约:hover / click / key,目标都是本件稳定 a11y id;步骤与备注不能空。
				size_t hover = 0;
				size_t click = 0;
				size_t key = 0;
				for (const WuiComponentInteraction& interaction : button->Interactions)
				{
					CHECK(interaction.TargetId == "showcase.button");
					CHECK(!interaction.Steps.empty());
					CHECK(!interaction.Note.empty());
					switch (interaction.Kind)
					{
					case WuiInteractionKind::Hover:
						++hover;
						CHECK(interaction.Expect == WuiInteractionExpect::PixelChange);
						break;
					case WuiInteractionKind::Click:
						++click;
						CHECK(interaction.Expect == WuiInteractionExpect::Event);
						break;
					case WuiInteractionKind::Key:
						++key;
						CHECK(interaction.Expect == WuiInteractionExpect::Event);
						break;
					default:
						break;
					}
				}
				CHECK(hover == 1 && click == 1 && key == 1);
			}
			// 静态件保持空契约(图片没有可交互语义;P1.6 口径:声明空表 = 工作台标"静态件")。
			const WuiComponentDesc* image = WuiComponentRegistry::Find("image");
			CHECK(image != nullptr && image->Interactions.empty());

			// ---- ③④ style / 尺寸覆盖:同一件、同一条真实控件路径 ----
			WuiAccessibility::Get().SetEnabled(false);
			WuiAccessibility::Get().Clear();
			const auto DrawButton = [](const std::vector<std::pair<std::string, std::string>>& properties,
				const std::string& state, std::vector<WuiDrawCommand>& out, std::vector<WuiDrawCommand>& overlay)
			{
				WuiContext ctx;
				WuiInputState input;
				input.ViewportSize = { 640.0f, 480.0f };
				ctx.BeginFrame(input);
				WuiComponentDraw draw;
				draw.Context = &ctx;
				draw.Theme = &CurrentTheme();
				draw.Rect = { 12.0f, 12.0f, 420.0f, 160.0f };
				draw.State = state;
				draw.Properties = properties;
				draw.UiScale = 1.0f;
				draw.Density = 1.0f;
				draw.Locale = "en";
				const WuiComponentDesc* component = WuiComponentRegistry::Find("button");
				CHECK(component != nullptr && component->Showcase != nullptr);
				if (component != nullptr && component->Showcase != nullptr)
					component->Showcase(draw);
				ctx.EndFrame();
				out = ctx.Commands();
				overlay = ctx.OverlayCommands();
			};
			const WuiTheme& theme = CurrentTheme();
			std::vector<WuiDrawCommand> plainDefault;
			std::vector<WuiDrawCommand> plainDefaultOverlay;
			DrawButton({}, "default", plainDefault, plainDefaultOverlay);
			CHECK(plainDefault.size() == 3);                  // 填充 + 描边 + 文本(焦点环走 overlay)
			CHECK(Near(plainDefault[0].Color.R, theme.ButtonBg.R)
				&& Near(plainDefault[0].Color.G, theme.ButtonBg.G)
				&& Near(plainDefault[0].Color.B, theme.ButtonBg.B));
			CHECK(Near(plainDefault[1].Color.R, theme.Border.R));
			CHECK(Near(plainDefault[2].Color.R, theme.Text.R));
			CHECK(Near(plainDefault[2].Rect.X, 20.0f));       // 12(画布左缘)+ 8(旧口径内边距)
			CHECK(Near(plainDefault[2].FontSize, 15.0f));     // 旧口径字号
			CHECK(!plainDefault[2].Bold);
			CHECK(Near(plainDefault[0].Rect.W, 128.0f) && Near(plainDefault[0].Rect.H, 24.0f));

			std::vector<WuiDrawCommand> plainHover;
			std::vector<WuiDrawCommand> plainHoverOverlay;
			DrawButton({}, "hover", plainHover, plainHoverOverlay);
			CHECK(Near(plainHover[0].Color.R, theme.ButtonHover.R));

			// ③ 5 态颜色:只影响自己那一态;未覆盖的通道原样回退主题令牌。
			std::vector<WuiDrawCommand> hoverOverride;
			std::vector<WuiDrawCommand> hoverOverrideOverlay;
			DrawButton({ { "bg.hover", "#FF0000" } }, "hover", hoverOverride, hoverOverrideOverlay);
			CHECK(Near(hoverOverride[0].Color.R, 1.0f) && Near(hoverOverride[0].Color.G, 0.0f)
				&& Near(hoverOverride[0].Color.B, 0.0f));
			CHECK(Near(hoverOverride[1].Color.R, theme.Border.R));   // 描边未覆盖 → 主题
			CHECK(Near(hoverOverride[2].Color.R, theme.Text.R));     // 文字未覆盖 → 主题
			CHECK(CommandStreamHash(hoverOverride) != CommandStreamHash(plainHover));
			CHECK(CommandStreamHash(hoverOverrideOverlay) == CommandStreamHash(plainHoverOverlay));

			// 状态作用域:改 bg.default 不该动 hover 态(改前/改后逐字段相同)。
			std::vector<WuiDrawCommand> defaultOnly;
			std::vector<WuiDrawCommand> defaultOnlyOverlay;
			DrawButton({ { "bg.default", "#00FF00" } }, "hover", defaultOnly, defaultOnlyOverlay);
			CHECK(CommandStreamHash(defaultOnly) == CommandStreamHash(plainHover));

			std::vector<WuiDrawCommand> pressedText;
			std::vector<WuiDrawCommand> pressedTextOverlay;
			DrawButton({ { "text.pressed", "#00FF00" } }, "pressed", pressedText, pressedTextOverlay);
			CHECK(Near(pressedText[2].Color.R, 0.0f) && Near(pressedText[2].Color.G, 1.0f));
			CHECK(CommandStreamHash(pressedText) != CommandStreamHash(plainDefault));

			// 排版三件套:字号 / 内边距 / 粗体都进真实绘制参数。
			std::vector<WuiDrawCommand> typography;
			std::vector<WuiDrawCommand> typographyOverlay;
			DrawButton({ { "fontSize", "24" }, { "padding", "20" }, { "bold", "1" } }, "default",
				typography, typographyOverlay);
			CHECK(Near(typography[2].FontSize, 24.0f));
			CHECK(Near(typography[2].Rect.X, 32.0f));
			CHECK(typography[2].Bold);
			CHECK(CommandStreamHash(typography) != CommandStreamHash(plainDefault));

			// 坏值 / 未知键:一律忽略(退回当前口径),不抛、不崩、不改命令流。
			std::vector<WuiDrawCommand> brokenValues;
			std::vector<WuiDrawCommand> brokenValuesOverlay;
			DrawButton({ { "bg.hover", "not-a-color" }, { "padding", "NaN" }, { "fontSize", "-4" },
				{ "preferred", "wide" }, { "bogus", "x" } }, "hover", brokenValues, brokenValuesOverlay);
			CHECK(CommandStreamHash(brokenValues) == CommandStreamHash(plainHover));

			// 焦点:border.focus 同时改按钮描边与焦点环(环在 overlay 层);未覆盖 = 主题 FocusRing。
			// MAT-UI3a:焦点环是两笔(主环 + 外发光,见 §33),这里只断言"颜色跟着基色走"。
			std::vector<WuiDrawCommand> focusPlain;
			std::vector<WuiDrawCommand> focusPlainOverlay;
			DrawButton({}, "focus", focusPlain, focusPlainOverlay);
			CHECK(focusPlainOverlay.size() == 2);
			CHECK(Near(focusPlainOverlay[0].Color.R, theme.FocusRing.R));
			std::vector<WuiDrawCommand> focusOverride;
			std::vector<WuiDrawCommand> focusOverrideOverlay;
			DrawButton({ { "border.focus", "#FF00FF" } }, "focus", focusOverride, focusOverrideOverlay);
			CHECK(Near(focusOverride[1].Color.R, 1.0f) && Near(focusOverride[1].Color.G, 0.0f)
				&& Near(focusOverride[1].Color.B, 1.0f));
			CHECK(focusOverrideOverlay.size() == 2);
			CHECK(Near(focusOverrideOverlay[0].Color.R, 1.0f)
				&& Near(focusOverrideOverlay[0].Color.G, 0.0f)
				&& Near(focusOverrideOverlay[0].Color.B, 1.0f));

			// 禁用态:仍走主题禁用令牌(未覆盖);bg.disabled 覆盖后只改填充。
			std::vector<WuiDrawCommand> disabledPlain;
			std::vector<WuiDrawCommand> disabledPlainOverlay;
			DrawButton({}, "disabled", disabledPlain, disabledPlainOverlay);
			CHECK(Near(disabledPlain[0].Color.R, theme.ContentBg.R));
			CHECK(Near(disabledPlain[2].Color.R, theme.TextDisabled.R));
			std::vector<WuiDrawCommand> disabledOverride;
			std::vector<WuiDrawCommand> disabledOverrideOverlay;
			DrawButton({ { "bg.disabled", "#010203" } }, "disabled", disabledOverride, disabledOverrideOverlay);
			CHECK(Near(disabledOverride[0].Color.R, 1.0f / 255.0f));
			CHECK(Near(disabledOverride[2].Color.R, theme.TextDisabled.R));

			// ④ Layout:preferred 决定画布槽位(宽/高都换,且竖直居中不出画布)。
			std::vector<WuiDrawCommand> sized;
			std::vector<WuiDrawCommand> sizedOverlay;
			DrawButton({ { "preferred", "200x32" } }, "default", sized, sizedOverlay);
			CHECK(Near(sized[0].Rect.W, 200.0f) && Near(sized[0].Rect.H, 32.0f));
			CHECK(Near(sized[0].Rect.Y, 12.0f + (160.0f - 32.0f) * 0.5f));
			CHECK(CommandStreamHash(sized) != CommandStreamHash(plainDefault));
			std::vector<WuiDrawCommand> oversized;
			std::vector<WuiDrawCommand> oversizedOverlay;
			DrawButton({ { "preferred", "9999x9999" } }, "default", oversized, oversizedOverlay);
			CHECK(Near(oversized[0].Rect.W, 420.0f) && Near(oversized[0].Rect.H, 160.0f));   // 夹在画布内

			// ---- ⑤ 微基准:1000 帧 button 命令流(数字只记录,不做性能门禁)----
			{
				WuiContext ctx;
				WuiInputState input;
				input.ViewportSize = { 640.0f, 480.0f };
				WuiComponentDraw draw;
				draw.Context = &ctx;
				draw.Theme = &CurrentTheme();
				draw.Rect = { 12.0f, 12.0f, 420.0f, 160.0f };
				draw.State = "default";
				draw.Properties = { { "label", "Apply" }, { "bg.hover", "#2A3038" } };
				draw.UiScale = 1.0f;
				draw.Density = 1.0f;
				draw.Locale = "en";
				const WuiComponentDesc* component = WuiComponentRegistry::Find("button");
				CHECK(component != nullptr && component->Showcase != nullptr);
				size_t commands = 0;
				const auto started = std::chrono::steady_clock::now();
				for (int frame = 0; frame < 1000; ++frame)
				{
					ctx.BeginFrame(input);
					if (component != nullptr && component->Showcase != nullptr)
						component->Showcase(draw);
					commands += ctx.Commands().size();
					ctx.EndFrame();
				}
				const double elapsedMs = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - started).count();
				std::printf("[bench] button command stream: 1000 frames in %.3f ms (%.4f ms/frame, %zu commands total)\n",
					elapsedMs, elapsedMs / 1000.0, commands);
				CHECK(commands == 3000);          // 3 条/帧(填充 + 描边 + 文本)
				CHECK(elapsedMs > 0.0);
				CHECK(elapsedMs < 20000.0);       // 只挡"荒谬量级"退化;真实数字进报告
			}
		}

		// 28. WUI-P1.5a2:保留模式 Wui::WuiButton(WuiWidget.h/.cpp)与立即模式 Wui::Button(WuiWidgets.cpp)
		//     读**同一份** WuiButtonStyle + 同一份解析 ResolveButtonStyle —— 四段硬口径:
		//     ① 解析器:状态优先级 disabled > pressed > hover > focus > normal,单槽覆盖只动那一槽,
		//        未设置哨兵(字号<=0 / 内边距<0 / 空 optional)= 8px·15px·非常规粗体·调用方兜底色;
		//     ② 未覆盖:两面的命令流差异**逐字段**钉成清单(不是"大概一样";任何意外差异即失败);
		//     ③ 覆盖:同一份覆盖值 → 两面样式字段全同,残差只有圆角(保留模式 = 主题令牌 theme.Radius,
		//        立即模式 = 历史字面量 3.0)—— 记录在案,不为了对齐改掉任何一面的可视结果;
		//     ④ 保留模式旧口径不漂:默认 Style = 主题令牌 + 仅交互态描边(无 Style 的按钮命令流不变)。
		{
			WuiAccessibility::Get().SetEnabled(false);
			WuiAccessibility::Get().Clear();
			const WuiTheme& theme = CurrentTheme();
			const WuiRect rect { 40.0f, 30.0f, 128.0f, 24.0f };
			const WuiId id = HashId("tests.two-faces.button");
			const std::string label = "Apply";

			// ---- ① 解析器(两面唯一的实现)----
			const WuiColor fallbackBg { 0.11f, 0.12f, 0.13f, 1.0f };
			const WuiColor fallbackBorder { 0.21f, 0.22f, 0.23f, 1.0f };
			const WuiColor fallbackText { 0.31f, 0.32f, 0.33f, 1.0f };
			const WuiColor fallbackRing { 0.41f, 0.42f, 0.43f, 1.0f };
			const auto Resolve = [&](const WuiButtonStyle* style, bool disabled, bool pressed, bool hovered, bool focused)
			{
				return ResolveButtonStyle(style, disabled, pressed, hovered, focused,
					fallbackBg, fallbackBorder, fallbackText, fallbackRing);
			};
			const WuiButtonResolvedStyle raw = Resolve(nullptr, false, false, false, false);
			CHECK(raw.State == WuiButtonStyle::State::Normal);
			CHECK(!raw.BgCovered && !raw.BorderCovered && !raw.TextCovered && !raw.FocusRingCovered);
			CHECK(SameColor(raw.Bg, fallbackBg) && SameColor(raw.Border, fallbackBorder)
				&& SameColor(raw.Text, fallbackText) && SameColor(raw.FocusRing, fallbackRing));
			CHECK(Near(raw.PaddingX, 8.0f) && Near(raw.FontSize, 15.0f) && !raw.Bold);
			CHECK(Resolve(nullptr, true, true, true, true).State == WuiButtonStyle::State::Disabled);
			CHECK(Resolve(nullptr, false, true, true, true).State == WuiButtonStyle::State::Pressed);
			CHECK(Resolve(nullptr, false, false, true, true).State == WuiButtonStyle::State::Hover);
			CHECK(Resolve(nullptr, false, false, false, true).State == WuiButtonStyle::State::Focused);
			WuiButtonStyle single;
			single.Colors[static_cast<size_t>(WuiButtonStyle::State::Hover)].Bg = WuiColor { 1.0f, 0.0f, 0.0f, 1.0f };
			const WuiButtonResolvedStyle singleResolved = Resolve(&single, false, false, true, false);
			CHECK(singleResolved.BgCovered && !singleResolved.BorderCovered && !singleResolved.TextCovered);
			CHECK(Near(singleResolved.Bg.R, 1.0f) && SameColor(singleResolved.Border, fallbackBorder)
				&& SameColor(singleResolved.Text, fallbackText));
			WuiButtonStyle sentinel;
			sentinel.FontSize = 0.0f;      // 哨兵:<=0 = 没给
			sentinel.PaddingX = -1.0f;     // 哨兵:<0 = 没给
			const WuiButtonResolvedStyle sentinelResolved = Resolve(&sentinel, false, false, false, false);
			CHECK(Near(sentinelResolved.PaddingX, 8.0f) && Near(sentinelResolved.FontSize, 15.0f)
				&& !sentinelResolved.Bold && !sentinelResolved.FocusRingCovered
				&& SameColor(sentinelResolved.FocusRing, fallbackRing));

			// ---- 两面的真实绘制路径 ----
			const auto FillInput = [&rect](const std::string& state, WuiInputState& input)
			{
				if (state == "hover" || state == "pressed" || state == "hover-focus")
					input.MousePos = { rect.X + rect.W * 0.5f, rect.Y + rect.H * 0.5f };
				if (state == "pressed")
					input.MouseDown[0] = true;
			};
			const auto PaintRetained = [&](const std::string& state, bool enabled, const WuiButtonStyle& style,
				std::vector<WuiDrawCommand>& out, std::vector<WuiDrawCommand>& overlay)
			{
				WuiContext ctx;
				WuiInputState input;
				input.ViewportSize = { 640.0f, 480.0f };
				FillInput(state, input);
				ctx.BeginFrame(input);
				if (state == "focus" || state == "hover-focus")
					ctx.SetFocus(id);
				const std::shared_ptr<WuiButton> button = std::make_shared<WuiButton>();
				button->SetId(id);
				button->Label = label;
				button->Enabled = enabled;
				button->Style = style;
				button->Arrange(rect);
				WuiPaintContext paint(ctx);
				button->Paint(paint);
				ctx.EndFrame();
				out = ctx.Commands();
				overlay = ctx.OverlayCommands();
			};
			const auto PaintImmediate = [&](const std::string& state, const WuiButtonStyle* style,
				std::vector<WuiDrawCommand>& out, std::vector<WuiDrawCommand>& overlay)
			{
				WuiContext ctx;
				WuiInputState input;
				input.ViewportSize = { 640.0f, 480.0f };
				FillInput(state, input);
				ctx.BeginFrame(input);
				if (state == "focus" || state == "hover-focus")
					ctx.SetFocus(id);
				CHECK(!Button(ctx, id, rect, label, theme, style));   // 注入的输入不含点击/按键沿 ⇒ 不激活
				ctx.EndFrame();
				out = ctx.Commands();
				overlay = ctx.OverlayCommands();
			};
			// 同一状态、同一矩形、同一份 style:两面各画一次,产出逐字段投影 + 差异清单。
			const auto PaintPair = [&](const std::string& state, bool retainedEnabled, const WuiButtonStyle* style,
				std::vector<std::string>& deltas, ButtonPaintFields& retainedFields, ButtonPaintFields& immediateFields)
			{
				std::vector<WuiDrawCommand> retained;
				std::vector<WuiDrawCommand> retainedOverlay;
				PaintRetained(state, retainedEnabled, style != nullptr ? *style : WuiButtonStyle {}, retained, retainedOverlay);
				std::vector<WuiDrawCommand> immediate;
				std::vector<WuiDrawCommand> immediateOverlay;
				PaintImmediate(state, style, immediate, immediateOverlay);
				retainedFields = ProjectButtonPaint(retained, retainedOverlay);
				immediateFields = ProjectButtonPaint(immediate, immediateOverlay);
				deltas = DiffButtonPaint(retainedFields, immediateFields);
			};

			ButtonPaintFields retainedFields;
			ButtonPaintFields immediateFields;
			std::vector<std::string> deltas;

			// ---- ② 未覆盖(style = nullptr / 默认 Style):差异清单 = 这张表 ----
			PaintPair("normal", true, nullptr, deltas, retainedFields, immediateFields);
			ExpectDeltas(deltas, { "fill.rounding", "border.drawn" }, __LINE__);
			// 未覆盖 = 同一组旧口径默认值:8px 内边距 / 15px 字号 / 非常规粗体,两面都成立。
			CHECK(Near(retainedFields.TextX, rect.X + 8.0f) && Near(immediateFields.TextX, rect.X + 8.0f));
			CHECK(Near(retainedFields.FontSize, 15.0f) && Near(immediateFields.FontSize, 15.0f));
			CHECK(!retainedFields.Bold && !immediateFields.Bold);
			CHECK(Near(retainedFields.TextY, immediateFields.TextY));
			// 保留模式默认态:干净(无描边)、填充 ButtonBg、文字 Text —— P4-UX13 的既定外观。
			CHECK(!retainedFields.BorderDrawn);
			CHECK(SameColor(retainedFields.Fill, theme.ButtonBg) && SameColor(retainedFields.Text, theme.Text));
			// 立即模式默认态:常驻底色描边 theme.Border。
			CHECK(immediateFields.BorderDrawn && SameColor(immediateFields.Border, theme.Border));
			CHECK(Near(retainedFields.FillRounding, theme.Radius) && Near(immediateFields.FillRounding, 3.0f));

			PaintPair("hover", true, nullptr, deltas, retainedFields, immediateFields);
			ExpectDeltas(deltas, { "fill.rounding", "border.color", "border.rounding" }, __LINE__);
			CHECK(SameColor(retainedFields.Fill, theme.ButtonHover) && SameColor(immediateFields.Fill, theme.ButtonHover));
			CHECK(SameColor(retainedFields.Border, theme.BorderStrong) && SameColor(immediateFields.Border, theme.Border));

			PaintPair("pressed", true, nullptr, deltas, retainedFields, immediateFields);
			ExpectDeltas(deltas, { "fill.rounding", "border.color", "border.rounding", "text.color" }, __LINE__);
			// 按下填充:两面用**不同令牌**(保留模式 ActiveBg、立即模式 ButtonHover);当前主题两者取值相同
			// (所以不构成差异),主题一旦把它们分开就会显形 —— 这里显式钉住"各自用哪个令牌"。
			CHECK(SameColor(retainedFields.Fill, theme.ActiveBg) && SameColor(immediateFields.Fill, theme.ButtonHover));
			CHECK(SameColor(retainedFields.Text, theme.Text) && SameColor(immediateFields.Text, theme.Accent));

			PaintPair("focus", true, nullptr, deltas, retainedFields, immediateFields);
			// 焦点环两面共用 DrawFocusRing(圆角 = 主题令牌,颜色 = 主题 FocusRing × 主环 alpha)
			// → 环本身没有残差;差异只在填充/描边。
			ExpectDeltas(deltas, { "fill.rounding", "border.color", "border.rounding" }, __LINE__);
			CHECK(retainedFields.RingDrawn && immediateFields.RingDrawn);
			const WuiColor expectedRing = DimmedRing(theme.FocusRing, 0.72f);
			CHECK(SameColor(retainedFields.Ring, expectedRing) && SameColor(immediateFields.Ring, expectedRing));

			// 禁用态未覆盖:保留模式用自身 Enabled(主题禁用令牌 ContentBg/TextDisabled,且旧口径**不描边**);
			// 立即模式要调用方给禁用外观 —— 展示台走 DisabledTheme 令牌替换,直接调用时只有样式里的
			// disabled 开关(未覆盖颜色 = 正常态兜底)。这就是两面"禁用来源"的既有差异。
			WuiButtonStyle disabledFlag;
			disabledFlag.Disabled = true;
			PaintPair("normal", false, &disabledFlag, deltas, retainedFields, immediateFields);
			ExpectDeltas(deltas, { "fill.color", "fill.rounding", "border.drawn", "text.color" }, __LINE__);
			CHECK(SameColor(retainedFields.Fill, theme.ContentBg) && SameColor(retainedFields.Text, theme.TextDisabled));
			CHECK(!retainedFields.BorderDrawn && SameColor(immediateFields.Fill, theme.ButtonBg));

			// ---- ③ 覆盖:同一覆盖值 → 两面样式字段全同(残差 = 圆角令牌 vs 字面量)----
			const auto MakeCoveredStyle = []()
			{
				// 每态每通道取互不相同的值 → "取错状态/取错通道"立刻可辨(不是"两边都错所以相等")。
				WuiButtonStyle style;
				const float steps[WuiButtonStyle::StateCount] = { 0.10f, 0.30f, 0.50f, 0.70f, 0.90f };
				for (size_t state = 0; state < WuiButtonStyle::StateCount; ++state)
				{
					style.Colors[state].Bg = WuiColor { steps[state], 0.05f, 0.05f, 1.0f };
					style.Colors[state].Border = WuiColor { 0.05f, steps[state], 0.05f, 1.0f };
					style.Colors[state].Text = WuiColor { 0.05f, 0.05f, steps[state], 1.0f };
				}
				style.PaddingX = 13.0f;
				style.FontSize = 19.0f;
				style.Bold = true;
				return style;
			};
			const WuiButtonStyle covered = MakeCoveredStyle();
			PaintPair("normal", true, &covered, deltas, retainedFields, immediateFields);
			ExpectDeltas(deltas, { "fill.rounding", "border.rounding" }, __LINE__);
			CHECK(Near(retainedFields.Fill.R, 0.10f) && Near(retainedFields.Border.G, 0.10f)
				&& Near(retainedFields.Text.B, 0.10f));
			CHECK(Near(retainedFields.TextX, rect.X + 13.0f) && Near(retainedFields.FontSize, 19.0f)
				&& retainedFields.Bold);
			// border.default 覆盖让保留模式默认态也描边(旧口径不描边 ⇒ 否则这个属性看不见)。
			CHECK(retainedFields.BorderDrawn && immediateFields.BorderDrawn);
			// 残差的实测来源(不在本单消除:改任何一面都是改可视结果)。
			CHECK(Near(retainedFields.FillRounding, theme.Radius) && Near(immediateFields.FillRounding, 3.0f));

			PaintPair("hover", true, &covered, deltas, retainedFields, immediateFields);
			ExpectDeltas(deltas, { "fill.rounding", "border.rounding" }, __LINE__);
			CHECK(Near(retainedFields.Fill.R, 0.30f) && Near(immediateFields.Fill.R, 0.30f));
			// 同一覆盖值 → 命令流**除圆角外逐字节相同**(判据:不含 Rounding 的 FNV 相等,
			// 且圆角差异恰好只出现在"填充 + 描边"两条上;焦点环共用 DrawFocusRing,圆角本来就一致)。
			{
				std::vector<WuiDrawCommand> retainedStream;
				std::vector<WuiDrawCommand> retainedOverlayStream;
				PaintRetained("hover", true, covered, retainedStream, retainedOverlayStream);
				std::vector<WuiDrawCommand> immediateStream;
				std::vector<WuiDrawCommand> immediateOverlayStream;
				PaintImmediate("hover", &covered, immediateStream, immediateOverlayStream);
				CHECK(retainedStream.size() == 3 && immediateStream.size() == 3);
				CHECK(CommandStreamHashNoRounding(retainedStream) == CommandStreamHashNoRounding(immediateStream));
				CHECK(CountRoundingDiffs(retainedStream, immediateStream) == 2);
				CHECK(CommandStreamHashNoRounding(retainedOverlayStream)
					== CommandStreamHashNoRounding(immediateOverlayStream));
			}

			PaintPair("pressed", true, &covered, deltas, retainedFields, immediateFields);
			ExpectDeltas(deltas, { "fill.rounding", "border.rounding" }, __LINE__);
			CHECK(Near(retainedFields.Fill.R, 0.50f) && Near(retainedFields.Text.B, 0.50f));

			PaintPair("focus", true, &covered, deltas, retainedFields, immediateFields);
			ExpectDeltas(deltas, { "fill.rounding", "border.rounding" }, __LINE__);
			CHECK(Near(retainedFields.Fill.R, 0.90f));
			// MAT-UI3a:border.focus 覆盖是焦点环的**基色**(RGB 不动);"低透明度"只落在 alpha 上
			// (1.0 × 0.72 = 主环 alpha)。
			CHECK(Near(retainedFields.Ring.G, 0.90f));
			CHECK(Near(immediateFields.Ring.G, 0.90f));
			CHECK(Near(retainedFields.Ring.A, 0.72f) && Near(immediateFields.Ring.A, 0.72f));

			// 优先级:hover 赢 focus(两面同一判据)。
			PaintPair("hover-focus", true, &covered, deltas, retainedFields, immediateFields);
			ExpectDeltas(deltas, { "fill.rounding", "border.rounding" }, __LINE__);
			CHECK(Near(retainedFields.Fill.R, 0.30f) && Near(immediateFields.Fill.R, 0.30f));

			// disabled 覆盖一切(悬停 + 禁用 = 禁用槽),且 border.disabled 覆盖后保留模式禁用态也描边。
			WuiButtonStyle coveredDisabled = covered;
			coveredDisabled.Disabled = true;
			PaintPair("hover", false, &coveredDisabled, deltas, retainedFields, immediateFields);
			ExpectDeltas(deltas, { "fill.rounding", "border.rounding" }, __LINE__);
			CHECK(Near(retainedFields.Fill.R, 0.70f) && Near(immediateFields.Fill.R, 0.70f));
			CHECK(retainedFields.BorderDrawn && Near(retainedFields.Border.G, 0.70f));

			// ---- ④ 单槽覆盖(不是"全槽覆盖"的副作用):border.default 必须看得见,其余仍是旧口径 ----
			WuiButtonStyle onlyBorderDefault;
			onlyBorderDefault.Colors[static_cast<size_t>(WuiButtonStyle::State::Normal)].Border =
				WuiColor { 0.2f, 0.4f, 0.6f, 1.0f };
			std::vector<WuiDrawCommand> retainedOnly;
			std::vector<WuiDrawCommand> retainedOnlyOverlay;
			PaintRetained("normal", true, onlyBorderDefault, retainedOnly, retainedOnlyOverlay);
			const ButtonPaintFields onlyFields = ProjectButtonPaint(retainedOnly, retainedOnlyOverlay);
			CHECK(retainedOnly.size() == 3);      // 填充 + 描边 + 文本
			CHECK(onlyFields.BorderDrawn && SameColor(onlyFields.Border, WuiColor { 0.2f, 0.4f, 0.6f, 1.0f }));
			CHECK(SameColor(onlyFields.Fill, theme.ButtonBg) && SameColor(onlyFields.Text, theme.Text));
			CHECK(Near(onlyFields.TextX, rect.X + 8.0f) && Near(onlyFields.FontSize, 15.0f) && !onlyFields.Bold);
			CHECK(!onlyFields.RingDrawn);
		}

		// 29. MAT-UI3a:取色器(色板/色相条/alpha 条)的**按住拖动逐帧跟随**。
		//     用户原话:「选取颜色板左键拖拽不松开,颜色不会变」。复现口径 = 逐帧注入
		//     press → move×N → release(不是"点击两帧"),并且**每一步都读值**:
		//     ① 区内每一步都必须改值;
		//     ② 拖出那块 110px 高的色板/12px 高的条(指针离开命中区)仍必须继续按当前坐标夹取更新
		//        —— 旧实现每帧要求 `IsHovered(子区域)`,这里就是"拖到一半不动了"的真因;
		//     ③ 松手帧落最终值,松手后同一位置不再改值(捕获收口)。
		{
			WuiAccessibility& accessibility = WuiAccessibility::Get();
			accessibility.SetEnabled(true);
			const WuiTheme theme {};
			const WuiId colorId = HashId("test.mat-ui3a.colorfield");
			const WuiRect fieldRect { 100.0f, 100.0f, 200.0f, 22.0f };
			glm::vec4 color { 1.0f, 0.0f, 0.0f, 1.0f };   // 纯红:H=0,S=1,V=1 → 色相可观察
			WuiContext ctx;
			const auto Draw = [&](const WuiInputState& input)
			{
				accessibility.BeginFrame("main", { 1280.0f, 720.0f });
				accessibility.SetPanel("test.mat-ui3a");
				ctx.BeginFrame(input);
				const bool changed = ColorField(ctx, colorId, fieldRect, color, theme);
				ctx.EndFrame();
				return changed;
			};
			const auto ChildRect = [&](const char* suffix, size_t index)
			{
				const std::string key = std::to_string(colorId) + suffix + std::to_string(index);
				const WuiAccessNode* node = accessibility.Find(HashId(key.c_str()));
				return node != nullptr ? node->Rect : WuiRect { 0.0f, 0.0f, 0.0f, 0.0f };
			};
			const auto Frame = [](glm::vec2 pos, bool down, bool clicked, bool released)
			{
				WuiInputState input;
				input.ViewportSize = { 1280.0f, 720.0f };
				input.MousePos = pos;
				input.MouseDown[0] = down;
				input.MouseClicked[0] = clicked;
				input.MouseReleased[0] = released;
				return input;
			};
			// 打开弹层:折叠态字段上的 press 帧(点击即展开,与既有口径一致)。
			Draw(Frame({ fieldRect.X + 20.0f, fieldRect.Y + 11.0f }, true, true, false));
			CHECK(ctx.IsPopupOpen(colorId));
			const WuiRect sv = ChildRect(".sv.", 0);
			const WuiRect hue = ChildRect(".hue.", 0);
			const WuiRect alpha = ChildRect(".alpha.", 0);
			CHECK(sv.W > 20.0f && sv.H > 20.0f);
			CHECK(hue.W > 20.0f && hue.H > 0.0f);
			CHECK(alpha.W > 20.0f && alpha.H > 0.0f);
			// ---- ① 色板:press → move×4(后两步在色板外)→ release ----
			const std::vector<glm::vec2> svPath {
				{ sv.X + sv.W * 0.60f, sv.Y + sv.H * 0.40f },
				{ sv.X + sv.W * 0.90f, sv.Y + sv.H * 0.20f },
				{ sv.X + sv.W * 2.00f, sv.Y + sv.H * 2.00f },   // 出区(右下)→ S=1,V=0(黑)
				{ sv.X - sv.W * 0.50f, sv.Y - sv.H * 0.50f },   // 出区(左上)→ S=0,V=1(白)
			};
			CHECK(Draw(Frame(svPath[0], true, true, false)));
			std::vector<glm::vec4> held;
			held.push_back(color);
			for (size_t step = 1; step < svPath.size(); ++step)
			{
				CHECK(Draw(Frame(svPath[step], true, false, false)));
				held.push_back(color);
			}
			CHECK(held[0] != held[1] && held[1] != held[2] && held[2] != held[3]);
			// 出区那两帧是被夹取后的结果:右下 = 黑(S=1,V=0)、左上 = 白(S=0,V=1)。
			CHECK(Near(held[2].g, 0.0f) && Near(held[2].b, 0.0f));
			CHECK(held[3].r > 0.999f && held[3].g > 0.999f && held[3].b > 0.999f);
			// 松手帧:落最终值(松手位置 = 区内的 S=0.25 / V=0.25,H 仍为 0 → HSV(0,0.25,0.25)
			// = (0.25, 0.1875, 0.1875):c = V·S = 0.0625、m = V − c = 0.1875)。
			const glm::vec2 releasePoint { sv.X + sv.W * 0.25f, sv.Y + sv.H * 0.75f };
			CHECK(Draw(Frame(releasePoint, false, false, true)));
			CHECK(Near(color.r, 0.25f) && Near(color.g, 0.1875f) && Near(color.b, 0.1875f));
			// 松手后同一位置不再改值(捕获收口,不会"幽灵拖动")。
			CHECK(!Draw(Frame(releasePoint, false, false, false)));
			// ---- ② 色相条:先把 S/V 拨到 1/1,再"起手在条内、跟到条外" ----
			const glm::vec2 saturated { sv.X + sv.W - 0.25f, sv.Y + 0.25f };   // S≈1,V≈1(纯色)
			Draw(Frame(saturated, true, true, false));
			Draw(Frame(saturated, false, false, true));
			CHECK(color.r > 0.99f && color.g < 0.01f && color.b < 0.01f);   // 仍是纯红(H=0)
			Draw(Frame({ hue.X + 1.0f, hue.Y + hue.H * 0.5f }, true, true, false));
			const bool hueChanged = Draw(Frame({ hue.X + hue.W * 0.5f, hue.Y + hue.H + 6.0f }, true, false, false));
			CHECK(hueChanged);
			CHECK(color.r < 0.02f && color.g > 0.98f && color.b > 0.98f);   // H=180° 青
			Draw(Frame({ hue.X + hue.W * 0.5f, hue.Y + hue.H + 6.0f }, false, false, true));
			// ---- ③ alpha 条:同样"起手在条内、跟到条外" ----
			Draw(Frame({ alpha.X + alpha.W * 0.5f, alpha.Y + alpha.H * 0.5f }, true, true, false));
			const bool alphaChanged = Draw(Frame({ alpha.X - 30.0f, alpha.Y + alpha.H + 8.0f }, true, false, false));
			CHECK(alphaChanged);
			CHECK(Near(color.a, 0.0f));   // 出区左侧 = 夹到 0
			Draw(Frame({ alpha.X - 30.0f, alpha.Y + alpha.H + 8.0f }, false, false, true));
			CHECK(Near(color.a, 0.0f));
			// 弹层仍然开着(拖动过程中不允许被误关),RGB 已经跟着走。
			CHECK(ctx.IsPopupOpen(colorId));
			CHECK(color.r < 0.02f && color.g > 0.98f && color.b > 0.98f);
			accessibility.SetEnabled(false);
			accessibility.Clear();
		}

		// 30. MAT-UI3a:DragBar 的"值编辑态挡住条体拖动"。
		//     用户原话:「在点击右侧值后左侧滑条没法滑动了」。逐帧口径:点击值区 → 进入文本编辑 →
		//     **不先点别处**直接按条体 → 同一帧先提交编辑、再按像素比例改值,并逐帧跟随到松手。
		{
			WuiAccessibility& accessibility = WuiAccessibility::Get();
			accessibility.SetEnabled(true);
			const WuiTheme theme {};
			const WuiId barId = HashId("test.mat-ui3a.dragbar");
			const WuiRect barRect { 100.0f, 200.0f, 200.0f, 22.0f };
			// 控件内部几何:valueW = max(40, min(style.ValueWidth = 56, 200×0.45 = 90)) = 56,
			// bar = {100,200,138,22},值区 = {244,200,56,22}。
			const WuiRect barZone { 100.0f, 200.0f, 138.0f, 22.0f };
			const WuiRect valueZone { 244.0f, 200.0f, 56.0f, 22.0f };
			float value = 0.45f;
			const WuiNumberStyle style {};
			WuiContext ctx;
			const auto Draw = [&](const WuiInputState& input)
			{
				accessibility.BeginFrame("main", { 1280.0f, 720.0f });
				accessibility.SetPanel("test.mat-ui3a");
				ctx.BeginFrame(input);
				const bool changed = DragBarFloat(ctx, barId, barRect, value, 0.0f, 1.0f, theme, style);
				ctx.EndFrame();
				return changed;
			};
			const auto Frame = [](glm::vec2 pos, bool down, bool clicked, bool released)
			{
				WuiInputState input;
				input.ViewportSize = { 1280.0f, 720.0f };
				input.MousePos = pos;
				input.MouseDown[0] = down;
				input.MouseClicked[0] = clicked;
				input.MouseReleased[0] = released;
				return input;
			};
			const auto Kind = [&]()
			{
				const WuiAccessNode* node = accessibility.Find(barId);
				return node != nullptr ? node->Kind : std::string("<none>");
			};
			const WuiRect valueCenter { valueZone.X + valueZone.W * 0.5f, valueZone.Y + valueZone.H * 0.5f,
				0.0f, 0.0f };
			// ① 点值区 → 松手进入文本编辑(值不变)。
			Draw(Frame({ valueCenter.X, valueCenter.Y }, true, true, false));
			Draw(Frame({ valueCenter.X, valueCenter.Y }, false, false, true));
			CHECK(Near(value, 0.45f));
			Draw(Frame({ valueCenter.X, valueCenter.Y }, false, false, false));
			CHECK(Kind() == "text-field");   // 编辑态:值区此刻是文本输入(kind 切换 + 缓冲文本)
			CHECK(accessibility.Find(barId) != nullptr && accessibility.Find(barId)->Value == "0.45");
			// ② 编辑态下直接按条体:同一帧先提交编辑,再按条体位置改值(中点 = 0.5)。
			const float barMidX = barZone.X + barZone.W * 0.5f;
			CHECK(Draw(Frame({ barMidX, barZone.Y + 11.0f }, true, true, false)));
			CHECK(Near(value, 0.5f));
			// ③ 按住继续跟随(绝对位置映射),④ 拖出条体右端 → 夹到 1.0,⑤ 松手收口。
			CHECK(Draw(Frame({ barZone.X + barZone.W * 0.9f, barZone.Y + 11.0f }, true, false, false)));
			CHECK(Near(value, 0.9f));
			CHECK(Kind() == "slider");       // 编辑已结束(不再吃掉后续拖动)
			CHECK(Draw(Frame({ barZone.X + barZone.W + 40.0f, barZone.Y + 11.0f }, true, false, false)));
			CHECK(Near(value, 1.0f));
			Draw(Frame({ barZone.X + barZone.W + 40.0f, barZone.Y + 11.0f }, false, false, true));
			CHECK(Near(value, 1.0f));
			// ⑥ 非法缓冲:编辑态下按条体仍先走 Enter 的提交路径 —— 保留原值、给红框,并退出编辑。
			Draw(Frame({ valueCenter.X, valueCenter.Y }, true, true, false));
			Draw(Frame({ valueCenter.X, valueCenter.Y }, false, false, true));
			{
				WuiInputState typing;
				typing.ViewportSize = { 1280.0f, 720.0f };
				typing.MousePos = { valueCenter.X, valueCenter.Y };
				typing.TextInput = { 'a', 'b', 'c' };   // 覆盖"进入编辑时全选"的缓冲 → 非法文本
				Draw(typing);
			}
			const float beforeInvalid = value;
			bool dangerDrawn = false;
			{
				accessibility.BeginFrame("main", { 1280.0f, 720.0f });
				accessibility.SetPanel("test.mat-ui3a");
				ctx.BeginFrame(Frame({ barMidX, barZone.Y + 11.0f }, true, true, false));
				DragBarFloat(ctx, barId, barRect, value, 0.0f, 1.0f, theme, style);
				for (const WuiDrawCommand& command : ctx.Commands())
					if (command.Kind == WuiDrawKind::RectOutline && command.Rect.W == valueZone.W
						&& command.Rect.X == valueZone.X && SameColor(command.Color, theme.Danger))
						dangerDrawn = true;
				ctx.EndFrame();
			}
			CHECK(beforeInvalid >= 0.0f && beforeInvalid <= 1.0f);
			CHECK(dangerDrawn);                       // 非法缓冲 → 与 Enter 提交失败同一条反馈(红框)
			CHECK(Near(value, 0.5f));                 // 提交没有写坏值;条体按下立刻生效
			// 收尾:松开这次按下,避免把按下态带出本节。
			Draw(Frame({ barMidX, barZone.Y + 11.0f }, false, false, true));
			Draw(Frame({ barMidX, barZone.Y + 11.0f }, false, false, false));
			CHECK(Kind() == "slider");                // 编辑态确实结束了(不是"看着提交、其实还在编辑")
			accessibility.SetEnabled(false);
			accessibility.Clear();
		}

		// 31. MAT-UI3a:Vec2Field / Vec4Field(与 Vec3Field 同一份实现)。
		//     ① 排布:Vec2 = 一行两段;Vec4 = 2×2(第一行 x y / 第二行 z w),layout=1 = 竖排;
		//     ② 无障碍:整体 kind=vec2-field/vec4-field、value='x,y' / 'x,y,z,w',每个分量一个
		//        kind=vec2-axis/vec4-axis 子节点(id = DerivedChildId(id,'.axis.',i)、label 大写单字母);
		//     ③ 拖动改值逐帧生效、只动当前分量;Vec3Field 的老排布/老 id 口径不变(§32 再钉一次)。
		{
			WuiAccessibility& accessibility = WuiAccessibility::Get();
			accessibility.SetEnabled(true);
			const WuiTheme theme {};
			const auto ChildRect = [&](WuiId parent, size_t index)
			{
				const std::string key = std::to_string(parent) + ".axis." + std::to_string(index);
				const WuiAccessNode* node = accessibility.Find(HashId(key.c_str()));
				return node != nullptr ? node->Rect : WuiRect { 0.0f, 0.0f, 0.0f, 0.0f };
			};
			const auto ChildNode = [&](WuiId parent, size_t index)
			{
				const std::string key = std::to_string(parent) + ".axis." + std::to_string(index);
				return accessibility.Find(HashId(key.c_str()));
			};
			const auto Frame = [](glm::vec2 pos, bool down, bool clicked, bool released)
			{
				WuiInputState input;
				input.ViewportSize = { 1280.0f, 720.0f };
				input.MousePos = pos;
				input.MouseDown[0] = down;
				input.MouseClicked[0] = clicked;
				input.MouseReleased[0] = released;
				return input;
			};
			// ---- Vec2Field:一行两段 ----
			const WuiId vec2Id = HashId("test.mat-ui3a.vec2");
			const WuiRect vec2Rect { 100.0f, 300.0f, 220.0f, 24.0f };
			glm::vec2 vec2Value { 0.0f, 1.0f };
			WuiContext ctx2;
			const auto Draw2 = [&](const WuiInputState& input)
			{
				accessibility.BeginFrame("main", { 1280.0f, 720.0f });
				accessibility.SetPanel("test.mat-ui3a");
				ctx2.BeginFrame(input);
				const bool changed = Vec2Field(ctx2, vec2Id, vec2Rect, vec2Value, 0.01f, -100.0f, 100.0f, theme, 0);
				ctx2.EndFrame();
				return changed;
			};
			Draw2(Frame({ 0.0f, 0.0f }, false, false, false));
			{
				const WuiAccessNode* overall = accessibility.Find(vec2Id);
				CHECK(overall != nullptr && overall->Kind == "vec2-field");
				CHECK(overall != nullptr && overall->Value == "0.00,1.00");
			}
			// 槽宽 = (220 − PadSmall×1) / 2 = 108;分量 X 的槽 = {100,300,108,24}、Y = {212,300,...}。
			const WuiRect xSlot = ChildRect(vec2Id, 0);
			const WuiRect ySlot = ChildRect(vec2Id, 1);
			CHECK(Near(xSlot.X, 100.0f) && Near(xSlot.W, 108.0f) && Near(xSlot.Y, 300.0f) && Near(xSlot.H, 24.0f));
			CHECK(Near(ySlot.X, 212.0f) && Near(ySlot.W, 108.0f));
			CHECK(ChildNode(vec2Id, 0) != nullptr && ChildNode(vec2Id, 0)->Kind == "vec2-axis"
				&& ChildNode(vec2Id, 0)->Label == "X");
			CHECK(ChildNode(vec2Id, 1) != nullptr && ChildNode(vec2Id, 1)->Label == "Y");
			// 拖动 X 分量:按下即取焦点,位移 > 1.5px 进入拖动 → value.x = 0 + 10×0.01 = 0.10,Y 不动。
			const glm::vec2 xCenter { xSlot.X + xSlot.W * 0.5f, xSlot.Y + xSlot.H * 0.5f };
			CHECK(!Draw2(Frame(xCenter, true, true, false)));
			CHECK(Draw2(Frame({ xCenter.x + 10.0f, xCenter.y }, true, false, false)));
			CHECK(Near(vec2Value.x, 0.10f) && Near(vec2Value.y, 1.0f));
			CHECK(Draw2(Frame({ xCenter.x + 25.0f, xCenter.y }, true, false, false)));
			CHECK(Near(vec2Value.x, 0.25f) && Near(vec2Value.y, 1.0f));
			Draw2(Frame({ xCenter.x + 25.0f, xCenter.y }, false, false, true));
			// 键盘:焦点在整体上,Up/Down 按 |speed| 步进当前分量(刚拖过的 X)。
			{
				WuiInputState input;
				input.ViewportSize = { 1280.0f, 720.0f };
				input.MousePos = xCenter;
				input.KeyPressed = { static_cast<uint32_t>(World::KeyCodes::Up) };
				CHECK(Draw2(input));
				CHECK(Near(vec2Value.x, 0.26f));
			}
			// 竖排:两行等分(窄列口径)。
			const WuiId vec2VerticalId = HashId("test.mat-ui3a.vec2.vertical");
			glm::vec2 vec2Vertical { 0.0f, 1.0f };
			WuiContext ctx2v;
			accessibility.BeginFrame("main", { 1280.0f, 720.0f });
			accessibility.SetPanel("test.mat-ui3a");
			ctx2v.BeginFrame(Frame({ 0.0f, 0.0f }, false, false, false));
			Vec2Field(ctx2v, vec2VerticalId, { 100.0f, 300.0f, 220.0f, 48.0f }, vec2Vertical,
				0.01f, -100.0f, 100.0f, theme, 1);
			ctx2v.EndFrame();
			CHECK(Near(ChildRect(vec2VerticalId, 0).H, 24.0f) && Near(ChildRect(vec2VerticalId, 1).Y, 324.0f));
			// ---- Vec4Field:2×2 默认排布 + 拖 W 分量 ----
			const WuiId vec4Id = HashId("test.mat-ui3a.vec4");
			const WuiRect vec4Rect { 100.0f, 400.0f, 220.0f, 52.0f };
			glm::vec4 vec4Value { 0.0f, 1.0f, 0.0f, 1.0f };
			WuiContext ctx4;
			const auto Draw4 = [&](const WuiInputState& input)
			{
				accessibility.BeginFrame("main", { 1280.0f, 720.0f });
				accessibility.SetPanel("test.mat-ui3a");
				ctx4.BeginFrame(input);
				const bool changed = Vec4Field(ctx4, vec4Id, vec4Rect, vec4Value, 0.01f, -100.0f, 100.0f, theme, 0);
				ctx4.EndFrame();
				return changed;
			};
			Draw4(Frame({ 0.0f, 0.0f }, false, false, false));
			{
				const WuiAccessNode* overall = accessibility.Find(vec4Id);
				CHECK(overall != nullptr && overall->Kind == "vec4-field");
				CHECK(overall != nullptr && overall->Value == "0.00,1.00,0.00,1.00");
			}
			// 2×2:槽宽 = (220 − 4) / 2 = 108,行高 = 52 / 2 = 26 → x{100,400} y{212,400} z{100,426} w{212,426}。
			CHECK(Near(ChildRect(vec4Id, 0).X, 100.0f) && Near(ChildRect(vec4Id, 0).Y, 400.0f)
				&& Near(ChildRect(vec4Id, 0).W, 108.0f) && Near(ChildRect(vec4Id, 0).H, 26.0f));
			CHECK(Near(ChildRect(vec4Id, 1).X, 212.0f) && Near(ChildRect(vec4Id, 1).Y, 400.0f));
			CHECK(Near(ChildRect(vec4Id, 2).X, 100.0f) && Near(ChildRect(vec4Id, 2).Y, 426.0f));
			CHECK(Near(ChildRect(vec4Id, 3).X, 212.0f) && Near(ChildRect(vec4Id, 3).Y, 426.0f));
			CHECK(ChildNode(vec4Id, 3) != nullptr && ChildNode(vec4Id, 3)->Label == "W");
			const WuiRect wSlot = ChildRect(vec4Id, 3);
			const glm::vec2 wCenter { wSlot.X + wSlot.W * 0.5f, wSlot.Y + wSlot.H * 0.5f };
			CHECK(!Draw4(Frame(wCenter, true, true, false)));
			CHECK(Draw4(Frame({ wCenter.x + 25.0f, wCenter.y }, true, false, false)));
			CHECK(Near(vec4Value.w, 1.25f) && Near(vec4Value.x, 0.0f) && Near(vec4Value.y, 1.0f)
				&& Near(vec4Value.z, 0.0f));
			Draw4(Frame({ wCenter.x + 25.0f, wCenter.y }, false, false, true));
			// 竖排:四行等分。
			const WuiId vec4VerticalId = HashId("test.mat-ui3a.vec4.vertical");
			glm::vec4 vec4Vertical { 0.0f, 1.0f, 0.0f, 1.0f };
			WuiContext ctx4v;
			accessibility.BeginFrame("main", { 1280.0f, 720.0f });
			accessibility.SetPanel("test.mat-ui3a");
			ctx4v.BeginFrame(Frame({ 0.0f, 0.0f }, false, false, false));
			Vec4Field(ctx4v, vec4VerticalId, { 100.0f, 400.0f, 220.0f, 96.0f }, vec4Vertical,
				0.01f, -100.0f, 100.0f, theme, 1);
			ctx4v.EndFrame();
			for (size_t axis = 1; axis < 4; ++axis)
				CHECK(Near(ChildRect(vec4VerticalId, axis).Y, 400.0f + 24.0f * static_cast<float>(axis)));
			accessibility.SetEnabled(false);
			accessibility.Clear();
		}

		// 32. MAT-UI3a:Vec3Field 的老口径没漂(2/3/4 分量共用 VecFieldCore 之后):
		//     老槽宽公式 (W − PadSmall×2)/3、老排布、老 id/kind 全部逐条保留。
		{
			WuiAccessibility& accessibility = WuiAccessibility::Get();
			accessibility.SetEnabled(true);
			const WuiTheme theme {};
			const WuiId vec3Id = HashId("test.mat-ui3a.vec3");
			const WuiRect vec3Rect { 100.0f, 500.0f, 220.0f, 24.0f };
			glm::vec3 vec3Value { 0.0f, 1.0f, 0.0f };
			WuiContext ctx;
			accessibility.BeginFrame("main", { 1280.0f, 720.0f });
			accessibility.SetPanel("test.mat-ui3a");
			WuiInputState input;
			input.ViewportSize = { 1280.0f, 720.0f };
			ctx.BeginFrame(input);
			Vec3Field(ctx, vec3Id, vec3Rect, vec3Value, 0.01f, -100.0f, 100.0f, theme, 0);
			ctx.EndFrame();
			const WuiAccessNode* overall = accessibility.Find(vec3Id);
			CHECK(overall != nullptr && overall->Kind == "vec3-field" && overall->Value == "0.00,1.00,0.00");
			const float slotW = (vec3Rect.W - theme.PadSmall * 2.0f) / 3.0f;
			for (size_t axis = 0; axis < 3; ++axis)
			{
				const std::string key = std::to_string(vec3Id) + ".axis." + std::to_string(axis);
				const WuiAccessNode* node = accessibility.Find(HashId(key.c_str()));
				CHECK(node != nullptr && node->Kind == "vec3-axis");
				CHECK(node != nullptr && Near(node->Rect.X, vec3Rect.X + (slotW + theme.PadSmall)
					* static_cast<float>(axis)));
				CHECK(node != nullptr && Near(node->Rect.W, slotW) && Near(node->Rect.H, vec3Rect.H));
			}
			const WuiAccessNode* firstAxis = accessibility.Find(HashId((std::to_string(vec3Id) + ".axis.0").c_str()));
			CHECK(firstAxis != nullptr && firstAxis->Label == "X");
			accessibility.SetEnabled(false);
			accessibility.Clear();
		}

		// 33. MAT-UI3a:焦点环视觉(圆角细描边 + 外发光,accent 低透明度)。
		//     用户原话:「这个聚焦选中能否按照人类美学重新设计下」。这里把新口径钉成命令流事实:
		//     ① 没有焦点 → 什么都不画(既有不变量);
		//     ② 有焦点 → 恰好两笔 overlay 描边:主环(控件自身矩形、1.25px、圆角 theme.Radius、
		//        基色 × 0.72)+ 外发光(外扩 1.5px、2.5px、圆角 theme.Radius + 2.5、基色 × 0.16);
		//     ③ 可辨识性:主环 alpha ≥ 0.7(不是"看不到"的装饰),基色仍可被 border.focus 覆盖。
		{
			WuiTheme theme {};
			const WuiId ringId = HashId("test.mat-ui3a.focusring");
			const WuiRect rect { 40.0f, 40.0f, 120.0f, 24.0f };
			WuiContext ctx;
			WuiInputState input;
			input.ViewportSize = { 1280.0f, 720.0f };
			ctx.BeginFrame(input);
			DrawFocusRing(ctx, rect, ringId, theme);   // 焦点不在这件上 → 无输出
			CHECK(ctx.OverlayCommands().empty());
			ctx.EndFrame();
			ctx.BeginFrame(input);
			ctx.SetFocus(ringId);
			DrawFocusRing(ctx, rect, ringId, theme);
			CHECK(ctx.OverlayCommands().size() == 2);
			if (ctx.OverlayCommands().size() == 2)
			{
				const WuiDrawCommand& core = ctx.OverlayCommands()[0];
				const WuiDrawCommand& glow = ctx.OverlayCommands()[1];
				CHECK(core.Kind == WuiDrawKind::RectOutline && glow.Kind == WuiDrawKind::RectOutline);
				CHECK(Near(core.Rect.X, rect.X) && Near(core.Rect.Y, rect.Y)
					&& Near(core.Rect.W, rect.W) && Near(core.Rect.H, rect.H));
				CHECK(Near(core.Rounding, theme.Radius) && Near(core.Thickness, 1.25f));
				CHECK(SameColor(core.Color, DimmedRing(theme.FocusRing, 0.72f)));
				CHECK(core.Color.A >= 0.7f);            // 可辨识:主环不是"几乎透明"
				CHECK(Near(glow.Rect.X, rect.X - 1.5f) && Near(glow.Rect.Y, rect.Y - 1.5f)
					&& Near(glow.Rect.W, rect.W + 3.0f) && Near(glow.Rect.H, rect.H + 3.0f));
				CHECK(Near(glow.Rounding, theme.Radius + 2.5f) && Near(glow.Thickness, 2.5f));
				CHECK(SameColor(glow.Color, DimmedRing(theme.FocusRing, 0.16f)));
			}
			ctx.EndFrame();
			// border.focus 覆盖 = 换基色(下面这条同时证明"覆盖不会绕过低透明度口径")。
			const WuiColor override { 0.2f, 0.4f, 0.9f, 1.0f };
			WuiContext ctxOverride;
			ctxOverride.BeginFrame(input);
			ctxOverride.SetFocus(ringId);
			DrawFocusRing(ctxOverride, rect, ringId, theme, &override);
			CHECK(ctxOverride.OverlayCommands().size() == 2);
			if (ctxOverride.OverlayCommands().size() == 2)
			{
				CHECK(SameColor(ctxOverride.OverlayCommands()[0].Color, DimmedRing(override, 0.72f)));
				CHECK(SameColor(ctxOverride.OverlayCommands()[1].Color, DimmedRing(override, 0.16f)));
			}
			ctxOverride.EndFrame();
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

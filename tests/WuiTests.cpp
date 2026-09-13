#include "World/WUI/WuiCore.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiOperationLog.h"
#include "World/WUI/WuiUndoStack.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/WuiDock.h"
#include "World/WUI/WuiJson.h"
#include "World/WUI/WuiLayoutStore.h"
#include "World/WUI/WuiWidget.h"

#include <filesystem>
#include <cstdio>
#include <functional>
#include <set>
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

		std::printf("World.Wui: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Wui: FAILED: %s\n", error.what());
		return 1;
	}
}

// P2 W3c:脚本 UI 绑定(ui.panel/text/button/checkbox/slider/image/list/grid/rows/columns)
// 的 headless 回归。
//
// 覆盖:命令生成与矩形、按钮命中/未命中、checkbox/slider 值往返、list/grid 点击返回索引与
// 越界 selected 夹紧、回调抛错只停自身、没有 OnUI 的实例零命令、rows/columns 几何、
// ui.* 只能在 UI 阶段调用,以及 ReloadScript 后 OnUiFunc 的整体交换。
#include "wldpch.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiCore.h"
#include "World/WUI/WuiWidgets.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using namespace World;
	using namespace World::Wui;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	bool Near(double left, double right, double epsilon = 1e-4)
	{
		return std::fabs(left - right) <= epsilon;
	}

	WorldContext& TestContext()
	{
		static WorldContext context;
		return context;
	}

	Ref<Scene> CreateSceneWithEntity(Entity* outEntity, const char* scriptPath)
	{
		Ref<Scene> scene = CreateRef<Scene>(TestContext());
		Entity entity = Entity::CreateEntity(scene.get(), "ui probe");
		entity.AddComponent<LuaScriptComponent>(scriptPath);
		ScriptEngine::OnCreateScript(entity.GetComponent<LuaScriptComponent>(), entity);
		if (outEntity)
			*outEntity = entity;
		return scene;
	}

	ScriptValue Field(LuaScriptComponent& script, const char* name)
	{
		return script.ScriptTable.GetField(name);
	}

	bool BoolField(LuaScriptComponent& script, const char* name)
	{
		bool value = false;
		if (!Field(script, name).AsBool(&value))
			throw std::runtime_error(std::string("script field '") + name + "' is not a boolean");
		return value;
	}

	double NumberField(LuaScriptComponent& script, const char* name)
	{
		double value = 0.0;
		if (!Field(script, name).AsNumber(&value))
			throw std::runtime_error(std::string("script field '") + name + "' is not a number");
		return value;
	}

	void SetBoolField(LuaScriptComponent& script, const char* name, bool value)
	{
		CHECK(script.ScriptTable.SetField(name, ScriptValue::Boolean(value)));
	}

	void SetNumberField(LuaScriptComponent& script, const char* name, double value)
	{
		CHECK(script.ScriptTable.SetField(name, ScriptValue::Number(value)));
	}

	std::size_t DrawFrame(Scene& scene, WuiContext& context, const WuiInputState& input)
	{
		context.BeginFrame(input);
		const std::size_t failures = ScriptEngine::DrawScriptUi(scene, context);
		context.EndFrame();
		return failures;
	}

	std::size_t ClickAt(Scene& scene, WuiContext& context, glm::vec2 position)
	{
		WuiInputState input;
		input.ViewportSize = { 800, 600 };
		input.MousePos = position;
		input.MouseDown[0] = true;
		input.MouseClicked[0] = true;
		return DrawFrame(scene, context, input);
	}

	const WuiDrawCommand* FindCommand(const std::vector<WuiDrawCommand>& commands, WuiDrawKind kind,
		const WuiRect& rect)
	{
		for (const WuiDrawCommand& command : commands)
		{
			if (command.Kind != kind)
				continue;
			if (Near(command.Rect.X, rect.X) && Near(command.Rect.Y, rect.Y) &&
				Near(command.Rect.W, rect.W) && Near(command.Rect.H, rect.H))
				return &command;
		}
		return nullptr;
	}

	const WuiDrawCommand* FindText(const std::vector<WuiDrawCommand>& commands, const std::string& text)
	{
		for (const WuiDrawCommand& command : commands)
			if (command.Kind == WuiDrawKind::Text && command.Text == text)
				return &command;
		return nullptr;
	}

	std::size_t CountText(const std::vector<WuiDrawCommand>& commands, const std::string& text)
	{
		std::size_t count = 0;
		for (const WuiDrawCommand& command : commands)
			if (command.Kind == WuiDrawKind::Text && command.Text == text)
				++count;
		return count;
	}

	// 1.命令生成:panel/text/button 的矩形与数量,rows/columns 的几何。
	void CommandsAndLayout()
	{
		Entity entity;
		Ref<Scene> scene = CreateSceneWithEntity(&entity, "scripts/tests/UiProbe.lua");
		LuaScriptComponent& script = entity.GetComponent<LuaScriptComponent>();
		CHECK(script.State == ScriptInstanceState::Running);
		CHECK(script.OnUiFunc.IsValid());

		WuiContext context;
		WuiInputState input;
		input.ViewportSize = { 800, 600 };
		CHECK(DrawFrame(*scene, context, input) == 0);

		const std::vector<WuiDrawCommand>& commands = context.Commands();
		const WuiRect panel { 10, 10, 300, 200 };
		const WuiRect button { 20, 60, 100, 24 };
		CHECK(FindCommand(commands, WuiDrawKind::Rect, panel) != nullptr);
		CHECK(FindCommand(commands, WuiDrawKind::RectOutline, panel) != nullptr);
		CHECK(FindCommand(commands, WuiDrawKind::Rect, button) != nullptr);
		CHECK(FindCommand(commands, WuiDrawKind::RectOutline, button) != nullptr);

		const WuiDrawCommand* title = FindText(commands, "HUD");
		CHECK(title != nullptr && Near(title->Rect.X, 20.0) && Near(title->Rect.Y, 14.0));
		const WuiDrawCommand* hello = FindText(commands, "Hello");
		CHECK(hello != nullptr && Near(hello->Rect.X, 20.0) && Near(hello->Rect.Y, 40.0) &&
			Near(hello->FontSize, 16.0));
		const WuiDrawCommand* go = FindText(commands, "Go");
		CHECK(go != nullptr && Near(go->Rect.X, 28.0) && Near(go->Rect.Y, 64.5));
		CHECK(CountText(commands, "Hello") == 1);
		CHECK(CountText(commands, "Go") == 1);

		// ui.rows(330,10,200,62,3,4):(200-8)/3 = 64,间距 4,x2 = 330+64+4 = 398。
		// ui.columns(330,90,100,62,3,4):(62-8)/3 = 18,间距 4,y2 = 90+18+4 = 112。
		CHECK(NumberField(script, "rowsCount") == 3.0);
		CHECK(Near(NumberField(script, "rowsW"), 64.0));
		CHECK(Near(NumberField(script, "rowsH"), 62.0));
		CHECK(Near(NumberField(script, "rowsX2"), 398.0));
		CHECK(NumberField(script, "columnsCount") == 3.0);
		CHECK(Near(NumberField(script, "columnsH"), 18.0));
		CHECK(Near(NumberField(script, "columnsY2"), 112.0));

		CHECK(BoolField(script, "checked") == false);
		CHECK(Near(NumberField(script, "sliderValue"), 0.5));
		CHECK(NumberField(script, "listIndex") == 1.0);
		CHECK(NumberField(script, "gridIndex") == 1.0);
	}

	// 2.按钮命中/未命中 + checkbox/slider 值往返 + list/grid 索引与越界夹紧。
	void InputRoundTrips()
	{
		Entity entity;
		Ref<Scene> scene = CreateSceneWithEntity(&entity, "scripts/tests/UiProbe.lua");
		LuaScriptComponent& script = entity.GetComponent<LuaScriptComponent>();
		WuiContext context;

		// 未命中:点击远处不应改变任何脚本字段。
		CHECK(ClickAt(*scene, context, { 500, 500 }) == 0);
		CHECK(NumberField(script, "clicks") == 0.0);
		CHECK(BoolField(script, "checked") == false);
		CHECK(Near(NumberField(script, "sliderValue"), 0.5));

		// 命中按钮中心 {20,60,100,24}。
		CHECK(ClickAt(*scene, context, { 70, 72 }) == 0);
		CHECK(NumberField(script, "clicks") == 1.0);

		// checkbox {20,90,120,20} 点击后返回新值并写回脚本字段。
		CHECK(ClickAt(*scene, context, { 80, 100 }) == 0);
		CHECK(BoolField(script, "checked") == true);

		// slider {20,116,120,20},点在 25% 处 → 0 + 0.25 * 10 = 2.5。
		CHECK(ClickAt(*scene, context, { 50, 126 }) == 0);
		CHECK(Near(NumberField(script, "sliderValue"), 2.5));

		// list 第二行(beta):行高 24,第一行 y=64..88,第二行 y=88..112。
		CHECK(ClickAt(*scene, context, { 200, 100 }) == 0);
		CHECK(NumberField(script, "listIndex") == 2.0);

		// grid 第二格:cell {204,144,52,46}。
		CHECK(ClickAt(*scene, context, { 230, 160 }) == 0);
		CHECK(NumberField(script, "gridIndex") == 2.0);

		// 越界 selected:夹紧到最后一个合法索引并交回脚本(3 行 / 2 格)。
		SetNumberField(script, "listIndex", 99.0);
		SetNumberField(script, "gridIndex", 99.0);
		WuiInputState idle;
		idle.ViewportSize = { 800, 600 };
		CHECK(DrawFrame(*scene, context, idle) == 0);
		CHECK(NumberField(script, "listIndex") == 3.0);
		CHECK(NumberField(script, "gridIndex") == 2.0);

		// 夹紧后仍然可以正常点击第一行(返回 1)。
		CHECK(ClickAt(*scene, context, { 200, 72 }) == 0);
		CHECK(NumberField(script, "listIndex") == 1.0);
	}

	// 3.单个实例的 OnUI 抛错只把自己置 Faulted,其它实例继续画且可交互。
	void ErrorIsolation()
	{
		Ref<Scene> scene = CreateRef<Scene>(TestContext());
		Entity goodEntity = Entity::CreateEntity(scene.get(), "ui good");
		goodEntity.AddComponent<LuaScriptComponent>("scripts/tests/UiProbe.lua");
		ScriptEngine::OnCreateScript(goodEntity.GetComponent<LuaScriptComponent>(), goodEntity);
		Entity badEntity = Entity::CreateEntity(scene.get(), "ui bad");
		badEntity.AddComponent<LuaScriptComponent>("scripts/tests/UiProbe.lua");
		ScriptEngine::OnCreateScript(badEntity.GetComponent<LuaScriptComponent>(), badEntity);
		LuaScriptComponent& good = goodEntity.GetComponent<LuaScriptComponent>();
		LuaScriptComponent& bad = badEntity.GetComponent<LuaScriptComponent>();
		SetBoolField(bad, "failOnUi", true);

		WuiContext context;
		WuiInputState input;
		input.ViewportSize = { 800, 600 };
		CHECK(DrawFrame(*scene, context, input) == 1);
		CHECK(bad.State == ScriptInstanceState::Faulted);
		CHECK(bad.LastError.find("non-empty string") != std::string::npos);
		CHECK(good.State == ScriptInstanceState::Running);
		CHECK(FindText(context.Commands(), "Go") != nullptr);

		// 第二帧:Faulted 实例不再被调用(错误数回到 0),good 实例继续绘制。
		CHECK(DrawFrame(*scene, context, input) == 0);
		CHECK(FindText(context.Commands(), "Go") != nullptr);
		CHECK(ClickAt(*scene, context, { 70, 72 }) == 0);
		CHECK(NumberField(good, "clicks") == 1.0);
	}

	// 4.没有 OnUI 的实例不产生任何命令(用既有的 ServicesProbe 夹具)。
	void MissingOnUiEmitsNothing()
	{
		Entity entity;
		Ref<Scene> scene = CreateSceneWithEntity(&entity, "scripts/tests/ServicesProbe.lua");
		LuaScriptComponent& script = entity.GetComponent<LuaScriptComponent>();
		CHECK(script.State == ScriptInstanceState::Running);
		CHECK(!script.OnUiFunc.IsValid());

		WuiContext context;
		WuiInputState input;
		input.ViewportSize = { 800, 600 };
		CHECK(DrawFrame(*scene, context, input) == 0);
		CHECK(context.Commands().empty());
	}

	// 5.ui.* 只能由 DrawScriptUi 建立的作用域调用;作用域外是普通 Lua error(pcall 可捕)。
	void UiOutsideUiPhaseFails()
	{
		std::string error;
		CHECK(ScriptEngine::GetState().RunString(
			"UI_OUTSIDE_OK = false\n"
			"UI_OUTSIDE_ERROR = nil\n"
			"local ok, err = pcall(function() ui.button('x', 0, 0, 10, 10, 'x') end)\n"
			"UI_OUTSIDE_OK = ok\n"
			"UI_OUTSIDE_ERROR = err\n",
			"ui_outside_phase", &error));
		bool ok = true;
		CHECK(ScriptEngine::GetState().GetGlobal("UI_OUTSIDE_OK").AsBool(&ok));
		CHECK(!ok);
		std::string message;
		CHECK(ScriptEngine::GetState().GetGlobal("UI_OUTSIDE_ERROR").AsString(&message));
		CHECK(message.find("UI phase") != std::string::npos);
	}

	// 6.热重载整体交换 OnUiFunc(新引用),交换后仍然绘制并可交互。
	void ReloadSwapsUiCallback()
	{
		Entity entity;
		Ref<Scene> scene = CreateSceneWithEntity(&entity, "scripts/tests/UiProbe.lua");
		LuaScriptComponent& script = entity.GetComponent<LuaScriptComponent>();
		const int before = script.OnUiFunc.RefId();
		std::string diagnostics;
		CHECK(ScriptEngine::ReloadScript(script, &diagnostics));
		CHECK(script.OnUiFunc.IsValid());
		CHECK(script.OnUiFunc.RefId() != before);

		WuiContext context;
		WuiInputState input;
		input.ViewportSize = { 800, 600 };
		CHECK(DrawFrame(*scene, context, input) == 0);
		CHECK(FindText(context.Commands(), "Go") != nullptr);
		CHECK(ClickAt(*scene, context, { 70, 72 }) == 0);
		CHECK(NumberField(script, "clicks") == 1.0);
	}

	// 7.ui.image 在没有渲染设备(headless)时给出可读错误,而不是触发 GL 断言。
	void ImageWithoutRendererFailsReadably()
	{
		Entity entity;
		Ref<Scene> scene = CreateSceneWithEntity(&entity, "scripts/tests/UiProbe.lua");
		LuaScriptComponent& script = entity.GetComponent<LuaScriptComponent>();
		SetBoolField(script, "failOnImage", true);
		WuiContext context;
		WuiInputState input;
		input.ViewportSize = { 800, 600 };
		CHECK(DrawFrame(*scene, context, input) == 1);
		CHECK(script.State == ScriptInstanceState::Faulted);
		CHECK(script.LastError.find("initialized renderer device") != std::string::npos);
	}
}

int main()
{
	try
	{
		std::setvbuf(stdout, nullptr, _IONBF, 0);
		World::Log::Init();
		World::ScriptEngine::Init();

		const std::pair<const char*, void(*)()> tests[] = {
			{ "panel/text/button commands and layout helpers", CommandsAndLayout },
			{ "input round-trips through button/checkbox/slider/list/grid", InputRoundTrips },
			{ "OnUI errors only fault the throwing instance", ErrorIsolation },
			{ "a script without OnUI emits no commands", MissingOnUiEmitsNothing },
			{ "ui.* outside the UI phase fails readably", UiOutsideUiPhaseFails },
			{ "reload swaps the OnUI callback", ReloadSwapsUiCallback },
			{ "ui.image without a renderer fails readably", ImageWithoutRendererFailsReadably },
		};
		int failures = 0;
		for (const auto& [name, test] : tests)
		{
			try { test(); std::printf("[PASS] %s\n", name); }
			catch (const std::exception& error) { ++failures; std::fprintf(stderr, "[FAIL] %s: %s\n", name, error.what()); }
			catch (...) { ++failures; std::fprintf(stderr, "[FAIL] %s: unknown exception\n", name); }
		}
		World::ScriptEngine::Shutdown();
		if (failures == 0)
		{
			std::printf("World.LuauUiBinding: all checks passed\n");
			return 0;
		}
		std::fprintf(stderr, "World.LuauUiBinding: %d group(s) failed\n", failures);
		return 1;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Test setup failed: %s\n", error.what());
		if (World::ScriptEngine::IsInitialized()) World::ScriptEngine::Shutdown();
		return 1;
	}
	catch (...)
	{
		std::fprintf(stderr, "Test setup failed: unknown exception\n");
		if (World::ScriptEngine::IsInitialized()) World::ScriptEngine::Shutdown();
		return 1;
	}
}

// World.UiBindAnim — GameUI(M6)数据绑定 + 动效状态机/属性补间,headless。
//
// 覆盖(派工单验收):
//   ① 解析:const/ecs/script/service 四协议各一条 + 非法协议(foo:bar)可读报错;
//   ② 求值:注册假解析器后绑定表取值正确;未注册协议(service:)→ warning 且不影响绘制;
//   ③ 变更检测:同一版本号不重复求值,版本号变化后才更新(计数解析器证明调用次数);
//   ④ 动画:线性补间在 0/0.5/1 三点值;曲线注册;未知节点/属性可读报错;
//   ⑤ 确定性:同 dt 序列两次运行逐值相同。
//
// 本文件不构建、不渲染;只跑 headless 逻辑。

#include "World/UI/UiAnimator.h"
#include "World/UI/UiBinding.h"
#include "World/UI/UiDocument.h"
#include "World/UI/UiScreen.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using namespace World;
	using namespace World::UI;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	void CheckStr(const std::string& actual, const std::string& expected, const char* expression, int line)
	{
		if (actual != expected)
		{
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression +
				" (actual '" + actual + "', expected '" + expected + "')");
		}
	}
#define CHECK_STR(actual, expected) CheckStr((actual), (expected), #actual " == " #expected, __LINE__)

	void CheckNear(float actual, float expected, float tolerance, const char* expression, int line)
	{
		if (std::fabs(actual - expected) > tolerance)
		{
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression +
				" (actual " + std::to_string(actual) + ", expected " + std::to_string(expected) + ")");
		}
	}
#define CHECK_NEAR(actual, expected, tolerance) \
	CheckNear(static_cast<float>(actual), static_cast<float>(expected), static_cast<float>(tolerance), #actual " ~= " #expected, __LINE__)

	// ---- UiScreen 夹具 ----

	UiNode MakeNode(std::string id, std::string type)
	{
		UiNode node;
		node.Id = std::move(id);
		node.Type = std::move(type);
		return node;
	}

	void AddBinding(UiNode& node, std::string target, std::string source)
	{
		node.Bind.push_back(UiBindingDecl { std::move(target), std::move(source) });
	}

	UiDocument MakeDoc(std::vector<UiNode> roots)
	{
		UiDocument doc;
		doc.Screen = "HUD";
		doc.Nodes = std::move(roots);
		return doc;
	}

	void BuildScreen(UiScreen& screen, const UiDocument& doc)
	{
		std::string error;
		if (!screen.Build(doc, &error))
			throw std::runtime_error("screen build: " + error);
	}

	// ---- 注册制解析器夹具(计数用于变更检测断言)----

	int g_ResolveCount = 0;
	bool g_ResolveFail = false;

	bool CountingResolver(const UiBindingSource& source, void* context, std::string& out, std::string* error)
	{
		++g_ResolveCount;
		if (g_ResolveFail)
		{
			if (error)
				*error = "resolver failed on purpose";
			return false;
		}
		out = context != nullptr ? (*static_cast<const std::string*>(context) + ":" + source.Text) : source.Text;
		return true;
	}

	// ---- ① 解析 ----

	void TestParseSchemes()
	{
		UiBindingSource parsed;
		std::string error;

		CHECK(UiBinding::Parse("const:HP 100", parsed, &error));
		CHECK(parsed.Scheme == UiBindingScheme::Const);
		CHECK_STR(parsed.Text, "HP 100");
		CHECK(parsed.Parts.size() == 1);
		CHECK_STR(parsed.Parts[0], "HP 100");

		CHECK(UiBinding::Parse("ecs:Player/Health/Current", parsed, &error));
		CHECK(parsed.Scheme == UiBindingScheme::Ecs);
		CHECK_STR(parsed.Text, "Player/Health/Current");
		CHECK(parsed.Parts.size() == 3);
		CHECK_STR(parsed.Parts[0], "Player");
		CHECK_STR(parsed.Parts[1], "Health");
		CHECK_STR(parsed.Parts[2], "Current");

		// 2 段 = 查询首选实体(契约允许的形态)。
		CHECK(UiBinding::Parse("ecs:Health/Current", parsed, &error));
		CHECK(parsed.Parts.size() == 2);

		CHECK(UiBinding::Parse("script:scripts/player.luau.health", parsed, &error));
		CHECK(parsed.Scheme == UiBindingScheme::Script);
		CHECK(parsed.Parts.size() == 2);
		CHECK_STR(parsed.Parts[0], "scripts/player.luau");
		CHECK_STR(parsed.Parts[1], "health");

		CHECK(UiBinding::Parse("service:matchTimer", parsed, &error));
		CHECK(parsed.Scheme == UiBindingScheme::Service);
		CHECK(parsed.Parts.size() == 1);
		CHECK_STR(parsed.Parts[0], "matchTimer");

		// 非法:未知协议 / 缺前缀 / 空值 / 段非法 —— 每条都可读报错。
		error.clear();
		CHECK(!UiBinding::Parse("foo:bar", parsed, &error));
		CHECK(!error.empty());
		error.clear();
		CHECK(!UiBinding::Parse("const", parsed, &error));
		CHECK(!error.empty());
		error.clear();
		CHECK(!UiBinding::Parse("const:", parsed, &error));
		CHECK(!error.empty());
		error.clear();
		CHECK(!UiBinding::Parse("", parsed, &error));
		CHECK(!error.empty());
		error.clear();
		CHECK(!UiBinding::Parse("ecs:A//B", parsed, &error));
		CHECK(!error.empty());
		error.clear();
		CHECK(!UiBinding::Parse("ecs:A/B/C/D", parsed, &error));
		CHECK(!error.empty());
		error.clear();
		CHECK(!UiBinding::Parse("script:nodot", parsed, &error));
		CHECK(!error.empty());
		error.clear();
		CHECK(!UiBinding::Parse("service:", parsed, &error));
		CHECK(!error.empty());
	}

	// ---- ②a Attach:解析失败 = warning 且不入表 ----

	void TestAttachWarnings()
	{
		UiNode root = MakeNode("root", "Panel");
		UiNode bad = MakeNode("bad", "Label");
		AddBinding(bad, "text", "foo:bar");
		root.Children.push_back(std::move(bad));

		UiDocument doc = MakeDoc({ std::move(root) });
		UiScreen screen;
		BuildScreen(screen, doc);

		UiBindingTable table;
		table.Attach(screen);
		CHECK(table.Count() == 0);             // 解析失败不入表
		CHECK(table.Warnings().size() == 1);   // 但留一条可读 warning
		CHECK_STR(table.Warnings()[0].NodeId, "bad");
		CHECK_STR(table.Warnings()[0].Target, "text");
		CHECK_STR(table.Warnings()[0].Source, "foo:bar");
		CHECK(!table.Warnings()[0].Message.empty());
	}

	// ---- ②b 求值:注册解析器 / 未注册协议 warning ----

	void TestResolveAndWarnings()
	{
		CHECK(UiBinding::RegisterResolver("ecs", &CountingResolver));
		CHECK(UiBinding::FindResolver("ecs") == &CountingResolver);
		CHECK(UiBinding::FindResolver("const") != nullptr);    // 内置 const
		CHECK(UiBinding::FindResolver("service") == nullptr);  // 未注册
		CHECK(!UiBinding::RegisterResolver("", &CountingResolver));
		CHECK(!UiBinding::RegisterResolver("ecs", nullptr));

		UiNode root = MakeNode("root", "Panel");
		UiNode hp = MakeNode("hp", "Label");
		AddBinding(hp, "text", "ecs:Player/Health/Current");
		UiNode label = MakeNode("label", "Label");
		AddBinding(label, "text", "const:HP 100");
		UiNode svc = MakeNode("svc", "Label");
		AddBinding(svc, "text", "service:matchTimer");
		root.Children.push_back(std::move(hp));
		root.Children.push_back(std::move(label));
		root.Children.push_back(std::move(svc));

		UiDocument doc = MakeDoc({ std::move(root) });
		UiScreen screen;
		BuildScreen(screen, doc);

		UiBindingTable table;
		table.Attach(screen);
		CHECK(table.Count() == 3);             // 三条源都能解析
		CHECK(table.Warnings().empty());

		std::string context = "V1";
		g_ResolveCount = 0;
		g_ResolveFail = false;
		CHECK((table.Refresh(UiBindingDataSource { 1ull, &context }) == 2));  // ecs + const
		CHECK(g_ResolveCount == 1);            // 只有 ecs 走自定义解析器

		std::string value;
		CHECK(table.Value("hp", "text", value));
		CHECK_STR(value, "V1:Player/Health/Current");
		CHECK(table.Value("label", "text", value));
		CHECK_STR(value, "HP 100");
		CHECK(!table.HasValue("svc", "text"));  // 未注册协议:无值
		CHECK(table.Warnings().size() == 1);    // 一条 warning
		CHECK_STR(table.Warnings()[0].NodeId, "svc");
		CHECK(table.Warnings()[0].Message.find("service") != std::string::npos);

		// 求值失败 = warning,该条保留上一值(不影响绘制)。
		g_ResolveFail = true;
		CHECK((table.Refresh(UiBindingDataSource { 2ull, &context }) == 1));  // 仅 const 成功
		CHECK(table.Value("hp", "text", value));
		CHECK_STR(value, "V1:Player/Health/Current");
		CHECK(!table.Warnings().empty());
		g_ResolveFail = false;
	}

	// ---- ③ 变更检测 ----

	void TestChangeDetection()
	{
		CHECK(UiBinding::RegisterResolver("script", &CountingResolver));

		UiNode root = MakeNode("root", "Panel");
		UiNode hp = MakeNode("hp", "Label");
		AddBinding(hp, "text", "script:player.hp");
		root.Children.push_back(std::move(hp));

		UiDocument doc = MakeDoc({ std::move(root) });
		UiScreen screen;
		BuildScreen(screen, doc);

		UiBindingTable table;
		table.Attach(screen);
		CHECK(table.Count() == 1);

		std::string v1 = "V1";
		std::string v2 = "V2";
		std::string value;
		g_ResolveCount = 0;
		g_ResolveFail = false;

		CHECK((table.Refresh(UiBindingDataSource { 10ull, &v1 }) == 1));
		CHECK(g_ResolveCount == 1);
		CHECK(table.Value("hp", "text", value));
		CHECK_STR(value, "V1:player.hp");

		// 同版本号 + 不同 context:不重复求值(计数不变、值不变)。
		CHECK((table.Refresh(UiBindingDataSource { 10ull, &v2 }) == 0));
		CHECK(g_ResolveCount == 1);
		CHECK(table.Value("hp", "text", value));
		CHECK_STR(value, "V1:player.hp");

		// 版本号变化:才更新。
		CHECK((table.Refresh(UiBindingDataSource { 11ull, &v2 }) == 1));
		CHECK(g_ResolveCount == 2);
		CHECK(table.Value("hp", "text", value));
		CHECK_STR(value, "V2:player.hp");

		table.Reset();
		CHECK(table.Count() == 0);
	}

	// ---- ④ 补间 / 曲线 ----

	void TestTweenLinear()
	{
		UiAnimator animator;
		CHECK(animator.RegisterNode("btn"));
		CHECK(animator.HasNode("btn"));
		CHECK(animator.NodeCount() == 1);
		CHECK(!animator.RegisterNode(""));

		float value = -1.0f;
		std::string error;

		CHECK(animator.Tween("btn", "opacity", 0.0f, 1.0f, 1.0f, "Linear"));
		CHECK(animator.IsAnimating("btn", "opacity"));
		CHECK(animator.Current("btn", "opacity", value));
		CHECK_NEAR(value, 0.0f, 1e-6f);          // t = 0

		animator.Update(0.5f);
		CHECK(animator.Current("btn", "opacity", value));
		CHECK_NEAR(value, 0.5f, 1e-6f);          // t = 0.5
		std::string text;
		CHECK(animator.CurrentText("btn", "opacity", text));
		CHECK_STR(text, "0.5");

		animator.Update(0.5f);
		CHECK(animator.Current("btn", "opacity", value));
		CHECK_NEAR(value, 1.0f, 1e-6f);          // t = 1
		CHECK(!animator.IsAnimating("btn", "opacity"));
		CHECK(animator.CurrentText("btn", "opacity", text));
		CHECK_STR(text, "1");

		// dt 超长:钳到终点,不越界。
		CHECK(animator.Tween("btn", "opacity", 0.0f, 1.0f, 0.25f, "Linear"));
		animator.Update(10.0f);
		CHECK(animator.Current("btn", "opacity", value));
		CHECK_NEAR(value, 1.0f, 1e-6f);

		// duration <= 0:立即落到 to。
		CHECK(animator.Tween("btn", "scale", 0.0f, 2.0f, 0.0f, "Linear"));
		CHECK(animator.Current("btn", "scale", value));
		CHECK_NEAR(value, 2.0f, 1e-6f);

		// 未知节点 / 属性 / 曲线:可读报错。
		error.clear();
		CHECK(!animator.Current("nope", "opacity", value, &error));
		CHECK(!error.empty());
		error.clear();
		CHECK(!animator.Current("btn", "missing", value, &error));
		CHECK(!error.empty());
		error.clear();
		CHECK(!animator.Tween("nope", "opacity", 0.0f, 1.0f, 1.0f, "Linear", &error));
		CHECK(!error.empty());
		error.clear();
		CHECK(!animator.Tween("btn", "opacity", 0.0f, 1.0f, 1.0f, "Nope", &error));
		CHECK(!error.empty());
	}

	float StepCurve(float t)
	{
		return t < 1.0f ? 0.0f : 1.0f;
	}

	void TestCurves()
	{
		CHECK(UiAnimator::FindCurve("Linear") != nullptr);
		CHECK(UiAnimator::FindCurve("EaseInOut") != nullptr);
		CHECK(UiAnimator::FindCurve("Nope") == nullptr);
		CHECK(!UiAnimator::RegisterCurve("", &StepCurve));
		CHECK(!UiAnimator::RegisterCurve("Empty", nullptr));

		CHECK(UiAnimator::RegisterCurve("Step", &StepCurve));
		CHECK(UiAnimator::FindCurve("Step") == &StepCurve);

		UiAnimator animator;
		CHECK(animator.RegisterNode("n"));
		CHECK(animator.Tween("n", "x", 0.0f, 1.0f, 1.0f, "Step"));
		float value = -1.0f;
		animator.Update(0.5f);
		CHECK(animator.Current("n", "x", value));
		CHECK_NEAR(value, 0.0f, 1e-6f);
		animator.Update(0.5f);
		CHECK(animator.Current("n", "x", value));
		CHECK_NEAR(value, 1.0f, 1e-6f);

		// EaseInOut = smoothstep:x=0.25 → 0.15625;x=0.5 → 0.5。
		UiAnimator eased;
		CHECK(eased.RegisterNode("m"));
		CHECK(eased.Tween("m", "x", 0.0f, 1.0f, 1.0f, "EaseInOut"));
		eased.Update(0.25f);
		CHECK(eased.Current("m", "x", value));
		CHECK_NEAR(value, 0.15625f, 1e-5f);
		eased.Update(0.25f);
		CHECK(eased.Current("m", "x", value));
		CHECK_NEAR(value, 0.5f, 1e-6f);
		eased.Update(0.5f);
		CHECK(eased.Current("m", "x", value));
		CHECK_NEAR(value, 1.0f, 1e-6f);
	}

	// ---- ④b 状态机 ----

	void TestStateMachine()
	{
		UiAnimator animator;
		CHECK(animator.RegisterNode("btn"));
		CHECK(animator.DefineState("btn", "default", "opacity", 1.0f, 0.2f, "Linear"));
		CHECK(animator.DefineState("btn", "hover", "opacity", 0.5f, 0.2f, "Linear"));
		CHECK(animator.DefineState("btn", "disabled", "opacity", 0.25f, 0.0f, "Linear"));

		std::string error;
		float value = -1.0f;

		CHECK(animator.SetState("btn", "default"));
		CHECK_STR(std::string(animator.State("btn")), "default");
		CHECK(animator.Current("btn", "opacity", value));
		CHECK_NEAR(value, 1.0f, 1e-6f);          // 首个状态以目标为起点

		CHECK(animator.SetState("btn", "hover"));
		animator.Update(0.1f);                    // 1.0 → 0.5 的半程
		CHECK(animator.Current("btn", "opacity", value));
		CHECK_NEAR(value, 0.75f, 1e-6f);
		animator.Update(0.1f);
		CHECK(animator.Current("btn", "opacity", value));
		CHECK_NEAR(value, 0.5f, 1e-6f);
		CHECK_STR(std::string(animator.State("btn")), "hover");

		// 零时长状态:立即落定。
		CHECK(animator.SetState("btn", "disabled"));
		CHECK(animator.Current("btn", "opacity", value));
		CHECK_NEAR(value, 0.25f, 1e-6f);

		// 未登记节点 / 空状态:可读报错。
		error.clear();
		CHECK(!animator.SetState("nope", "hover", &error));
		CHECK(!error.empty());
		error.clear();
		CHECK(!animator.DefineState("nope", "hover", "opacity", 1.0f, 0.1f, "Linear", &error));
		CHECK(!error.empty());
		error.clear();
		CHECK(!animator.SetState("btn", "", &error));
		CHECK(!error.empty());
	}

	// ---- ⑤ 确定性 ----

	std::string RunDeterministicSequence()
	{
		UiAnimator animator;
		animator.RegisterNode("n");
		animator.Tween("n", "x", 0.0f, 3.0f, 2.0f, "EaseInOut");
		std::string out;
		const float steps[7] = { 0.37f, 0.11f, 0.52f, 0.4f, 0.05f, 0.8f, 0.25f };
		for (const float dt : steps)
		{
			animator.Update(dt);
			float value = 0.0f;
			animator.Current("n", "x", value);
			out += std::to_string(value);
			out += ';';
		}
		return out;
	}

	void TestDeterminism()
	{
		const std::string first = RunDeterministicSequence();
		const std::string second = RunDeterministicSequence();
		CHECK_STR(second, first);   // 同 dt 序列 → 逐值相同
	}
}

int main()
{
	try
	{
		TestParseSchemes();
		TestAttachWarnings();
		TestResolveAndWarnings();
		TestChangeDetection();
		TestTweenLinear();
		TestCurves();
		TestStateMachine();
		TestDeterminism();
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "World.UiBindAnim FAILED: %s\n", e.what());
		return 1;
	}
	std::printf("World.UiBindAnim OK\n");
	return 0;
}

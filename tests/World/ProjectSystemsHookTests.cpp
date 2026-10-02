// PECS-T4:WorldContext 场景系统挂载钩子(项目层 C++ 系统的挂载时机)契约回归。
//
// 被测接口(冻结,契约原文见 Engine/src/World/Core/WorldContext.h):
//   AddSceneSystemsHook / RemoveSceneSystemsHook / RunSceneAttachHooks / RunSceneDetachHooks
// 运行时接线(见 Engine/src/World/Scene/Scene.cpp):
//   Scene::OnRuntimeStart() 末尾 Attach;Scene::OnRuntimeStop() 只在 Running -> Stopping
//   那一次的开头 Detach。
//
// 覆盖(逐项断言,失败打印可读信息):
//   1. Attach 时机:Start 后 attachCalls == 1 且 detachCalls == 0;Stop 后 detachCalls == 1;
//      Attach 回调里引擎内建帧系统已就绪(证明挂在"内建帧系统 + Lua 系统脚本之后")。
//   2. 系统真的挂上了:Attach 里 RegisterSystem<ProbeSystem>() -> HasFrameSystem 为 true,
//      Detach 里 UnregisterFrameSystem -> 为 false。
//   3. 顺序:Attach = A -> B,Detach = B -> A(字符串序列断言)。
//   4. 幂等:同一对指针 Add 两次只调一次;Add(nullptr, ...) 不产生条目。
//   5. 重复停止:OnRuntimeStop() 连调两次 -> detachCalls == 1。
//   6. Remove:摘掉的钩子不再调用,同时保留的钩子仍照常调用。
//
// 本测试不初始化 ScriptEngine(其未初始化时 OnRuntimeStart 里的系统脚本加载整段跳过),
// 不依赖文件系统/内容目录;每个场景都先 OnRuntimeStop 再让 Scene 先于 WorldContext 离开作用域,
// 保证钩子表不会指向已销毁的场景(WorldContext.h 的悬垂契约)。

#include "World/Core/WorldContext.h"
#include "World/Scene/ISystem.h"
#include "World/Scene/Scene.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace World;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(...) Check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __LINE__)

	std::string JoinSequence(const std::vector<std::string>& items)
	{
		std::string text = "[";
		for (std::size_t i = 0; i < items.size(); ++i)
		{
			if (i != 0)
				text += ", ";
			text += items[i];
		}
		text += "]";
		return text;
	}

	void CheckSequence(const std::vector<std::string>& actual, const std::vector<std::string>& expected, int line)
	{
		if (actual == expected)
			return;
		throw std::runtime_error("line " + std::to_string(line) + ": hook call order mismatch; expected " +
			JoinSequence(expected) + " but got " + JoinSequence(actual));
	}
#define CHECK_SEQUENCE(actual, expected) CheckSequence((actual), (expected), __LINE__)

	// Attach 钩子挂载的探针系统。ISystem::Name() 是虚函数,不能声明为静态成员,
	// 所以静态名字用独立的 kName 常量暴露给 HasFrameSystem / UnregisterFrameSystem 使用。
	class ProbeSystem : public ISystem
	{
	public:
		static constexpr const char* kName = "WorldProjectSystemsHookProbeSystem";

		std::string_view Name() const override { return kName; }
		void Update(Scene&, Timestep) override {}
	};

	// ---- 场景 1:Attach 时机 + 系统真的挂上了 + 重复停止 ----
	int g_TimingAttachCalls = 0;
	int g_TimingDetachCalls = 0;
	bool g_BuiltinFrameSystemsReadyAtAttach = false;

	void TimingAttach(Scene& scene)
	{
		++g_TimingAttachCalls;
		// OnRuntimeStart 在跑钩子前已 EnsureDefaultFrameSystems():证明 Attach 位于"末尾"。
		g_BuiltinFrameSystemsReadyAtAttach = scene.HasFrameSystem("physics-2d") && scene.HasFrameSystem("camera-system");
		scene.RegisterSystem<ProbeSystem>();
	}

	void TimingDetach(Scene& scene)
	{
		++g_TimingDetachCalls;
		CHECK(scene.UnregisterFrameSystem(ProbeSystem::kName));
	}

	// ---- 场景 2:登记顺序 / 逆序撤销 ----
	std::vector<std::string> g_OrderLog;

	void OrderAttachA(Scene&) { g_OrderLog.emplace_back("attach:A"); }
	void OrderAttachB(Scene&) { g_OrderLog.emplace_back("attach:B"); }
	void OrderDetachA(Scene&) { g_OrderLog.emplace_back("detach:A"); }
	void OrderDetachB(Scene&) { g_OrderLog.emplace_back("detach:B"); }

	// ---- 场景 3:同一对指针重复 Add 幂等 / Add(nullptr, ...) 忽略 ----
	int g_IdempotentAttachCalls = 0;
	int g_IdempotentDetachCalls = 0;

	void IdempotentAttach(Scene&) { ++g_IdempotentAttachCalls; }
	void IdempotentDetach(Scene&) { ++g_IdempotentDetachCalls; }

	// ---- 场景 4:Remove 只摘指定的一对 ----
	int g_KeptAttachCalls = 0;
	int g_KeptDetachCalls = 0;
	int g_RemovedAttachCalls = 0;
	int g_RemovedDetachCalls = 0;

	void KeptAttach(Scene&) { ++g_KeptAttachCalls; }
	void KeptDetach(Scene&) { ++g_KeptDetachCalls; }
	void RemovedAttach(Scene&) { ++g_RemovedAttachCalls; }
	void RemovedDetach(Scene&) { ++g_RemovedDetachCalls; }
	void NeverRegisteredAttach(Scene&) { CHECK(false); }
	void NeverRegisteredDetach(Scene&) { CHECK(false); }
}

int main()
{
	try
	{
		using namespace World;

		// -----------------------------------------------------------------
		// 1. Attach 时机 + 2. 系统真的挂上了 + 5. 重复停止
		// -----------------------------------------------------------------
		{
			WorldContext context;
			Scene scene(context);
			context.AddSceneSystemsHook(&TimingAttach, &TimingDetach);

			CHECK(g_TimingAttachCalls == 0);
			CHECK(g_TimingDetachCalls == 0);
			CHECK(!scene.HasFrameSystem(ProbeSystem::kName));

			scene.OnRuntimeStart();
			CHECK(g_TimingAttachCalls == 1);
			CHECK(g_TimingDetachCalls == 0);
			CHECK(g_BuiltinFrameSystemsReadyAtAttach);
			CHECK(scene.IsRunning());
			CHECK(scene.HasFrameSystem(ProbeSystem::kName)); // Attach 里注册的系统真的挂上了

			// 已在运行态重复 Start:Scene 自身的幂等(IsActive 早退),不重复 Attach。
			scene.OnRuntimeStart();
			CHECK(g_TimingAttachCalls == 1);

			scene.OnRuntimeStop();
			CHECK(g_TimingDetachCalls == 1);
			CHECK(!scene.HasFrameSystem(ProbeSystem::kName)); // Detach 里把系统摘掉了
			CHECK(!scene.IsRunning());

			// 重复停止不再 Detach(只在 Running -> Stopping 那次跑)。
			scene.OnRuntimeStop();
			CHECK(g_TimingDetachCalls == 1);
			CHECK(g_TimingAttachCalls == 1);
		}

		// -----------------------------------------------------------------
		// 3. 登记顺序 Attach = A -> B,Detach = B -> A;Add(nullptr, ...) 被忽略
		// -----------------------------------------------------------------
		{
			WorldContext context;
			Scene scene(context);

			g_OrderLog.clear();
			context.AddSceneSystemsHook(nullptr, nullptr);
			context.AddSceneSystemsHook(nullptr, &OrderDetachB); // Attach 为空 -> 整条忽略
			context.AddSceneSystemsHook(&OrderAttachA, &OrderDetachA);
			context.AddSceneSystemsHook(&OrderAttachB, &OrderDetachB);

			const std::vector<std::string> expectedAttach{ "attach:A", "attach:B" };
			const std::vector<std::string> expectedDetach{ "attach:A", "attach:B", "detach:B", "detach:A" };

			scene.OnRuntimeStart();
			CHECK_SEQUENCE(g_OrderLog, expectedAttach);

			scene.OnRuntimeStop();
			CHECK_SEQUENCE(g_OrderLog, expectedDetach);
		}

		// -----------------------------------------------------------------
		// 4. 同一对指针重复 Add = no-op;Add(nullptr, detach) 不产生条目
		// -----------------------------------------------------------------
		{
			WorldContext context;
			Scene scene(context);

			context.AddSceneSystemsHook(nullptr, nullptr);
			context.AddSceneSystemsHook(nullptr, &IdempotentDetach); // 若被登记,Stop 时 detach 会被调用
			context.AddSceneSystemsHook(&IdempotentAttach, &IdempotentDetach);
			context.AddSceneSystemsHook(&IdempotentAttach, &IdempotentDetach); // 与上一条同一对 -> no-op

			scene.OnRuntimeStart();
			CHECK(g_IdempotentAttachCalls == 1);

			scene.OnRuntimeStop();
			CHECK(g_IdempotentDetachCalls == 1);
		}

		// -----------------------------------------------------------------
		// 6. Remove:摘掉的一对不再调用,保留的一对照常调用;摘未登记的对无副作用
		// -----------------------------------------------------------------
		{
			WorldContext context;
			Scene scene(context);

			context.AddSceneSystemsHook(&RemovedAttach, &RemovedDetach);
			context.AddSceneSystemsHook(&KeptAttach, &KeptDetach);
			context.RemoveSceneSystemsHook(&RemovedAttach, &RemovedDetach);
			context.RemoveSceneSystemsHook(&NeverRegisteredAttach, &NeverRegisteredDetach);

			scene.OnRuntimeStart();
			CHECK(g_RemovedAttachCalls == 0);
			CHECK(g_KeptAttachCalls == 1);

			scene.OnRuntimeStop();
			CHECK(g_RemovedDetachCalls == 0);
			CHECK(g_KeptDetachCalls == 1);
		}

		std::puts("[World.ProjectSystemsHook] All scene systems hook assertions passed.");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "[World.ProjectSystemsHook] Test failed: %s\n", e.what());
		return 1;
	}
}

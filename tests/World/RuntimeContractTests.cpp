// PECS-T5:宿主(Editor/Runtime)与 WorldRuntime.dll 的**运行期布局契约**回归。
//
// 背景(2026-10-02 实测崩溃):`Scene` 的数据成员布局同时被 DLL(分配/维护)与宿主
// (头文件内联访问器按宿主编译时的偏移直读内存)使用。两侧来自不同世代的头文件时,
// 宿主会按错误偏移读到"物理正在运行 + 垃圾世界指针",随后崩在物理调试绘制里。
// `RuntimeLayoutMatches()` 让这种不一致在 main 早期就被**干净拒绝**(退出码 3)。

#include "World/Core/RuntimeContract.h"
#include "World/Core/WorldContext.h"
#include "World/Modules/WeModule.h"
#include "World/Scene/Scene.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(...) Check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __LINE__)
}

int main()
{
	try
	{
		// 1. 同一次构建:宿主侧取值宏必须通过(这就是 Editor/Runtime main 里的自检)。
		CHECK(WE_RUNTIME_LAYOUT_MATCHES());

		// 2. 与宏同源的显式取值也必须通过(证明宏没有把 sizeof 交给 DLL 的导出符号)。
		CHECK(World::RuntimeLayoutMatches(sizeof(World::Scene), sizeof(World::WorldContext),
			World::Modules::WE_MODULE_ABI_VERSION));

		// 3. Scene 尺寸不一致(M8 删了 3 个成员 ⇒ sizeof 变小)必须被拒绝。
		CHECK(!World::RuntimeLayoutMatches(sizeof(World::Scene) + 1, sizeof(World::WorldContext),
			World::Modules::WE_MODULE_ABI_VERSION));
		CHECK(!World::RuntimeLayoutMatches(sizeof(World::Scene) - 1, sizeof(World::WorldContext),
			World::Modules::WE_MODULE_ABI_VERSION));

		// 4. WorldContext 尺寸不一致同样被拒绝。
		CHECK(!World::RuntimeLayoutMatches(sizeof(World::Scene), sizeof(World::WorldContext) + 1,
			World::Modules::WE_MODULE_ABI_VERSION));

		// 5. 模块 ABI 不一致也被拒绝(与 Game.dll 的等值门同口径)。
		CHECK(!World::RuntimeLayoutMatches(sizeof(World::Scene), sizeof(World::WorldContext),
			World::Modules::WE_MODULE_ABI_VERSION + 1));

		// 6. 真实构造一对 context/scene 之后仍然一致,并把**崩溃当时读到垃圾的两个值**钉住:
		//    没进运行时 ⇒ IsPhysics3DRunning() 必须是 false、PhysicsDebug 必须是 false
		//    (崩溃实测:这两个都因偏移错位读成 true,于是拿垃圾指针去 CollectDebugLines)。
		World::WorldContext context;
		World::Scene scene(context);
		CHECK(WE_RUNTIME_LAYOUT_MATCHES());
		CHECK(scene.GetWorldSettings().PhysicsDebug == false);
		CHECK(!scene.IsPhysics3DRunning());
		CHECK(scene.GetPhysics3DWorld() == nullptr);

		std::puts("[World.RuntimeContract] All runtime contract assertions passed.");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "[World.RuntimeContract] Test failed: %s\n", e.what());
		return 1;
	}
}

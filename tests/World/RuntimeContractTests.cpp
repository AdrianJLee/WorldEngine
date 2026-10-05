// PECS-T5:宿主(Editor/Runtime)与 WorldRuntime.dll 的**运行期布局契约**回归。
//
// 背景(2026-10-02 实测崩溃):`Scene` 的数据成员布局同时被 DLL(分配/维护)与宿主
// (头文件内联访问器按宿主编译时的偏移直读内存)使用。两侧来自不同世代的头文件时,
// 宿主会按错误偏移读到"物理正在运行 + 垃圾世界指针",随后崩在物理调试绘制里。
// `RuntimeLayoutMatches()` 让这种不一致在 main 早期就被**干净拒绝**(退出码 3)。
//
// 2026-10-05 扩展(数据布局门禁 B,标准 docs/dev/performance-and-data-layout.md §4.3):
// 从"只比 sizeof"升级为"sizeof + 成员偏移指纹"。理由是 sizeof 抓不住**等大小的成员重排**
// (把两个同尺寸成员换位、或把成员换成等大小的另一类型):偏移全变了、尺寸一模一样,
// 宿主按错误偏移直读内存 —— 与上面那次崩溃同一类事故,只是不再被尺寸变化兜住。
// 本文件用**合成的假指纹**证明比较真的覆盖了它(见第 3、4 组)。
#include "World/Core/RuntimeContract.h"
#include "World/Core/WorldContext.h"
#include "World/Modules/WeModule.h"
#include "World/Scene/Scene.h"

#include <cstdio>
#include <cstdint>
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

	// 宿主真实传给 RuntimeLayoutMatches 的五个值(宏在**本 TU** 展开,所以都是宿主视角)。
	const uint64_t kSceneSize = sizeof(World::Scene);
	const uint64_t kContextSize = sizeof(World::WorldContext);
	const uint64_t kSceneFp = World::Scene::LayoutFingerprint();
	const uint64_t kContextFp = World::WorldContext::LayoutFingerprint();
	const uint32_t kAbi = World::Modules::WE_MODULE_ABI_VERSION;
}

int main()
{
	try
	{
		// 1. 同一次构建:宿主侧取值宏必须通过(这就是 Editor/Runtime main 里的自检)。
		CHECK(WE_RUNTIME_LAYOUT_MATCHES());

		// 2. 与宏同源的显式取值也必须通过(证明宏没有把 sizeof/指纹交给 DLL 的导出符号)。
		CHECK(World::RuntimeLayoutMatches(kSceneSize, kContextSize, kSceneFp, kContextFp, kAbi));

		// 3. 尺寸不一致必须被拒绝(2026-10-02 的 M8 删了 3 个 Scene 成员,正是这一类)。
		CHECK(!World::RuntimeLayoutMatches(kSceneSize + 1, kContextSize, kSceneFp, kContextFp, kAbi));
		CHECK(!World::RuntimeLayoutMatches(kSceneSize - 1, kContextSize, kSceneFp, kContextFp, kAbi));
		CHECK(!World::RuntimeLayoutMatches(kSceneSize, kContextSize + 1, kSceneFp, kContextFp, kAbi));

		// 4. **等大小但指纹不同**必须被拒绝 —— 这是本次扩展的核心新能力。
		//    合成两个假指纹:尺寸一字不改,只让成员偏移不同(等价于"重排了成员")。
		CHECK(kSceneFp != 0);
		CHECK(kContextFp != 0);
		CHECK(!World::RuntimeLayoutMatches(kSceneSize, kContextSize, kSceneFp + 1, kContextFp, kAbi));
		CHECK(!World::RuntimeLayoutMatches(kSceneSize, kContextSize, kSceneFp, kContextFp + 1, kAbi));
		CHECK(!World::RuntimeLayoutMatches(kSceneSize, kContextSize, kSceneFp ^ 0x1ull, kContextFp, kAbi));

		// 5. 两个指纹同时错、模块 ABI 不一致,都必须被拒绝(与 Game.dll 的等值门同口径)。
		CHECK(!World::RuntimeLayoutMatches(kSceneSize, kContextSize, kSceneFp + 1, kContextFp + 1, kAbi));
		CHECK(!World::RuntimeLayoutMatches(kSceneSize, kContextSize, kSceneFp, kContextFp, kAbi + 1));

		// 6. 指纹必须真的与成员偏移挂钩:它在同一进程内稳定(两次调用相等),
		//    且不是简单的常量(大于 kOffsetBasis 说明真的混过内容)。
		CHECK(World::Scene::LayoutFingerprint() == World::Scene::LayoutFingerprint());
		CHECK(World::WorldContext::LayoutFingerprint() == World::WorldContext::LayoutFingerprint());
		CHECK(kSceneFp != World::LayoutHash::kOffsetBasis);
		CHECK(kSceneFp != kContextFp);

		// 7. 真实构造一对 context/scene 之后仍然一致,并把**崩溃当时读到垃圾的两个值**钉住:
		//    没进运行时 ⇒ IsPhysics3DRunning() 必须是 false、PhysicsDebug 必须是 false
		//    (崩溃实测:这两个都因偏移错位读成 true,于是拿垃圾指针去 CollectDebugLines)。
		World::WorldContext context;
		World::Scene scene(context);
		CHECK(WE_RUNTIME_LAYOUT_MATCHES());
		CHECK(scene.GetWorldSettings().PhysicsDebug == false);
		CHECK(!scene.IsPhysics3DRunning());
		CHECK(scene.GetPhysics3DWorld() == nullptr);

		std::printf("[World.RuntimeContract] sizes: scene=%llu context=%llu | fingerprint: scene=%llu context=%llu\n",
			static_cast<unsigned long long>(kSceneSize), static_cast<unsigned long long>(kContextSize),
			static_cast<unsigned long long>(kSceneFp), static_cast<unsigned long long>(kContextFp));
		std::puts("[World.RuntimeContract] All runtime contract assertions passed.");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "[World.RuntimeContract] Test failed: %s\n", e.what());
		return 1;
	}
}

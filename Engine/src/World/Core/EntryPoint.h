#pragma once
#ifdef WLD_PLATFORM_WINDOWS
extern World::Application* World::CreateApplication(World::WorldContext& context);

#include "World/Core/RuntimeContract.h"

#include <cstdio>

int main(int argc, char** argv)
{
	World::Log::Init();

	// 宿主(Editor / Runtime)与 WorldRuntime.dll 的布局契约:世代不一致时**干净退出**,
	// 不要带着错误偏移继续跑(实测会崩在 DLL 内部的物理调试绘制里,栈上看不到真因)。
	// 契约与覆盖边界见 RuntimeContract.h。
	if (!WE_RUNTIME_LAYOUT_MATCHES())
	{
		std::fprintf(stderr,
			"[WorldEngine] 宿主程序与 WorldRuntime.dll 不是同一次构建出来的(Engine/Scene 布局不一致)。\n"
			"[WorldEngine] 请重新构建整个引擎(例如 cmake --build build/x64-Debug --config Debug),\n"
			"[WorldEngine] 不要只重建 Editor/Runtime —— 增量构建漏掉的头文件改动会让两侧成员偏移错位。\n");
		WLD_CORE_CRITICAL("[runtime-contract] host/runtime layout mismatch; rebuild the whole engine");
		return 3;
	}

	World::WorldContext context;

	// 插桩不再有"会话"概念:进程生命周期由 Telemetry::Init/Shutdown 管,
	// trace 只在**按需采集**窗口内产出(环境变量或 AI 命令驱动,见 docs/dev/profiling.md)。
	auto app = World::CreateApplication(context);
	app->Run();
	delete app;


}
#endif

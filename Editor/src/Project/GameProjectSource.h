#pragma once

#include <filesystem>
#include <string>

namespace World::Editor
{
	// PURE-ECS:「新建 C++ 系统」的登记落盘。
	//
	// 纯 ECS 里项目的系统要在 `<项目根>/src/GameProject.cpp` 里做三件**互相配套**的事:
	//
	//     #include "Systems/MySystem.h"                                    // 顶部:系统定义要可见
	//     void AttachProjectSystems(Scene& scene) { scene.RegisterSystem<MySystem>(); }
	//     void DetachProjectSystems(Scene& scene) { scene.UnregisterSystem<MySystem>(); }
	//
	// Attach/Detach 两处的**字符串必须逐字配对**(Detach 靠名字找系统),漏一个 Detach 会留下跨运行
	// 存活的帧系统对象,而运行期没有任何校验能发现它 —— 手工维护是已知的坑,所以由编辑器代劳。
	// include 也不能少:`<项目根>/src/Systems/*.h` 是 header-only,只有被某个 .cpp include 才会进
	// Game.dll 的编译单元;缺了它 `scene.RegisterSystem<MySystem>()` 里的类型不可见,项目编不过。
	//
	// 实现刻意保持**纯文本、零解析依赖**(不引 clang/语法树):在顶部 `#include "GameAPI.h"` 之后
	// 插一行 systems include,再在两个函数体各自最后一个 `}` 之前各插一行。判定与插入**只认真代码**
	// —— 注释与字符串字面量里的同名字样不算已登记(模板 GameProject.cpp 的示例注释就是这种坑);
	// 认不出结构(函数被改名/挪走、找不到 include 锚点、花括号不配对)时**一个字节都不写**并回报
	// 可读原因 —— 不猜格式,也不做"尽量改改看"。
	struct GameProjectSystemRegistration
	{
		bool Ok = false;                 // 整个操作成功(含"本来就已登记"的幂等路径)
		bool FileCreated = false;        // GameProject.cpp 本来不存在,按骨架创建
		bool IncludeAdded = false;       // 本次真的插了 #include "Systems/<Name>.h"
		bool AttachAdded = false;        // 本次真的插了 Attach 行
		bool DetachAdded = false;        // 本次真的插了 Detach 行
		bool AlreadyRegistered = false;  // Attach/Detach 两处都已登记(幂等:没有写文件)
		std::string Error;               // Ok=false 时的可读原因
		std::string Path;                // 目标文件(Ok 与否都填,便于报错指向)
	};

	// 幂等:Attach/Detach 都已登记 ⇒ Ok + AlreadyRegistered(不写文件)。只缺其中一处时只补那一行
	// (可修复"手工登记漏掉 Detach"的既有文件)。全新登记(两处都没有)时,若顶部还缺
	// `#include "Systems/<Name>.h"` 会一并补上;两处已有其一时视为"系统定义本来就可见"(include
	// 写在别的头里/别的写法),不再补 include,避免把同名类重复引入。
	// systemName 必须是合法 C++ 标识符;非法一律 Error(调用方应先在 UI 里拦下,这里是第二道)。
	GameProjectSystemRegistration RegisterProjectSystem(const std::filesystem::path& projectRoot,
		const std::string& systemName);

	// `<项目根>/src/Systems/<Name>.h` 的模板正文(ISystem + Query:Each 范例 + 挂载说明)。
	std::string GameProjectSystemHeaderSource(const std::string& systemName);

	// `GameProject.cpp` 缺失时用的骨架(与 `templates/project-empty/src/GameProject.cpp` 逐字节同源,
	// 含 `#include "GameAPI.h"` 头部与两个空函数体)。
	std::string GameProjectSkeletonSource();

	// 合法 C++ 标识符:[A-Za-z_][A-Za-z0-9_]*(与编辑器"新建 C++ …"向导同一口径)。
	bool IsValidSystemIdentifier(const std::string& name);
}

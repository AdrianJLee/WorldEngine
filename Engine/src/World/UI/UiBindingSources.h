#pragma once

// 游戏 UI 框架(GameUI)— 带 ECS / 会话服务依赖的绑定解析器(工作包 M26)。
//
// 为什么独立成本文件:`UiBinding` 核心层是**无依赖层**(不 include entt / Scene / 脚本运行时,
// 见 `UiBinding.h` 头注释)。`ecs:` / `service:` 的求值需要反射与场景/会话,所以解析器住在
// 这里,由宿主在初始化时注册进 `UiBinding::RegisterResolver`(幂等;同名重复注册 = 覆盖)。
//
// 数据源口径:`UiBindingTable::Refresh(UiBindingDataSource{ Version, Context })` 的 `Context`
// 指向一个 `UiBindingContext`;核心层只当它是 `void*` 透传,解析器按约定解释它。
// `Version` 由调用方给(引擎两个宿主都用 `Scene::CurrentWorldTick()`),版本不变 = 不重复求值。
//
// 边界(硬):**只读** —— 解析器不写 ECS 结构、不改 `.wui` 文档、不发命令;失败一律
// false + 可读 error(调用方 `UiBindingTable::Refresh` 记一条 warning,不影响绘制)。

#include "World/Core/Export.h"

namespace World
{
	class Scene;
}

namespace World::Gameplay
{
	class GameApp;
}

namespace World::UI
{
	// 绑定运行时上下文(`UiBindingDataSource::Context` 指向它)。
	struct UiBindingContext
	{
		const World::Scene* Scene = nullptr;       // `ecs:` 解析器的数据源(空 = 无场景)
		Gameplay::GameApp* App = nullptr;          // `service:` 解析器的数据源(空 = 无会话)
	};

	// 幂等注册 `ecs:` 解析器(运行时数据源 = `UiBindingContext::Scene`):
	//   `ecs:<Entity>/<Component>/<Field>` —— Entity = 实体 Tag 名,或十进制实体句柄
	//   `ecs:<Component>/<Field>`          —— 查询**首个**带该组件的实体
	// 组件 / 字段解析走 `Schema::SchemaRegistry` 与 `FieldSchema::Get`(唯一反射入口,不按名手写分支);
	// 实体不存在 / 组件缺失 / 字段缺失 / 无上下文 ⇒ false + 可读 error。
	WLD_API void RegisterEcsBindingResolver();

	// 幂等注册 `service:` 只读查询解析器。`app` = 会话数据源(注册期登记;解析时优先用
	// `UiBindingContext::App`,为空才回退到它)。支持的查询见 `UiBindingSources.cpp` 顶部注释。
	WLD_API void RegisterServiceBindingResolver(World::Gameplay::GameApp& app);

	// 便捷入口:一次注册 ecs(以及会话可用时的 service)。宿主 `Initialize` 时调一次即可(幂等)。
	WLD_API void RegisterBuiltinBindingSources(World::Gameplay::GameApp* app = nullptr);
}

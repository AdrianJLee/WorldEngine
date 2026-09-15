#pragma once

#include "World/Core/Export.h"
#include "World/Core/Timestep.h"
#include "World/Gameplay/EventBus.h"
// 行为接口按值接收 Entity,默认实现体内联在本头文件 —— 必须提供完整定义,
// 否则任何"先包含 Behavior.h 再包含 Entity.h"的使用方都会编译失败(实测踩过)。
#include "World/Scene/Entity.h"

#include <cstdint>
#include <string>

namespace World::Gameplay
{
	// 行为描述:模块归属 + 稳定 id + schema 版本。热重载/存档迁移都按 StableId + 字段 id 定位。
	struct BehaviorDesc
	{
		std::string ModuleId;      // 提供该行为的模块(游戏 DLL / 插件)
		std::string StableId;      // 稳定标识(不要用 C++ 类型名,便于重命名与迁移)
		uint32_t SchemaVersion = 1;
	};

	// 语言无关的行为接口(P2a W5):C++ 与 Luau 行为都实现同一组生命周期。
	// 契约:
	//  - OnCreate/OnDestroy 只在实体脚本组件启停时调用;
	//  - OnUpdate 走可变步长,OnFixedUpdate 走固定步长(权威模拟);
	//  - OnEvent 接收类型化事件(载荷尺寸不匹配由调用方过滤);
	//  - CaptureState/RestoreState 供存档与热重载迁移(按字段 id 读写,由宿主实现桥接)。
	class WLD_API IBehavior
	{
	public:
		// 注意:实现放在 Behavior.cpp(而不是内联)——IBehavior 标了 WLD_API,
		// 导出的类若把成员只写在头里,DLL 消费者(如 Game.dll)会缺符号而链接失败(实测)。
		IBehavior();
		virtual ~IBehavior();

		virtual const BehaviorDesc& GetDesc() const = 0;
		virtual void OnCreate(Entity self);
		virtual void OnUpdate(Entity self, Timestep dt);
		virtual void OnFixedUpdate(Entity self, Timestep dt);
		virtual void OnEvent(Entity self, const EventValue& event);
		virtual void OnDestroy(Entity self);
	};
}

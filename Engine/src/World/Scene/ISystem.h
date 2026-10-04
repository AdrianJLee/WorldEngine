#pragma once

#include "World/Core/Export.h"
#include "World/Core/Timestep.h"
#include "World/Gameplay/Runtime/SystemRegistry.h"
#include <entt.hpp>
#include <string_view>
#include <string>
#include <vector>

namespace World
{
	class Scene;

	// 相位内并行判定的依据:系统声明"我会读/写哪些组件类型"。
	// 调过 Read/Write 才算声明过(Declared = true);未声明 ⇒ 调度器保守串行。
	class SystemAccess
	{
	public:
		void Read(entt::id_type component)  { Reads.push_back(component); Declared = true; }
		void Write(entt::id_type component) { Writes.push_back(component); Declared = true; }
		bool Declared = false;
		std::vector<entt::id_type> Reads;
		std::vector<entt::id_type> Writes;
	};

	enum class SystemKind : uint8_t
	{
		Pipeline = 0,   // 常规每帧/每固定步推进
		Interval,       // 定时间隔节流推进
		Startup,        // 场景开局执行一次
		Teardown,       // 场景结束执行一次
	};

	class WLD_API ISystem
	{
	public:
		virtual ~ISystem() = default;
		virtual std::string_view Name() const = 0;
		virtual Gameplay::SystemPhase Phase() const { return Gameplay::SystemPhase::Update; }
		virtual SystemKind Kind() const { return Interval() > 0.0f ? SystemKind::Interval : SystemKind::Pipeline; }
		virtual float Interval() const { return 0.0f; }
		// 并行**权限**:返回 true 表示"本系统可以离线线程执行"(不碰仅主线程可用的资源,
		// 如 Luau VM / 平台句柄)。它与 DeclareAccess 的读写集是**两个独立前提**,缺一不可:
		// 只声明读写集而 ParallelSafe=false 的系统仍会被强制串行。
		virtual bool ParallelSafe() const { return false; }
		// 声明本系统的读写集。默认不声明(空实现),调度器据此保守串行。
		virtual void DeclareAccess(SystemAccess& access) { (void)access; }
		// 便捷判据:等价于声明里是否调过 Read/Write(见 SystemAccess::Declared)。
		virtual bool AccessDeclared() const { return false; }
		virtual std::vector<std::string> After() const { return {}; }
		virtual bool ShouldRun(const Scene& scene) const { (void)scene; return true; }
		virtual void Update(Scene& scene, Timestep dt) = 0;
	};
}

#pragma once

#include "World/Core/Export.h"

#include <entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace World
{
	class Scene;

	// P1b D6:3D 物理(Jolt)的世界空间调试线段(供编辑器视口画线框 / headless 断言)。
	struct DebugLine
	{
		glm::vec3 Begin { 0.0f };
		glm::vec3 End { 0.0f };
	};

	// P1b D6:3D 物理(Jolt)引擎侧封装。
	//
	// 边界与约定:
	//  - pimpl:Jolt 头文件**只**出现在 Physics3D.cpp;本头与 Scene.h 都不 include Jolt;
	//  - 单线程 job system(不引引擎 JobSystem 依赖);所有入口必须在场景 owner 线程调用;
	//  - 世界只在 Start(Scene&) → Stop() 之间存在;未启动时 Step/SyncTransforms/CollectDebugLines
	//    抛可读 std::logic_error(与 2D 物理"不静默"约定一致);
	//  - 实体句柄 ↔ Jolt 刚体的映射留在本模块内部(组件里没有 Jolt 运行态字段);
	//  - 位姿权威:Static 由 TransformComponent 决定;Kinematic 每步前跟随 TransformComponent;
	//    Dynamic/Kinematic 每步后写回 TransformComponent(只在位姿变化时写)。
	class WLD_API Physics3DWorld
	{
	public:
		// 重力加速度(与 2D 世界同一口径:"符合 g"的自由落体断言用这个常量,不散落魔数)。
		static constexpr float kGravity = -9.81f;

		Physics3DWorld();
		~Physics3DWorld();
		Physics3DWorld(const Physics3DWorld&) = delete;
		Physics3DWorld& operator=(const Physics3DWorld&) = delete;

		// 校验场景里不存在会触发 Jolt 断言 / 语义冲突的组合:
		//  - 同一实体同时挂 2D 与 3D 物理组件(RigidBody2D/BoxCollider2D/CircleCollider2D
		//    vs RigidBody3D/BoxCollider3D/SphereCollider3D/CapsuleCollider3D/MeshCollider3D);
		//  - MeshCollider3D 的 StaticTriangles 挂在非 Static 刚体上;
		//  - 3D 碰撞体尺寸非正 / 非有限。
		// 失败返回 false + 可读 error,**不改动任何数据**。Scene::OnRuntimeStart 在创建任何物理世界前
		// 先调它(拒绝启动);Start 也调它(直接 API 调用同样拒绝)。
		static bool ValidateScene(const Scene& scene, std::string* error);

		// 按场景组件建世界与刚体。已启动 / 校验失败 / 形状或资产加载失败 → 抛可读
		// std::logic_error,不留下半启动状态。
		void Start(Scene& scene);
		// 销毁全部刚体与世界;可重复调用。
		void Stop();
		bool IsStarted() const { return m_Impl != nullptr; }

		// 固定步长推进:Step 前把 Kinematic 刚体同步到 TransformComponent(Kinematic 跟随 Transform),
		// 之后由调用方(Scene::OnUpdatePhysics3D)调 SyncTransforms 写回。
		// fixedDeltaSeconds <= 0 时是 no-op(与 2D 步进同一条固定步路径)。
		void Step(float fixedDeltaSeconds);
		// 把 Dynamic/Kinematic 刚体的位姿写回 TransformComponent,只在位姿变化时写
		// (避免每帧标脏/打断层级);Static 不写。
		void SyncTransforms();

		// 接触事件:added=true → OnContactAdded,added=false → OnContactRemoved;
		// 参数是实体句柄(Jolt 的 body1/body2 顺序,body1 ID 更小)。可在 Start 前设置。
		void SetContactCallback(std::function<void(bool added, entt::entity entityA, entt::entity entityB)> callback);

		// 调试绘制(世界空间线段):box = 12 棱;sphere = 3 圆;capsule = 2 圆 + 4 条侧线;
		// MeshCollider3D 不产线(凸包/三角网格线框不在 D6 范围)。outLines 会被清空。
		void CollectDebugLines(std::vector<DebugLine>& outLines) const;

		// 运行时维护:删除某实体的刚体(销毁实体 / 移除 RigidBody3DComponent 时由 Scene 调用)。
		// 没有该实体的刚体时是 no-op;世界未启动时也是 no-op。
		void DestroyBody(entt::entity entity);

		// 测试 / 调试入口:取实体对应刚体的 Jolt 权威世界位姿;没有刚体返回 false。
		bool TryGetBodyTransform(entt::entity entity, glm::vec3* outLocation, glm::quat* outRotation) const;

	private:
		struct Impl;

		void RequireStarted(const char* operation) const;
		void EmitContact(bool added, entt::entity entityA, entt::entity entityB) const;

		std::unique_ptr<Impl> m_Impl;
		std::function<void(bool added, entt::entity entityA, entt::entity entityB)> m_ContactCallback;
	};
}

#pragma once

#include "World/Core/Export.h"
#include "World/Physics/PhysicsEvents.h"

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
	//  - pimpl:Jolt 头文件**只**出现在 Physics3D.cpp 与 PhysicsJobSystem.{h,cpp};
	//    本头与 Scene.h 都不 include Jolt;
	//  - P5 事件/过滤:接触/触发事件按固定步产出、Step 返回后按序 flush(不在 Jolt 回调里直接入队);
	//    PhysicsEvents.h(不含 Jolt)是本模块唯一新增的物理头;per-body (Layer,Mask) 过滤在 .cpp 内
	//    映射成 Jolt ObjectLayer;传感器是刚体级开关。
	//  - job system:默认单线程(逐位可复现);physics.multithreaded = true 且引擎 JobSystem
	//    在跑时才接 PhysicsJobSystem(要吞吐,放弃逐位可复现);所有入口必须在场景 owner 线程调用;
	//  - 世界只在 Start(Scene&) → Stop() 之间存在;未启动时 Step/SyncTransforms/CollectDebugLines
	//    抛可读 std::logic_error(与 2D 物理"不静默"约定一致);
	//  - 实体句柄 ↔ Jolt 刚体的映射留在本模块内部(组件里没有 Jolt 运行态字段);
	//  - P7 约束:关节的 Jolt 句柄与"被禁用碰撞"的配对集合也留在本模块内部;DestroyJoints /
	//    DestroyAllJoints 负责释放(不能把悬垂 Constraint* 留给 Stop);
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
		// JOBSYS:当前选路是否走 Jolt 多线程适配器(= physics.multithreaded 且引擎 JobSystem
		// 可用)。默认 false;测试用它证明 MT 分支真的被选到(不是死代码)。
		bool IsUsingMultithreadedJobSystem() const;

		// ---- P5:物理事实回调(Step 内部按序 flush;不在 Jolt 接触回调里直接调用) ----
		// 实体接触事件(非传感器)。参数已解析成实体句柄:
		//   EntityA/EntityB = Jolt 的 body1/body2(该配对在 Jolt 侧保证 body1 ID < body2 ID),
		//   Normal 指向 A → B(与 Jolt ContactManifold::mWorldSpaceNormal 同向),
		//   Point = 第一个接触点的世界坐标,PenetrationDepth = 本流形最大值;
		//   End 阶段(OnContactRemoved)拿不到流形 ⇒ Point/Normal 为零、PenetrationDepth = 0。
		// 可在 Start 前设置;回调抛出的异常被捕获并记日志(不会破坏 Step)。
		void SetContactCallback(std::function<void(const Physics::ContactEvent&)> callback);
		// 传感器/触发器事件(刚体 RigidBody3DComponent::IsSensor)。Jolt 对 sensor 同样回调接触监听器,
		// 这里按 IsSensor 分流;两个刚体都是 sensor 时 SensorEntity 取 body1(顺序确定)。可在 Start 前设置。
		void SetTriggerCallback(std::function<void(const Physics::TriggerEvent&)> callback);

		// 调试绘制(世界空间线段):box = 12 棱;sphere = 3 圆;capsule = 2 圆 + 4 条侧线;
		// MeshCollider3D 不产线(凸包/三角网格线框不在 D6 范围)。outLines 会被清空。
		void CollectDebugLines(std::vector<DebugLine>& outLines) const;

		// 运行时维护:删除某实体的刚体(销毁实体 / 移除 RigidBody3DComponent 时由 Scene 调用)。
		// 没有该实体的刚体时是 no-op;世界未启动时也是 no-op。
		void DestroyBody(entt::entity entity);

		// ---- P7:关节/约束(Jolt 2 体约束) ----
		// 建约束:owner/other 都必须是本世界里**已建好的**刚体,否则抛可读 std::logic_error
		// (不静默)。kind:0 = Fixed / 1 = Distance / 2 = Hinge(与 JointComponent::JointKind 数值对齐)。
		// anchorSelf/anchorOther 是两实体**局部空间**的连接点,axis 是 Hinge 轴(主实体局部空间);
		// 内部用刚体当前世界位姿换算到世界系(Jolt EConstraintSpace::WorldSpace)。
		// minDistance/maxDistance < 0 ⇒ 原样交给 Jolt(它用两锚点实际距离);仅 Distance 使用。
		// enableCollision == false(默认)时两体间的碰撞被禁用 —— Jolt 没有每约束的 collide-connected
		// 开关,内部用 GroupFilterTable 实现(见 .cpp)。
		void CreateJoint(entt::entity owner, entt::entity other, int kind,
			const glm::vec3& anchorSelf, const glm::vec3& anchorOther, const glm::vec3& axis,
			float minDistance, float maxDistance, bool enableCollision);
		// 销毁某实体参与的全部约束(销毁实体 / 移除 JointComponent 时由 Scene 调用);无则 no-op。
		void DestroyJoints(entt::entity entity);
		// 销毁全部约束(Stop() 里随世界一起);可重复调用。
		void DestroyAllJoints();

		// 测试 / 调试入口:取实体对应刚体的 Jolt 权威世界位姿;没有刚体返回 false。
		bool TryGetBodyTransform(entt::entity entity, glm::vec3* outLocation, glm::quat* outRotation) const;
		// P7 测试入口:`RigidBody3DComponent::Ccd` 是否真的落到 Jolt 的 LinearCast 档位
		// (读回后端,而不是只看组件字段)。没有该实体的刚体返回 false。
		bool GetBodyMotionQualityIsLinearCast(entt::entity entity) const;

	private:
		struct Impl;

		void RequireStarted(const char* operation) const;
		void EmitContactEvent(const Physics::ContactEvent& event) const;
		void EmitTriggerEvent(const Physics::TriggerEvent& event) const;

		std::unique_ptr<Impl> m_Impl;
		std::function<void(const Physics::ContactEvent&)> m_ContactCallback;
		std::function<void(const Physics::TriggerEvent&)> m_TriggerCallback;
	};
}

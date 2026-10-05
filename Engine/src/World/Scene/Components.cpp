#include "wldpch.h"
#include "World/Scene/Components.h"
#include "World/Scene/ComponentLayoutBudget.h"

#include <type_traits>

namespace World
{
	// WP3(PECS 1.1):2D physics runtime handles moved into the Scene-internal tables, so these
	// components are pure data again. Locked here so a future field can not silently
	// reintroduce a non-trivial or runtime-only member.
	static_assert(std::is_trivially_copyable_v<RigidBody2DComponent>,
		"RigidBody2DComponent must stay trivially copyable (no runtime handles)");
	static_assert(std::is_trivially_copyable_v<JointComponent>,
		"JointComponent must stay trivially copyable (no runtime handles)");
	static_assert(sizeof(RigidBody2DComponent) <= 64, "RigidBody2DComponent must stay within one cache line");
	static_assert(sizeof(JointComponent) <= 64, "JointComponent must stay within one cache line");
	static_assert(std::is_trivially_copyable_v<HierarchyComponent>,
		"HierarchyComponent must stay trivially copyable (the child list lives in HierarchyChildrenComponent)");
	static_assert(sizeof(HierarchyComponent) == 8, "HierarchyComponent must stay Parent + InheritTransform");

	// 相机数据导向化(2026-10-04):组件只留权威参数,投影矩阵是派生量(CameraViewComponent 缓存)。
	// 锁死 64B(一条 cache line)并禁止派生成员回流到组件。
	static_assert(std::is_trivially_copyable_v<CameraSettings>, "CameraSettings must stay trivially copyable (pure parameters)");
	static_assert(std::is_trivially_copyable_v<CameraComponent>, "CameraComponent must stay trivially copyable (no derived cache)");
	static_assert(sizeof(CameraComponent) <= 64, "CameraComponent must stay within one cache line");

	// PECS 1.2(2026-10-04):资产引用 = 驻留 PathId(4B POD),不再是 std::string。
	// 这一层锁死两件事:(1) PathId 本身必须是 4B 平凡类型;(2) 只含资产引用 + POD 的组件
	// 必须回到平凡可拷贝 —— 否则"复制实体 / 实例化 Prefab / 场景克隆"又会走堆深拷贝。
	static_assert(sizeof(PathId) == 4, "PathId must stay a 4-byte POD handle");
	static_assert(std::is_trivially_copyable_v<PathId>, "PathId must stay trivially copyable");
	static_assert(std::is_trivially_copyable_v<MeshCollider3DComponent>,
		"MeshCollider3DComponent must stay trivially copyable (ColliderMode + PathId only)");
	// MeshRendererComponent 现在**没有任何字符串**:Primitive 已枚举化,资产引用是 PathId ⇒ 平凡可拷贝。
	// SkinnedMeshRendererComponent 还留一个 AnimationClip 名字字符串(T6b 走驻留 NameId)。
	// 棘轮:上限 = 一个持有型字符串(AnimationClip)+ 64B POD 尾巴(两个 AssetRef 各 16B +
	// MeshIndex/Playing/Speed/Loop/Time)。改回字符串资产引用或往组件里塞堆对象都会编译失败。
	static_assert(std::is_trivially_copyable_v<MeshRendererComponent>,
		"MeshRendererComponent must stay trivially copyable (no heap members)");
	static_assert(sizeof(MeshRendererComponent) <= 64,
		"MeshRendererComponent must fit one cache line");


	// ---- F8 空洞审计的棘轮(标准 §4.8 R2)----
	// 2026-10-05 用一次性探针审计了全部 22 个 schema 组件的**内部空洞**(实测偏移,非推算):
	//   * 只有这两个组件的空洞会**真正缩小 stride**(其余组件的空洞被尾部对齐填充吸收,去掉 sizeof 不变);
	//   * 两者都按 R2 重排(8B 对齐的 AssetRef 先排、4B 标量其次、1B bool 最后)落到 56B。
	// 这两条断言把"无内部空洞"钉住:若有人插回 4B/1B 字段打断 8B 边界,sizeof 会回到 60/64 ⇒ 编译失败。
	static_assert(sizeof(MeshRendererComponent) == 56,
		"MeshRendererComponent must be 56B (hole-free per 4.8 R2); 60/64 here means an interior hole came back");
	static_assert(sizeof(SkinnedMeshRendererComponent) == 56,
		"SkinnedMeshRendererComponent must be 56B (hole-free per 4.8 R2); 60/64 here means an interior hole came back");
	// T6b(2026-10-04):名字字段(NameId)也是驻留 POD ⇒ TagComponent 从此平凡可拷贝(4 字节)。
	// 这是组件契约的最后一块:此前 Tag 的 std::string 让它无法参与 memcpy 级的复制/Prefab 实例化。
	static_assert(sizeof(TagComponent) == 4, "TagComponent must be exactly one NameId (4 bytes)");
	static_assert(std::is_trivially_copyable_v<TagComponent>, "TagComponent must stay trivially copyable");
	static_assert(std::is_trivially_copyable_v<SkinnedMeshRendererComponent>,
		"SkinnedMeshRendererComponent must stay trivially copyable (no heap members)");
	static_assert(sizeof(SkinnedMeshRendererComponent) <= 64,
		"SkinnedMeshRendererComponent must fit one cache line (two AssetRef + one NameId + scalars)");
}

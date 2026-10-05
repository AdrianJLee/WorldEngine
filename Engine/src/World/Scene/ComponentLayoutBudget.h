#pragma once

// 数据布局预算与豁免登记(标准的机器可读部分)。
//
// 规则正文:docs/dev/performance-and-data-layout.md(公开) +
//           WE_DOCS/knowledge/contracts/data-layout.md(知识层合同)。
//
// 判据来源:标准 §4.1 判定四问 + §4.2 类型四分类。**任何组件**的布局变化都会经过这里:
//   * 编译期:`ComponentSchemaBridge.h` 的 MakeComponentStorage<T>() 对每个 schema 组件
//     施加"平凡可拷贝 + ≤ kComponentCacheLineBytes"断言 ⇒ 违反即编译失败;
//   * 运行期:TESTS(World.ComponentLayout)枚举 schema 注册表,核对每个组件的 Size 与豁免一致性。
//
// 豁免不是宽容度,而是**白名单**:必须写下理由与复核日期,过期未复核由门禁告警。
// 新增豁免 = 一次有意识的架构决定,不是"为了让门禁变绿"。

#include "World/Scene/Components.h"

#include <cstddef>
#include <type_traits>

namespace World
{
	// 组件尺寸预算:单条 cache line(标准 §4.4)。
	constexpr std::size_t kComponentCacheLineBytes = 64;

	// 豁免登记(T 为组件类型)。默认不豁免 —— 未登记且超预算 = 编译失败。
	template <typename T>
	struct ComponentLayoutExempt : std::false_type {};

	// ---- 已登记豁免(每条 = 组件名 + 理由 + 复核日期)----
	//
	// 1) CameraViewComponent:112B。相机数量是 O(1)(每场景 1–2 台,非热路径),
	//    内含 64B 投影矩阵缓存 + 32B 指纹参数;为"减少 48B"拆分热/冷两个组件
	//    买不到可测收益(判定四问 Q1 = No)。复核:2027-01-05。
	template <>
	struct ComponentLayoutExempt<CameraViewComponent> : std::true_type {};

	// 2) PhysicsInterpolationState:68B(64B 上一帧局部矩阵 + Valid)。
	//    物理体数量可能 ≥1000,Q1 未定 ⇒ 登记豁免并列为优化候选(O2:改存分解后的
	//    Location/RotationQuat/Scale = 44B 可回到单条 cache line,但需实测帧时间占比)。
	//    复核:2026-11-05(与基准结论一起裁决)。
	template <>
	struct ComponentLayoutExempt<PhysicsInterpolationState> : std::true_type {};

	// 3) HierarchyChildrenComponent:内嵌 std::vector(**有意非平凡**)。
	//    它是运行期子列表缓存,不入 .wd、不进属性面板、不参与 schema 复制与 Prefab 实例化
	//    (见 Components.h 注释)。尺寸预算对它不适用,故仅在尺寸断言中豁免;
	//    "必须平凡可拷贝"只约束 schema 组件,不约束它。
	template <>
	struct ComponentLayoutExempt<HierarchyChildrenComponent> : std::true_type {};

	// ---- 非 schema 组件的编译期尺寸棘轮(标准 §4.2 C1)----
	// 这些组件不进 schema 注册表,所以 MakeComponentStorage 的断言覆盖不到它们;在这里钉住。
	static_assert(sizeof(WorldTransformComponent) == 64, "WorldTransformComponent must stay exactly one cache line");
	static_assert(std::is_trivially_copyable_v<WorldTransformComponent>, "WorldTransformComponent must stay trivially copyable");

	// 豁免项仍钉住"当前实测值",防止悄悄膨胀(超预算可以,但必须是同一个已知数字)。
	static_assert(sizeof(CameraViewComponent) <= 128, "CameraViewComponent grew past its registered exemption (112B); re-review the budget");
	static_assert(sizeof(PhysicsInterpolationState) <= 72, "PhysicsInterpolationState grew past its registered exemption (68B); re-review O2");
}

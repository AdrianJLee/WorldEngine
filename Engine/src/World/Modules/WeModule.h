#pragma once

#include <cstdint>

namespace World
{
	class WorldContext;
}

namespace World::Modules
{
	// P0 模块契约(同构建工具链下的内部 C++ 契约;版本化 C ABI 在 P5)。
	// 字段只增不改号;宿主与模块双方都校验 struct_size 与 abi_version。
	// 2 = 2026-09-26 脚本组件重写:`Schema::ScriptBinding` 从 `{ void(*Bind)(void*) }` 变成
	//     `{ Create, Destroy }` + 组件类型改名 ⇒ 旧 Game.dll 必须被**干净拒绝**,不能按旧形状解释
	//     (旧 DLL 的 Bind 会被当 Create 调用)。宿主侧改成等值校验(见 ModuleManager.cpp)。
	// B 期(2026-09-26):`PropertyNode` 增加递归子字段(TypeName/Children/ReadOnly)⇒ 组件布局变化,
	// 旧 Game.dll 与新媒体混用会在属性表上读出错误布局 —— 升版让宿主干净拒绝旧模块。
	// C 期(2026-09-26 同一天):`PropertyNode` 再增加集合形态(Collection/ElementKind/KeyKind)⇒
	// 布局再次变化,旧 Game.dll 必须被干净拒绝(与 2→3 同一理由)。
	// D 期(2026-09-27):`PropertyNode` 又增加"声明默认值"(Default)与"集合形状归场景"
	// (ShapeFromScene)两个尾部字段 ⇒ 布局再次变化(旧 Game.dll 会按旧布局读属性表),
	// 同一条理由再升一版让宿主干净拒绝旧模块。
	// CPPT-2(2026-09-27,D-B 模块级热重载):`Schema::FieldMetadata` 尾部追加 Unit/Step
	// (编辑期提示)⇒ FieldSchema/TypeSchema 的布局再次变化;同时 Category==Script 的
	// "未声明 Default = 未设(monostate)"语义变化也会让旧 Game.dll 按零值默认值解释属性表。
	// 宿主与模块仍走 ModuleManager 的**等值**校验 ⇒ 旧 DLL 必须被干净拒绝(不按新布局解释)。
	// CPPT-6(2026-09-28,C++ 容器属性):`Schema::Value` 追加容器替代项(数组/映射)、
	// `Schema::FieldSchema` 尾部追加容器形状描述(Collection/ElementKind/KeyKind/…)
	// ⇒ 反射与边界值布局再次变化,旧 Game.dll(ABI 6)必须被干净拒绝(同一条等值门)。
	// PURE-ECS(2026-10-02):`TypeCategory::Script` 与 `Schema::ScriptBinding` 整体删除
	// (单实体脚本组件已不存在,Script 类型既挂不上实体也没有工厂消费者)⇒
	// `TypeCategory` 少一个枚举值、`TypeSchema` 少一个指针字段,布局再次变化,
	// 旧 Game.dll(ABI 7)必须被干净拒绝(同一条等值门)。
	constexpr uint32_t WE_MODULE_ABI_VERSION = 8;

	struct WeModule
	{
		uint32_t StructSize = sizeof(WeModule);
		uint32_t AbiVersion = WE_MODULE_ABI_VERSION;
		const char* Id = nullptr;
		const char* Name = nullptr;
		uint32_t Version = 0;
		bool (*Register)(WorldContext& context) = nullptr;
		void (*Unregister)(WorldContext& context) = nullptr;
	};
}

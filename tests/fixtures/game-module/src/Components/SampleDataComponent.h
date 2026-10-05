#pragma once

#include <type_traits>

#include "World/Core/InlineString.h"
#include "World/Scene/Components.h"

namespace World
{
	// 纯数据示例组件:只做反射声明,不写任何 UI/编辑代码,
	// 由 Editor 的 InspectorRegistry 自动生成控件编辑。
	//
	// 数据布局(标准 docs/dev/performance-and-data-layout.md §4.2 C1 / §4.8):
	// 字段全部是 POD ⇒ 组件**平凡可拷贝**(复制实体 / Prefab 实例化 = memcpy,无堆深拷贝),
	// 且 sizeof 恰好卡进**一条 64B cache line**。这两条不是风格,是门禁 A 的编译期断言
	// (MakeComponentStorage<T>:is_trivially_copyable + sizeof ≤ 64)。
	//
	// 成员顺序遵循 §4.8:
	//   * R1:游戏侧每帧要读的字段(Offset/Health/Speed/Count/Opacity)全部放在第一条线内偏移 0..23;
	//   * R4:编辑器侧才读的 DisplayName/Tags 排在热字段之后;
	//   * 冲突裁决 R1 > R2:`Enabled` 是 1 字节,排最后 —— 若插在中间会因 4 字节对齐产生
	//     3 字节内部空洞(61 → 64 变 65+),把总尺寸顶出门禁;
	//   * 尾部的 3 字节是**对齐填充**(结构体尺寸必须是 alignof 的整数倍),不是成员间空洞。
	struct SampleDataComponent
	{
		glm::vec2 Offset { 0.0f, 0.0f };

		float Health = 100.0f;

		float Speed = 1.0f;

		int32_t Count = 3;

		float Opacity = 1.0f;

		// Text = **有界内联文本**(InlineString,无堆、平凡可拷贝)。
		// 为什么不用 std::string:String 会让组件不再平凡可拷贝(32B + 堆指针 + 深拷贝),
		// 于是"复制实体/Prefab 实例化"要走堆,门禁 A 也会直接拒绝。
		// 有界文本用 Text kind;无界的用户文本(文本框、备注)才用 String。
		InlineText20 DisplayName { "Sample" };

		InlineText16 Tags;

		bool Enabled = true;

		WE_SCHEMA_BODY(Game, SampleDataComponent, Component)
			WE_SCHEMA_META(Category("Project"),
				Doc("Game-module sample component used to exercise schema-driven editing; its eight fields cover bool, integer, float, vector, string and bounded-text kinds."))
			WE_FIELD(Health, Float);
			WE_FIELD(Speed, Float, Range(0.0f, 10.0f));
			WE_FIELD(Enabled, Bool);
			WE_FIELD(Offset, Vec2);
			WE_FIELD(Count, Int32);
			WE_FIELD(Opacity, Float, Range(0.0f, 1.0f));
			WE_FIELD(DisplayName, Text, ReadOnly);
			WE_FIELD(Tags, Text);
		WE_SCHEMA_END
	};
}


// 本组件的布局棘轮(标准 §4.4):显式钉住尺寸与平凡性。
// 门禁 A 只覆盖**引擎 schema 组件**(它们经 MakeComponentStorage<T> 注册);这个示例组件在
// Game 模块里,所以在这里自己钉一遍 —— 加字段撑破 64B、或塞进字符串/容器,都会编译失败。
// 放类外:类内求值面对的是未完成类型。
static_assert(sizeof(World::SampleDataComponent) == 64,
	"SampleDataComponent must stay exactly one 64B cache line (see docs/dev/performance-and-data-layout.md §4.4)");
static_assert(std::is_trivially_copyable_v<World::SampleDataComponent>,
	"SampleDataComponent must stay trivially copyable: bounded text uses InlineString, never std::string");
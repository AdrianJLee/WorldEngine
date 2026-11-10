#pragma once

// PLUG-T2b:组件 schema 测试的**共享布局夹具**。
//
// 这个头被两侧各自编译一份(测试插件 DLL 与 World.Plugins 单测 exe):
//   * 插件侧:用 offsetof/sizeof 填字段描述的 Offset/Size(注册契约的事实源);
//   * 单测侧:按同一布局构造实例,再经宿主生成的 schema 访问器读/写 ——
//     于是可以逐字段断言"宿主访问器真的写在插件声明的偏移上",而不是只做自洽的往返。
//
// 约束:只含标准 C++ 类型(测试插件不链接 World,拿不到 glm/引擎头)。Vec3 用同布局的
// 三浮点结构表达(12 字节,与 glm::vec3 相同 —— 宿主按 Kind::Vec3 以 glm::vec3 解释)。

#include <cstddef>
#include <cstdint>

namespace WePluginComponentFixture
{
	// Kind = Vec3 的字段布局(12 字节,与 glm::vec3 同布局)。
	struct PodVec3
	{
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
	};
	static_assert(sizeof(PodVec3) == 12, "Vec3 fixture must match the glm::vec3 layout");

	// test.component.Health:多字段类型(布尔/整型/浮点/向量/无符号小整型)。
	struct HealthFixture
	{
		bool Enabled = false;
		int32_t Charges = 0;
		float Health = 0.0f;
		PodVec3 Offset {};
		uint8_t Tier = 0;
	};

	// test.component.Shield:最小类型(单字段)—— 验证"注销一个类型不影响同模块其余类型"。
	struct ShieldFixture
	{
		float Shield = 0.0f;
	};
}

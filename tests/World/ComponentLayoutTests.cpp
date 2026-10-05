// 数据布局门禁 A(标准 docs/dev/performance-and-data-layout.md §4.2 C1 / §4.4):
// 对**每一个**已注册 schema 组件,在运行期核对"装得进单条 cache line(64B)"。
//
// 为什么要有它(与编译期断言分工):
//   * 编译期:MakeComponentStorage<T>() 的 static_assert 拦"写代码时就违反"的情况
//     (含新组件、新字段);
//   * 运行期(本测试):用 schema 注册表**枚举**事实源,拦"忘了加断言"或
//     "生成物与声明不同步"的情况 —— 断言写在生成 TU 里,枚举写在测试里,两条独立通道
//     互相交叉验证。
//
// 附带产出:打印全部组件的布局快照表(name / size),是标准 §4.4 基线的可重跑证据。
// 快照表用"预期值"钉住 ⇒ 任何尺寸变化(即使仍在预算内)都必须显式更新本文件,
// 让布局变化永远是一次有意识的决定(标准 §4.1 Q4:可证伪)。
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/ComponentLayoutBudget.h"
#include "World/Schema/Schema.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace World;
	using namespace World::Schema;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	struct ExpectedLayout
	{
		const char* Name;
		std::size_t Size;
	};

	// 布局快照(2026-10-05 基线,22 个 schema 组件)。改动组件布局 ⇒ 必须同步更新这一行,
	// 并在提交信息/方案"实施记录"里说明理由(收窄字段 / 拆分 / 登记豁免)。
	const std::vector<ExpectedLayout> kExpectedLayouts = {
		{ "World::UUIDComponent",                 8 },
		{ "World::TagComponent",                  4 },
		{ "World::TransformComponent",           48 },
		{ "World::VelocityComponent",            24 },
		{ "World::SpriteComponent",              40 },
		{ "World::CircleRendererComponent",      24 },
		{ "World::MeshRendererComponent",        64 },
		{ "World::SkinnedMeshRendererComponent", 64 },
		{ "World::HierarchyComponent",            8 },
		{ "World::CameraComponent",              36 },
		{ "World::DirectionalLightComponent",    32 },
		{ "World::PointLightComponent",          20 },
		{ "World::AmbientLightComponent",        16 },
		{ "World::RigidBody2DComponent",         16 },
		{ "World::BoxCollider2DComponent",       32 },
		{ "World::CircleCollider2DComponent",    28 },
		{ "World::RigidBody3DComponent",         40 },
		{ "World::BoxCollider3DComponent",       24 },
		{ "World::SphereCollider3DComponent",    16 },
		{ "World::CapsuleCollider3DComponent",   20 },
		{ "World::JointComponent",               56 },
		{ "World::MeshCollider3DComponent",      24 },
	};

	// 非 schema 组件:不在注册表里,预算与豁免由 ComponentLayoutBudget.h 的编译期断言覆盖。
	// 这里只打印它们的实测布局,让"豁免项"在门禁输出里可见(而不是藏在注释里)。

	std::size_t ExpectedSizeOf(const std::string& name, bool* found)
	{
		for (const ExpectedLayout& entry : kExpectedLayouts)
		{
			if (name == entry.Name)
			{
				*found = true;
				return entry.Size;
			}
		}
		*found = false;
		return 0;
	}
}

int main()
{
	try
	{
		Log::Init();

		WorldContext context;
		const std::vector<const TypeSchema*> components = context.Schemas().List(TypeCategory::Component);
		CHECK(!components.empty());

		std::printf("[World.ComponentLayout] registered components: %zu\n", components.size());
		std::printf("  %-46s %6s  %s\n", "component (schema)", "size", "budget 64B");

		std::size_t violations = 0;
		std::size_t drift = 0;
		std::size_t missing = 0;
		for (const TypeSchema* schema : components)
		{
			CHECK(schema != nullptr);
			CHECK(schema->Size > 0);

			const bool overBudget = schema->Size > kComponentCacheLineBytes;
			std::printf("  %-46s %6zu  %s\n", schema->Id.Name.c_str(), schema->Size,
				overBudget ? "EXEMPT?" : "ok");
			if (overBudget)
				++violations;

			bool found = false;
			const std::size_t expected = ExpectedSizeOf(schema->Id.Name, &found);
			if (!found)
			{
				// 新增组件必须登记快照:否则"新组件"会绕过布局评审。
				std::printf("      ! no snapshot entry for this component\n");
				++missing;
			}
			else if (expected != schema->Size)
			{
				std::printf("      ! snapshot drift: expected %zu, got %zu\n", expected, schema->Size);
				++drift;
			}
		}

		// 非 schema 组件(豁免项):打印实测值,让豁免"可见"。
		// 预算断言在 ComponentLayoutBudget.h;这里只做交叉验证(数值必须与登记一致)。
		std::printf("  %-46s %6s  %s\n", "component (non-schema)", "size", "registered budget");
		const struct { const char* Name; std::size_t Size; const char* Budget; } nonSchema[] = {
			{ "World::WorldTransformComponent",    sizeof(WorldTransformComponent),    "== 64" },
			{ "World::CameraViewComponent",        sizeof(CameraViewComponent),        "<= 128 (exempt)" },
			{ "World::PhysicsInterpolationState",  sizeof(PhysicsInterpolationState),  "<= 72 (exempt; O2)" },
			{ "World::HierarchyChildrenComponent", sizeof(HierarchyChildrenComponent), "non-trivial by design" },
		};
		for (const auto& entry : nonSchema)
			std::printf("  %-46s %6zu  %s\n", entry.Name, entry.Size, entry.Budget);

		CHECK(sizeof(WorldTransformComponent) == 64);

		// 1. 预算:schema 组件里没有豁免项 —— 全部必须 ≤ 64B。
		CHECK(violations == 0);

		// 2. 快照覆盖与漂移:22 个注册组件必须逐个登记,且尺寸逐一相等。
		CHECK(missing == 0);
		CHECK(drift == 0);
		CHECK(components.size() == kExpectedLayouts.size());

		// 3. 交叉验证:预算头声明的上限必须真的是 64,防止"抬高上限让门禁变绿"。
		static_assert(kComponentCacheLineBytes == 64, "the component budget itself must not be silently raised");

		std::puts("[World.ComponentLayout] All component layout checks passed.");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "[World.ComponentLayout] Test failed: %s\n", e.what());
		return 1;
	}
}

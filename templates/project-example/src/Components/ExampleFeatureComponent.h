#pragma once
#include "World.h"

#include <map>
#include <string>
#include <vector>

namespace World
{
	// ============================================================================
	// 示例组件(纯数据):怎么声明字段、面板怎么画、什么进存档。
	//
	// 组件只描述"实体有哪些数据",不写逻辑:
	//
	//   * 标量(Health/Speed/...):一行一个控件。写了 Default(...) 的行显示声明默认值;
	//     没写 Default(...) 的行显示"未设" —— Play 时未设字段不覆盖成员初值
	//     (成员初始化 = 组件的默认值),复位(↺)只是回到"未设"。
	//   * 只读摘要(IVec3/Mat4):面板只画一行不可编辑的摘要文本;值不进存档,
	//     Play 时保留成员初值 —— 适合放"运行期状态",不把几何/矩阵写死进场景。
	//   * 嵌套 struct:`WE_SCHEMA_BODY(..., Struct)` 声明一份可复用的字段模型
	//     (字段名 + kind + 默认值);标量字段用 `Object, Of(ExampleStats)` 引用,
	//     面板里展开成可折叠子行,容器元素也能共用同一份模型。
	//   * 容器(Array/Map):声明成 `WE_FIELD(名字, Array|Map, Of(元素类型), ...)`;
	//     C++ 成员类型 = `std::vector<元素>` / `std::map<std::string, 元素>`
	//     (Map 的键固定是 std::string)。元素类型可以是叶子 kind(Float/Vec3/String...)、
	//     枚举(Of(枚举名))、资产(Of("资产类型"))或已注册的命名 struct。
	//     容器没有 Default(...):成员初始化是默认形状,面板里加/改过的元素才进存档;
	//     更深的匿名嵌套(Array<Array<T>>)不支持 —— 用命名 struct 再套容器表达。
	//
	// **逻辑不写在这里**:写到 `src/Systems/` 的系统里,并在 `src/GameProject.cpp` 里挂上。
	// 组件 / 系统 / 脚本库三层边界见 docs/dev/scripting-architecture.md。
	// ============================================================================

	// 枚举示例:WE_ENUM_SCHEMA 注册后,`Enum, Of(ExampleMode)` 在面板里是可编辑下拉,
	// 存档写整数(底层类型 Int32)。
	enum class ExampleMode : int32_t
	{
		None = 0,
		Patrol = 1,
		Chase = 2,
	};
	WE_ENUM_SCHEMA(Game, ExampleMode, Int32)
		WE_ENUM_VALUE(None);
		WE_ENUM_VALUE(Patrol);
		WE_ENUM_VALUE(Chase);
	WE_ENUM_END

	// 嵌套 struct 示例:字段模型(名字 + kind + 默认值)在这里声明一次;
	// 标量字段 Stats 与容器 Squad/Units 的元素都复用这份声明(数组元素可展开成子字段)。
	// Struct 本身只描述数据形状;由引用它的组件字段承载进场景。
	struct ExampleStats
	{
		float Health = 5.0f;
		int32_t Count = 1;

		WE_SCHEMA_BODY(Game, ExampleStats, Struct)
			WE_FIELD(Health, Float, Range(0.0f, 9999.0f),
				Doc("Nested struct sample field: hit points of one entry (declared default 5)."));
			WE_FIELD(Count, Int32,
				Doc("Nested struct sample field: how many units this entry stands for (declared default 1)."));
		WE_SCHEMA_END
	};

	// 组件本体:纯数据,不继承任何基类、没有生命周期回调 —— 逻辑属于系统。
	struct ExampleFeatureComponent
	{
		// ---- 标量:一行一个控件(Default/Range/Unit/Step/Doc 的写法) ----
		float Health = 100.0f;
		float Speed = 1.0f;
		bool Enabled = true;
		std::string Label = "WorldEngine Example";
		ExampleMode Mode = ExampleMode::Patrol;
		// 资产引用 = 驻留 PathId(4B POD);解析由 AssetRegistry/TextureLibrary 在渲染时完成。
		PathId Icon;
		// vec3 可编辑(三个数值分量);IVec3/Mat4 是只读摘要(见上面的注释口径)。
		glm::vec3 SpawnPoint { 0.0f, 1.0f, 0.0f };
		glm::ivec3 GridCell { 0, 0, 0 };
		glm::mat4 PreviewMatrix = glm::mat4(1.0f);
		// 嵌套 struct 标量字段:子字段来自 ExampleStats 的声明。
		ExampleStats Stats {};

		// ---- 容器:std::vector<元素> / std::map<std::string, 元素> ----
		// 成员初始化 = 默认形状(没被编辑过时按这里显示,不写进场景)。
		std::vector<float> Scores { 1.5f, 2.5f, 3.5f };
		std::vector<glm::vec3> Path { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 1.0f } };
		std::vector<ExampleStats> Squad { { 5.0f, 2 }, { 7.0f, 3 } };
		std::vector<ExampleMode> Modes { ExampleMode::Patrol, ExampleMode::Chase };
		std::map<std::string, float> Costs { { "gold", 3.0f }, { "wood", 5.0f } };
		std::map<std::string, ExampleStats> Units { { "boss", { 9.0f, 4 } } };

		WE_SCHEMA_BODY(Game, ExampleFeatureComponent, Component)
			WE_SCHEMA_META(Category("Examples"),
				Doc("Pure-data example component: field declaration and inspector coverage for scalars with edit metadata, bool/string/enum/asset, editable vec3, read-only IVec3/Mat4 summaries, a nested struct and Array/Map container fields."))
			// ---- 标量:Default/Range/Unit/Step/Doc 各来一份(Health/Speed 保留原示例口径) ----
			WE_FIELD(Health, Float, Default(100.0f), Range(0.0f, 1000.0f), Unit("hp"), Step(1.0f),
				Doc("Scalar sample: declared default 100; the inspector edits it with a 0..1000 range slider and shows the 'hp' unit."));
			WE_FIELD(Speed, Float, Default(1.0f), Range(0.0f, 100.0f), Unit("m/s"), Step(0.1f),
				Doc("Scalar sample: declared default 1; Step(0.1) sets the drag/keyboard increment."));
			WE_FIELD(Enabled, Bool, Default(true),
				Doc("Bool sample: a checkbox row; the declared default is true."));
			WE_FIELD(Label, String,
				Doc("String sample: free text row; without Default(...) it starts unset and the member initializer stays in effect."));
			WE_FIELD(Mode, Enum, Of(ExampleMode),
				Doc("Enum sample: WE_ENUM_SCHEMA(Game, ExampleMode, Int32) turns this into a dropdown; the scene stores the integer value."));
			WE_FIELD(Icon, Asset, Of("Texture2D"),
				Doc("Asset sample: searchable asset dropdown (catalog-driven); the scene stores the logical asset path while the component holds a 4-byte resident PathId."));
			// ---- vec3 可编辑 + 只读摘要 ----
			WE_FIELD(SpawnPoint, Vec3,
				Doc("Editable vec3 sample: three numeric components edited together."));
			WE_FIELD(GridCell, IVec3,
				Doc("Read-only summary sample (IVec3): the inspector shows a non-editable summary row and the value never enters the scene file."));
			WE_FIELD(PreviewMatrix, Mat4,
				Doc("Read-only summary sample (Mat4): same rule as GridCell — keep matrices as runtime state, not scene data."));
			// ---- 嵌套 struct:字段模型/默认值来自 ExampleStats 的 WE_SCHEMA_BODY ----
			WE_FIELD(Stats, Object, Of(ExampleStats),
				Doc("Nested struct sample: expandable child rows defined once by Game::ExampleStats (Health/Count with declared defaults)."));
			// ---- Array:元素类型覆盖 Float / Vec3 / Struct / Enum ----
			WE_FIELD(Scores, Array, Of(Float),
				Range(0.0f, 100.0f), Unit("pts"), Step(1.0f),
				Doc("Array<Float> sample: one row per element (row label = index), '-' removes and '+' appends."));
			WE_FIELD(Path, Array, Of(Vec3),
				Doc("Array<Vec3> sample: waypoints; each element is an editable vector row."));
			WE_FIELD(Squad, Array, Of(ExampleStats),
				Doc("Array<Struct> sample: elements are Game::ExampleStats, so each row expands into the struct's own child fields."));
			WE_FIELD(Modes, Array, Of(ExampleMode),
				Doc("Array<Enum> sample: every element is an ExampleMode dropdown; the scene stores integers."));
			// ---- Map:键固定 std::string ----
			WE_FIELD(Costs, Map, Of(Float),
				Range(0.0f, 500.0f), Unit("coins"),
				Doc("Map<Float> sample: one row per key (key = string, value = number); '+' asks for a new key name first."));
			WE_FIELD(Units, Map, Of(ExampleStats),
				Doc("Map<Struct> sample: string key -> Game::ExampleStats; expand an entry to edit the nested fields."));
		WE_SCHEMA_END
	};
}

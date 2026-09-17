#pragma once

#include "World/Core/Export.h"
#include "World/Schema/Schema.h"
#include "World/Script/ScriptValue.h"

#include <string>

namespace World
{
	class Entity;
	class ScriptBindingContext;

	// W3a-A1:Schema → 脚本的字段映射表(单一入口)。
	// 组件字段代理与 W3a-A2 的存根渲染都从这里取类型,不要再写第二张表。
	struct ScriptFieldMapping
	{
		const char* LuaTypeName = nullptr;   // nullptr = 本包未映射:读/写都会给出可读错误
		bool ReadOnly = false;               // 脚本侧只读(身份类字段);读仍然可用
		bool UuidIdentity = false;           // Object(UUID):读=十进制字符串(与 PropertiesPanel 同口径)
	};

	// Entity:GetComponent 返回的字段代理在 Lua 里的类型名(typeof 见 __type)。
	inline constexpr const char* ComponentProxyLuaTypeName = "ComponentProxy";

	// Kind → 脚本类型;Object 只在嵌套类型是 UUID(实体身份)时映射成只读字符串,其余未映射。
	WLD_API ScriptFieldMapping DescribeScriptField(const Schema::FieldSchema& field);

	// 读路径:Schema::Value → 脚本值(vec/mat 复用现有脚本可见的 userdata 类型)。
	// 调用方需先用 DescribeScriptField 确认字段已映射;未映射的 Value 抛出可读 logic_error。
	WLD_API ScriptValue SchemaValueToScript(ScriptBindingContext& bindings,
		const Schema::FieldSchema& field, const Schema::Value& value);

	// 写路径:脚本值 → Schema::Value。返回值的 variant 类型与 schema 生成的 Set 访问器一致
	// (Vec3 → glm::vec3、Enum → int64/uint64、UInt64 → uint64 …);
	// 类型不符/非整数/越界/枚举名未知 → 抛 std::logic_error(可读)。
	WLD_API Schema::Value ScriptValueToSchemaValue(ScriptBindingContext& bindings,
		const Schema::FieldSchema& field, const ScriptValue& value);

	// 注册字段代理 userdata 类型(ComponentProxy);重复注册 → false + error。
	WLD_API bool RegisterComponentProxyBinding(ScriptBindingContext& bindings, std::string* error = nullptr);

	// Entity:GetComponent 的返回值:字段代理(组件存在时)。不做存活检查,检查在每次字段访问时做。
	WLD_API ScriptValue MakeComponentProxy(ScriptBindingContext& bindings,
		const Entity& entity, const Schema::TypeSchema& type);

	// ---- W3a-A2:存根注解辅助(只追加) ----
	// LuaStubGenerator 渲染 `---@field <Name> <LuaType> <Note>` 时使用本结构。
	// LuaType 只从 DescribeScriptField 这一张表派生,不新开第二份 Kind → Lua 类型映射;
	// 未映射的 Kind 用占位类型 "unknown",并在 Note 里说明原因。
	struct ScriptFieldAnnotation
	{
		std::string LuaType;   // 已映射的脚本类型名;未映射 Kind = "unknown"
		std::string Note;      // 空,或 ";" 分隔的稳定标注(no script mapping… / transient / read-only)
	};

	WLD_API ScriptFieldAnnotation DescribeScriptFieldAnnotation(const Schema::FieldSchema& field);
}

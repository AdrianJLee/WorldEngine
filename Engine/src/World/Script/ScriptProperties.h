#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Components.h"
#include "World/Schema/Schema.h"

#include <string>
#include <utility>
#include <vector>

namespace World
{
	// 2026-09-26 脚本组件重写:属性表(`std::vector<ScriptProperty>`)的**唯一维护点**。
	//
	// 为什么单独一层:C++ 脚本的字段来自 schema 反射,Luau 脚本的字段来自 `---@field` 注解,
	// 但两者进检视器 / 进存档 / 参与热重载迁移时用的是**同一份模型**。规则集中在这里:
	//   * 顺序 = 声明顺序(先声明先显示,生成物/注解顺序稳定);
	//   * 保留同名同类型的已有值(编辑器改过、场景读过);类型变了 → 用新声明重置该字段;
	//   * 声明里没有的旧属性直接丢弃(脚本改了就该以脚本为准,不做兼容);
	//   * 只接受叶子类型(标量 / 字符串);其它类型不参与"脚本属性"。
	namespace ScriptProperties
	{
		// 该 schema 值类型能不能当脚本属性(与 schema 的叶类型口径一致)。
		WLD_API bool IsPropertyKind(Schema::Kind kind);

		// 类型的可读名 / 反查(存档里写名字,便于手改与排查;未知名 → Kind::None)。
		WLD_API const char* KindName(Schema::Kind kind);
		WLD_API Schema::Kind KindFromName(const std::string& name);

		// 按 schema 类型(C++ 脚本)同步:fields 里每个叶子字段 → 一条属性。
		WLD_API void SyncFromSchema(std::vector<ScriptProperty>& properties, const Schema::TypeSchema& type);

		// 按"名字 + 类型"声明(Luau 脚本的注解解析结果)同步。
		WLD_API void SyncFromDeclarations(std::vector<ScriptProperty>& properties,
			const std::vector<std::pair<std::string, Schema::Kind>>& declarations);

		WLD_API ScriptProperty* Find(std::vector<ScriptProperty>& properties, const std::string& name);
		WLD_API const ScriptProperty* Find(const std::vector<ScriptProperty>& properties, const std::string& name);
	}
}

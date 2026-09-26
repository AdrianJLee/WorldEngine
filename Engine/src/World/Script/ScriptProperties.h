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
		// V1(2026-09-26 用户反馈):一条脚本属性声明 —— 名字 / schema 类型 / 注解说明 / 脚本里的默认值。
		// 顺序 = 声明顺序(注解顺序 → 脚本表顺序)。
		// Default 为 monostate = 脚本没给出这个字段的默认值(例如注解声明了但脚本表里没有,
		// 或 VM 不可用);此时属性保持"未设",检视器显示"未设"而不是类型零值。
		struct Declaration
		{
			std::string Name;
			Schema::Kind Type = Schema::Kind::None;
			std::string Doc;
			Schema::Value Default;
		};

		// 该 schema 值类型能不能当脚本属性(与 schema 的叶类型口径一致)。
		WLD_API bool IsPropertyKind(Schema::Kind kind);

		// 类型的可读名 / 反查(存档里写名字,便于手改与排查;未知名 → Kind::None)。
		WLD_API const char* KindName(Schema::Kind kind);
		WLD_API Schema::Kind KindFromName(const std::string& name);

		// 按 schema 类型(C++ 脚本)同步:fields 里每个叶子字段 → 一条属性。
		WLD_API void SyncFromSchema(std::vector<ScriptProperty>& properties, const Schema::TypeSchema& type);

		// V1:按完整声明(名字 / 类型 / 说明 / 默认值)同步。规则:
		//   * 顺序 = 声明顺序;Doc 每次刷新(脚本里的说明改了就跟着走);
		//   * 同名同类型 → 保留已有值(场景保存值 / 编辑器改过的值优先;未设仍是未设);
		//   * 新字段 / 类型变化 → 取声明里的默认值;没有默认值 → 保持 monostate(未设),
		//     **绝不写类型零值**(审查 P1-1);
		//   * 声明里没有的旧属性丢弃。
		WLD_API void SyncFromDeclarations(std::vector<ScriptProperty>& properties,
			const std::vector<Declaration>& declarations);

		// 兼容重载(无 doc / 无默认值):等价于每条声明 Doc 为空、Default 为 monostate。
		WLD_API void SyncFromDeclarations(std::vector<ScriptProperty>& properties,
			const std::vector<std::pair<std::string, Schema::Kind>>& declarations);

		// 未设值(monostate)判定:检视器/写入路径用它区分"没有值"与"值为 0"。
		WLD_API bool IsUnset(const ScriptProperty& property);

		WLD_API ScriptProperty* Find(std::vector<ScriptProperty>& properties, const std::string& name);
		WLD_API const ScriptProperty* Find(const std::vector<ScriptProperty>& properties, const std::string& name);
	}
}

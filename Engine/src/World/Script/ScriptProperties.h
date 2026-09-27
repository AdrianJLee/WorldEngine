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
	//   * 叶子类型 = 标量 / 字符串 / Vec2 / Vec3 / Vec4;B 期起 `Schema::Kind::Object` 表示
	//     **嵌套 `---@class` 结构化表**(Fields 递归同构;没有子字段的裸 table = ReadOnly 摘要行);
	//     C 期起还表示**数组/映射**(Collection 区分 Struct/Array/Map,见 Declaration 的字段);
	//     其它类型不参与"脚本属性"。
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
			// B 期(Object 专用):声明的类型名(类名 / "table")与递归子声明(顺序 = 注解顺序)。
			std::string TypeName;
			std::vector<Declaration> Fields;
			// C 期(只追加在尾部):集合形态 + 元素/键类型。Array: Fields = 元素行(顺序 = 下标 1..n,
			// Name = 下标字符串,Default = 脚本表里的初值);Map: Fields = 键值行(Name = 键)。
			// ReadOnly = true 表示这一条只做只读摘要(裸 table / 元素类型不支持 / 推断失败 / 超护栏):
			// 检视器只画一行,不进存档。Struct 的裸 table 也走这一条。
			ScriptPropertyCollection Collection = ScriptPropertyCollection::None;
			Schema::Kind ElementKind = Schema::Kind::None;
			Schema::Kind KeyKind = Schema::Kind::String;
			bool ReadOnly = false;
			// 数组/映射的**元素行/键值行在声明里不可用**(没有 VM 读不到默认表,或脚本表里没有这个字段):
			// 合并时保留属性里已有的子行,不把它当成"空容器"——否则一次无 VM 的同步就会清掉场景里的元素值。
			bool FieldsUnknown = false;
		};

		// 该 schema 值类型能不能当脚本属性(与 schema 的叶类型口径一致)。
		WLD_API bool IsPropertyKind(Schema::Kind kind);

		// 类型的可读名 / 反查(存档里写名字,便于手改与排查;未知名 → Kind::None)。
		WLD_API const char* KindName(Schema::Kind kind);
		WLD_API Schema::Kind KindFromName(const std::string& name);
		// C 期:集合形态的可读名(None/Struct/Array/Map)—— 存档的 `Type: Array|Map` 与诊断共用。
		WLD_API const char* CollectionName(ScriptPropertyCollection collection);

		// 按 schema 类型(C++ 脚本)同步:fields 里每个叶子字段 → 一条属性。
		WLD_API void SyncFromSchema(std::vector<ScriptProperty>& properties, const Schema::TypeSchema& type);

		// V1:按完整声明(名字 / 类型 / 说明 / 默认值)同步。规则:
		//   * 顺序 = 声明顺序;Doc 每次刷新(脚本里的说明改了就跟着走);
		//   * 同名同类型 → 保留已有值(场景保存值 / 编辑器改过的值优先;未设仍是未设);
		//   * D1(2026-09-27):声明里的默认值**只材料化到 `ScriptProperty::Value` 供展示/
		//     Play 兜底**,`Default` 记下声明值;序列化按"Value 未设或与 Default 相同 = 未设"
		//     整条跳过(改脚本默认值后老场景跟着变)。没有默认值 → 保持 monostate(未设),
		//     **绝不写类型零值**(审查 P1-1);
		//   * D2:数组/映射的形状在 `ShapeFromScene`(读档时场景真的写了这个容器)时以场景为准,
		//     声明只按行名补说明/默认值与缺省行;否则用声明的默认形状重建;
		//   * 声明里没有的旧属性丢弃。
		WLD_API void SyncFromDeclarations(std::vector<ScriptProperty>& properties,
			const std::vector<Declaration>& declarations);

		// 兼容重载(无 doc / 无默认值):等价于每条声明 Doc 为空、Default 为 monostate。
		WLD_API void SyncFromDeclarations(std::vector<ScriptProperty>& properties,
			const std::vector<std::pair<std::string, Schema::Kind>>& declarations);

		// 未设值(monostate)判定:检视器/写入路径用它区分"没有值"与"值为 0"。
		WLD_API bool IsUnset(const ScriptProperty& property);

		// D1(2026-09-27 用户口径:复位 = 回到"未设",默认值只用于展示/Play 兜底):
		//   * 声明默认值相同判定(浮点按位相等:同一个值同一条路径写进去,不需要容差);
		//   * IsSceneRecorded = "这条属性在场景里真的记录过" —— 序列化/存档只写它的行:
		//       叶子:Value 未设(monostate)或与 Default 相同 → false;Default 为 monostate
		//             (声明没有默认值)时,有值就是场景自己的值 → true;
		//       容器:任一行被记录 → true(数组/映射的**形状 + 值**一起进存档;
		//             ReadOnly 的只读摘要永不进存档)。
		WLD_API bool ValuesEqual(const Schema::Value& left, const Schema::Value& right);
		WLD_API bool IsDefaultValue(const ScriptProperty& property);
		WLD_API bool IsSceneRecorded(const ScriptProperty& property);

		// 值的 variant 备选是否**正好**是 kind 对应的那一支(手改场景 / 坏存档的防线;
		// monostate 不算匹配 —— 未设值请先用 IsUnset 判定)。
		WLD_API bool ValueMatchesKind(const Schema::Value& value, Schema::Kind kind);

		WLD_API ScriptProperty* Find(std::vector<ScriptProperty>& properties, const std::string& name);
		WLD_API const ScriptProperty* Find(const std::vector<ScriptProperty>& properties, const std::string& name);
	}
}

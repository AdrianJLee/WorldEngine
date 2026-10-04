#include "wldpch.h"

#include "World/Core/AssetId.h"
#include "World/Schema/Schema.h"

// CPPT-6:字段/结构的通用读写(叶 / 枚举 / 资产 / 命名 struct / 容器,递归同构)。
// 契约见 Schema.h 的声明注释;这里是 SchemaWriter/SchemaReader 与生成的容器访问器共用的唯一实现。

namespace World::Schema
{
	const char* CollectionKindName(CollectionKind collection)
	{
		switch (collection)
		{
			case CollectionKind::Array: return "Array";
			case CollectionKind::Map: return "Map";
			case CollectionKind::None:
			default: return "None";
		}
	}

	Value ReadSchemaField(const FieldSchema& field, const void* instance)
	{
		if (!instance || field.Meta.Transient)
			return Value {};
		if (field.Collection != CollectionKind::None)
			return field.Get ? field.Get(instance) : Value {};
		if (field.K == Kind::Object)
		{
			const TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
			const void* nestedInstance = field.GetPtrConst ? field.GetPtrConst(instance) : nullptr;
			if (!nested || !nestedInstance)
				return Value {};
			return ReadStructValue(*nested, nestedInstance);
		}
		return field.Get ? field.Get(instance) : Value {};
	}

	Value ReadStructValue(const TypeSchema& type, const void* instance)
	{
		ValueMap fields;
		if (!instance)
			return Value(std::move(fields));
		for (const FieldSchema& child : type.Fields)
		{
			if (child.Meta.Transient)
				continue;
			fields.emplace(child.Name, ReadSchemaField(child, instance));
			// 资产稳定身份:与路径并列写一个 `<字段名>Id` 兄弟键(.wd 里人可读、读回时一起恢复)。
			// 只在已分配身份时写 —— 未分配不产生噪音键。
			if (child.K == Kind::Asset && child.GetAssetIdentity)
			{
				const uint64_t identity = child.GetAssetIdentity(instance);
				if (identity != 0)
					fields.emplace(child.Name + "Id", Value(FormatAssetId(AssetId { identity })));
			}
		}
		return Value(std::move(fields));
	}

	bool WriteSchemaField(const FieldSchema& field, void* instance, const Value& value)
	{
		if (!instance || field.Meta.Transient)
			return false;
		// 未设 = 保持实例原值(与脚本属性 D1 的"未设不落盘/不覆盖"同口径)。
		if (std::holds_alternative<std::monostate>(value))
			return false;
		if (field.Collection != CollectionKind::None)
		{
			if (!field.Set)
				return false;
			field.Set(instance, value);
			return true;
		}
		if (field.K == Kind::Object)
		{
			const TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
			void* nestedInstance = field.GetPtr ? field.GetPtr(instance) : nullptr;
			if (!nested || !nestedInstance || !std::holds_alternative<ValueMap>(value))
				return false;
			return WriteStructValue(*nested, nestedInstance, value);
		}
		if (!field.Set)
			return false;
		field.Set(instance, value);
		return true;
	}

	bool WriteStructValue(const TypeSchema& type, void* instance, const Value& value)
	{
		const ValueMap* fields = std::get_if<ValueMap>(&value);
		if (!fields || !instance)
			return false;
		bool anyWritten = false;
		for (const FieldSchema& child : type.Fields)
		{
			if (child.Meta.Transient)
				continue;
			const auto found = fields->find(child.Name);
			if (found == fields->end() || std::holds_alternative<std::monostate>(found->second))
				continue;   // 未设子字段:保留实例/脚本自己的成员初值
			anyWritten = WriteSchemaField(child, instance, found->second) || anyWritten;
			// 资产身份与路径一起恢复(键不存在 = 老文件/未分配 ⇒ 保持实例原值)。
			if (child.K == Kind::Asset && child.SetAssetIdentity)
			{
				const auto identity = fields->find(child.Name + "Id");
				if (identity != fields->end())
				{
					const std::string* text = std::get_if<std::string>(&identity->second);
					AssetId parsed;
					if (text && ParseAssetId(*text, &parsed))
						child.SetAssetIdentity(instance, parsed.Value);
				}
			}
		}
		return anyWritten;
	}
}

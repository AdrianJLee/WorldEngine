#include "wldpch.h"

#include "World/Script/ScriptProperties.h"

#include <algorithm>
#include <unordered_map>

namespace World
{
	namespace ScriptProperties
	{
		const char* KindName(Schema::Kind kind)
		{
			switch (kind)
			{
				case Schema::Kind::Bool: return "Bool";
				case Schema::Kind::Int8: return "Int8";
				case Schema::Kind::Int16: return "Int16";
				case Schema::Kind::Int32: return "Int32";
				case Schema::Kind::Int64: return "Int64";
				case Schema::Kind::UInt8: return "UInt8";
				case Schema::Kind::UInt16: return "UInt16";
				case Schema::Kind::UInt32: return "UInt32";
				case Schema::Kind::UInt64: return "UInt64";
				case Schema::Kind::Float: return "Float";
				case Schema::Kind::Double: return "Double";
				case Schema::Kind::String: return "String";
				case Schema::Kind::None:
				default: return "None";
			}
		}

		Schema::Kind KindFromName(const std::string& name)
		{
			if (name == "Bool") return Schema::Kind::Bool;
			if (name == "Int8") return Schema::Kind::Int8;
			if (name == "Int16") return Schema::Kind::Int16;
			if (name == "Int32") return Schema::Kind::Int32;
			if (name == "Int64") return Schema::Kind::Int64;
			if (name == "UInt8") return Schema::Kind::UInt8;
			if (name == "UInt16") return Schema::Kind::UInt16;
			if (name == "UInt32") return Schema::Kind::UInt32;
			if (name == "UInt64") return Schema::Kind::UInt64;
			if (name == "Float") return Schema::Kind::Float;
			if (name == "Double") return Schema::Kind::Double;
			if (name == "String") return Schema::Kind::String;
			return Schema::Kind::None;
		}

		bool IsPropertyKind(Schema::Kind kind)
		{
			switch (kind)
			{
				case Schema::Kind::Bool:
				case Schema::Kind::Int8:
				case Schema::Kind::Int16:
				case Schema::Kind::Int32:
				case Schema::Kind::Int64:
				case Schema::Kind::UInt8:
				case Schema::Kind::UInt16:
				case Schema::Kind::UInt32:
				case Schema::Kind::UInt64:
				case Schema::Kind::Float:
				case Schema::Kind::Double:
				case Schema::Kind::String:
					return true;
				default:
					return false;
			}
		}

		namespace
		{
			// V1 契约开关(2026-09-26):声明里的默认值是"同步时就写进组件"还是"只作为显示/Play 兜底"?
			// 派工单 item 3 要"把默认值填进 ScriptProperty.Value";item 4① / 验收 #3 要
			// "未改过就不落盘"。两者互斥 —— 由主 agent 裁决,这里保留一个开关便于一键切换。
			// true  = 新字段/未设字段直接材料化脚本默认值(面板零改动也不显示 0);
			// false = 保持 monostate(未设),默认值只放在 Declaration.Default 里给检视器显示。
			constexpr bool kMaterializeDeclarationDefaults = true;

			void Apply(std::vector<ScriptProperty>& properties, const std::vector<Declaration>& declarations)
			{
				std::unordered_map<std::string, ScriptProperty> previous;
				previous.reserve(properties.size());
				for (ScriptProperty& property : properties)
					previous.emplace(property.Name, std::move(property));

				std::vector<ScriptProperty> next;
				next.reserve(declarations.size());
				for (const Declaration& declaration : declarations)
				{
					if (!IsPropertyKind(declaration.Type))
						continue;
					if (std::any_of(next.begin(), next.end(),
							[&declaration](const ScriptProperty& item) { return item.Name == declaration.Name; }))
						continue;   // 重复声明以第一次为准(与注解解析同口径)

					ScriptProperty property;
					property.Name = declaration.Name;
					property.Type = declaration.Type;
					property.Doc = declaration.Doc;
					property.Value = declaration.Default;   // 没有默认值 → monostate(未设),不写零值

					// 场景保存值 / 编辑器改过的值优先;同名同类型时原样保留(未设也保留,除非开关要求补默认值)。
					const auto found = previous.find(declaration.Name);
					const bool sameType = found != previous.end() && found->second.Type == declaration.Type;
					const bool unsetExisting =
						sameType && std::holds_alternative<std::monostate>(found->second.Value);
					if (sameType && !(kMaterializeDeclarationDefaults && unsetExisting))
						property.Value = std::move(found->second.Value);
					next.push_back(std::move(property));
				}
				properties = std::move(next);
			}
		}

		void SyncFromSchema(std::vector<ScriptProperty>& properties, const Schema::TypeSchema& type)
		{
			std::vector<Declaration> declared;
			for (const Schema::FieldSchema& field : type.Fields)
			{
				if (!IsPropertyKind(field.K))
					continue;
				Declaration declaration;
				declaration.Name = field.Name;
				declaration.Type = field.K;
				declaration.Doc = field.Meta.Doc;     // C++ 脚本的说明来自 schema 的 Doc("…")
				declaration.Default = field.Default;
				declared.push_back(std::move(declaration));
			}
			Apply(properties, declared);
		}

		void SyncFromDeclarations(std::vector<ScriptProperty>& properties,
			const std::vector<Declaration>& declarations)
		{
			Apply(properties, declarations);
		}

		void SyncFromDeclarations(std::vector<ScriptProperty>& properties,
			const std::vector<std::pair<std::string, Schema::Kind>>& declarations)
		{
			std::vector<Declaration> declared;
			declared.reserve(declarations.size());
			for (const auto& [name, kind] : declarations)
			{
				if (!IsPropertyKind(kind))
					continue;
				Declaration declaration;
				declaration.Name = name;
				declaration.Type = kind;
				declared.push_back(std::move(declaration));
			}
			Apply(properties, declared);
		}

		bool IsUnset(const ScriptProperty& property)
		{
			return std::holds_alternative<std::monostate>(property.Value);
		}

		bool ValueMatchesKind(const Schema::Value& value, Schema::Kind kind)
		{
			switch (kind)
			{
				case Schema::Kind::Bool: return std::holds_alternative<bool>(value);
				case Schema::Kind::Int8: return std::holds_alternative<int8_t>(value);
				case Schema::Kind::Int16: return std::holds_alternative<int16_t>(value);
				case Schema::Kind::Int32: return std::holds_alternative<int32_t>(value);
				case Schema::Kind::Int64: return std::holds_alternative<int64_t>(value);
				case Schema::Kind::UInt8: return std::holds_alternative<uint8_t>(value);
				case Schema::Kind::UInt16: return std::holds_alternative<uint16_t>(value);
				case Schema::Kind::UInt32: return std::holds_alternative<uint32_t>(value);
				case Schema::Kind::UInt64: return std::holds_alternative<uint64_t>(value);
				case Schema::Kind::Float: return std::holds_alternative<float>(value);
				case Schema::Kind::Double: return std::holds_alternative<double>(value);
				case Schema::Kind::String: return std::holds_alternative<std::string>(value);
				default: return false;
			}
		}

		ScriptProperty* Find(std::vector<ScriptProperty>& properties, const std::string& name)
		{
			const auto found = std::find_if(properties.begin(), properties.end(),
				[&name](const ScriptProperty& property) { return property.Name == name; });
			return found == properties.end() ? nullptr : &*found;
		}

		const ScriptProperty* Find(const std::vector<ScriptProperty>& properties, const std::string& name)
		{
			const auto found = std::find_if(properties.begin(), properties.end(),
				[&name](const ScriptProperty& property) { return property.Name == name; });
			return found == properties.end() ? nullptr : &*found;
		}
	}
}

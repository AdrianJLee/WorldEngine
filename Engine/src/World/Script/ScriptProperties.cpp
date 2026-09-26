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
			void Apply(std::vector<ScriptProperty>& properties,
				const std::vector<std::pair<std::string, Schema::Kind>>& declared,
				const std::vector<Schema::Value>& defaults)
			{
				std::unordered_map<std::string, ScriptProperty> previous;
				previous.reserve(properties.size());
				for (ScriptProperty& property : properties)
					previous.emplace(property.Name, std::move(property));

				std::vector<ScriptProperty> next;
				next.reserve(declared.size());
				for (std::size_t index = 0; index < declared.size(); ++index)
				{
					const std::string& name = declared[index].first;
					const Schema::Kind kind = declared[index].second;
					if (!IsPropertyKind(kind))
						continue;

					ScriptProperty property;
					property.Name = name;
					property.Type = kind;
					property.Value = index < defaults.size() ? defaults[index] : Schema::Value {};

					const auto found = previous.find(name);
					if (found != previous.end() && found->second.Type == kind)
						property.Value = std::move(found->second.Value);
					next.push_back(std::move(property));
				}
				properties = std::move(next);
			}
		}

		void SyncFromSchema(std::vector<ScriptProperty>& properties, const Schema::TypeSchema& type)
		{
			std::vector<std::pair<std::string, Schema::Kind>> declared;
			std::vector<Schema::Value> defaults;
			for (const Schema::FieldSchema& field : type.Fields)
			{
				if (!IsPropertyKind(field.K))
					continue;
				declared.emplace_back(field.Name, field.K);
				defaults.push_back(field.Default);
			}
			Apply(properties, declared, defaults);
		}

		void SyncFromDeclarations(std::vector<ScriptProperty>& properties,
			const std::vector<std::pair<std::string, Schema::Kind>>& declarations)
		{
			std::vector<std::pair<std::string, Schema::Kind>> declared;
			declared.reserve(declarations.size());
			for (const auto& [name, kind] : declarations)
			{
				if (!IsPropertyKind(kind))
					continue;
				if (std::any_of(declared.begin(), declared.end(),
						[&name](const auto& item) { return item.first == name; }))
					continue;   // 重复声明以第一次为准(与注解解析同口径)
				declared.emplace_back(name, kind);
			}
			Apply(properties, declared, std::vector<Schema::Value>(declared.size()));
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

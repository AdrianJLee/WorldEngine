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
				// VEC-A1(D2):向量属性走同一份模型;Kinds 名与 Schema::Kind 一一对应,
				// 存档里的 `Type: Vec3` 由这里写、KindFromName 读回。
				case Schema::Kind::Vec2: return "Vec2";
				case Schema::Kind::Vec3: return "Vec3";
				case Schema::Kind::Vec4: return "Vec4";
				case Schema::Kind::Object: return "Object";   // B 期:嵌套 ---@class 结构化表
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
			if (name == "Vec2") return Schema::Kind::Vec2;
			if (name == "Vec3") return Schema::Kind::Vec3;
			if (name == "Vec4") return Schema::Kind::Vec4;
			if (name == "Object") return Schema::Kind::Object;
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
				case Schema::Kind::Vec2:
				case Schema::Kind::Vec3:
				case Schema::Kind::Vec4:
				case Schema::Kind::Object:
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

			// B 期:结构化表的递归护栏(与 plan v2 §护栏 一致)。
			constexpr int kMaxObjectDepth = 4;
			constexpr size_t kMaxObjectFields = 64;

			// 一条声明 → 一条属性(递归)。previous = 上一轮的属性表(场景保存值 / 编辑器改过的值)。
			// 叶子:同名同类型旧值优先;Object:Value 保持 monostate,子字段按名字/类型递归对齐,
			// 声明里消失的子字段丢弃(与"脚本即事实源"同口径)。
			void ApplyInto(std::vector<ScriptProperty>& out, const std::vector<Declaration>& declarations,
				const std::vector<ScriptProperty>& previous, int depth)
			{
				std::unordered_map<std::string, const ScriptProperty*> previousByName;
				previousByName.reserve(previous.size());
				for (const ScriptProperty& property : previous)
					previousByName.emplace(property.Name, &property);

				static const std::vector<ScriptProperty> kNoChildren;
				static const std::vector<Declaration> kNoDeclarations;

				out.clear();
				out.reserve(declarations.size());
				for (const Declaration& declaration : declarations)
				{
					if (!IsPropertyKind(declaration.Type))
						continue;
					if (std::any_of(out.begin(), out.end(),
							[&declaration](const ScriptProperty& item) { return item.Name == declaration.Name; }))
						continue;   // 重复声明以第一次为准(与注解解析同口径)
					if (out.size() >= kMaxObjectFields)
						break;      // 护栏:单层子字段上限(超出部分不展开,由上层出诊断)

					ScriptProperty property;
					property.Name = declaration.Name;
					property.Type = declaration.Type;
					property.Doc = declaration.Doc;
					property.TypeName = declaration.TypeName;

					const auto found = previousByName.find(declaration.Name);
					const ScriptProperty* old = (found != previousByName.end()
						&& found->second->Type == declaration.Type) ? found->second : nullptr;

					if (declaration.Type == Schema::Kind::Object)
					{
						// 没有子字段的裸 table = 只读摘要行(看得到、不进存档);有子字段则递归展开。
						property.ReadOnly = declaration.Fields.empty();
						if (!property.ReadOnly && depth < kMaxObjectDepth)
							ApplyInto(property.Children, declaration.Fields,
								old ? old->Children : kNoChildren, depth + 1);
					}
					else
					{
						property.Value = declaration.Default;   // 没有默认值 → monostate(未设),不写零值
						const bool unsetExisting = old && std::holds_alternative<std::monostate>(old->Value);
						if (old && !(kMaterializeDeclarationDefaults && unsetExisting))
							property.Value = old->Value;
					}
					out.push_back(std::move(property));
				}
			}

			void Apply(std::vector<ScriptProperty>& properties, const std::vector<Declaration>& declarations)
			{
				std::vector<ScriptProperty> next;
				ApplyInto(next, declarations, properties, 0);
				properties = std::move(next);
			}
		}

		namespace
		{
			// B 期:schema 侧的 Object 字段(嵌套结构体)递归成子声明 —— 与 Luau 侧同一份模型。
			void AppendSchemaDeclarations(std::vector<Declaration>& out, const Schema::TypeSchema& type, int depth)
			{
				if (depth >= kMaxObjectDepth)
					return;
				for (const Schema::FieldSchema& field : type.Fields)
				{
					if (!IsPropertyKind(field.K))
						continue;
					if (out.size() >= kMaxObjectFields)
						return;
					Declaration declaration;
					declaration.Name = field.Name;
					declaration.Type = field.K;
					declaration.Doc = field.Meta.Doc;     // C++ 脚本的说明来自 schema 的 Doc("…")
					if (field.K == Schema::Kind::Object)
					{
						const Schema::TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
						if (!nested)
							continue;                      // 拿不到嵌套 schema:不进属性表(不静默展开成空表)
						declaration.TypeName = nested->Id.Name;
						AppendSchemaDeclarations(declaration.Fields, *nested, depth + 1);
					}
					else
					{
						declaration.Default = field.Default;
					}
					out.push_back(std::move(declaration));
				}
			}
		}

		void SyncFromSchema(std::vector<ScriptProperty>& properties, const Schema::TypeSchema& type)
		{
			std::vector<Declaration> declared;
			AppendSchemaDeclarations(declared, type, 0);
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
				case Schema::Kind::Vec2: return std::holds_alternative<glm::vec2>(value);
				case Schema::Kind::Vec3: return std::holds_alternative<glm::vec3>(value);
				case Schema::Kind::Vec4: return std::holds_alternative<glm::vec4>(value);
				// B 期:结构化表的值在 Children 里,Value 必须是 monostate(空表也是 monostate)。
				case Schema::Kind::Object: return std::holds_alternative<std::monostate>(value);
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

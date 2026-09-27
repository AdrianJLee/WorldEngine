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

		const char* CollectionName(ScriptPropertyCollection collection)
		{
			switch (collection)
			{
				case ScriptPropertyCollection::Struct: return "Struct";
				case ScriptPropertyCollection::Array: return "Array";
				case ScriptPropertyCollection::Map: return "Map";
				case ScriptPropertyCollection::None:
				default: return "None";
			}
		}

		namespace
		{
			// B 期:结构化表的递归护栏(与 plan v2 §护栏 一致)。
			constexpr int kMaxObjectDepth = 4;
			constexpr size_t kMaxObjectFields = 64;

			// C 期:上一轮的属性与新声明是不是"同一形状"(同名同类型才保值)。
			// Object 还要比集合形态与元素/键类型:数组元素类型变了(或 Struct↔Array 变了)必须回新默认值,
			// 不能拿旧值按新形状解释。
			bool SameShape(const ScriptProperty& property, const Declaration& declaration)
			{
				if (property.Type != declaration.Type)
					return false;
				if (property.Collection != declaration.Collection)
					return false;
				if (declaration.Collection == ScriptPropertyCollection::Array)
					return property.ElementKind == declaration.ElementKind;
				if (declaration.Collection == ScriptPropertyCollection::Map)
					return property.KeyKind == declaration.KeyKind &&
						property.ElementKind == declaration.ElementKind;
				return true;
			}

			// D1(2026-09-27 用户口径):一个值是不是"未设" —— 未设(monostate)或与声明默认值相同。
			// 声明没有默认值(monostate)时,有值就是场景自己的值(不算未设)。
			bool ValueIsUnsetOrDefault(const Schema::Value& value, const Schema::Value& declaredDefault)
			{
				if (std::holds_alternative<std::monostate>(value))
					return true;
				if (std::holds_alternative<std::monostate>(declaredDefault))
					return false;
				return ValuesEqual(value, declaredDefault);
			}

			// 一条声明 → 一条属性(递归)。previous = 上一轮的属性表(场景保存值 / 编辑器改过的值)。
			// 叶子:同名同类型旧值优先(D1:未设/仍是旧默认值 → 换成新默认值,但不落盘);
			// Object:Value 保持 monostate,子字段按名字/类型递归对齐,声明里消失的子字段丢弃
			// (与"脚本即事实源"同口径);数组/映射见 D2 的形状归属。
			void ApplyInto(std::vector<ScriptProperty>& out, const std::vector<Declaration>& declarations,
				const std::vector<ScriptProperty>& previous, int depth);
			void AdoptSceneRows(std::vector<ScriptProperty>& out,
				const std::vector<ScriptProperty>& sceneRows,
				const std::vector<Declaration>& declaredRows, int depth);

			// D1:一条叶子属性按声明刷新 —— 场景记录过的值优先;未设/仍等于上次默认值 → 换成新默认值
			// (展示值跟着脚本走;要不要落盘由 IsSceneRecorded 判定)。old 为空 = 新字段 → 只用默认值展示。
			void ApplyLeafInto(ScriptProperty& property, const Declaration& declaration,
				const ScriptProperty* old)
			{
				// 先取出旧值/旧默认值:RefreshSceneRow 会传 `&row`(property 与 old 同一对象),
				// 直接读写同一 variant 会在覆盖 Default 之后拿到新默认值。
				const Schema::Value oldValue = old ? old->Value : Schema::Value {};
				const Schema::Value oldDefault = old ? old->Default : Schema::Value {};
				property.Default = declaration.Default;
				if (!old)
				{
					property.Value = declaration.Default;   // 展示/Play 兜底;未设 → 不让它进场景
					return;
				}
				// 上一次同步留下的默认值优先(它才代表"上次是不是材料化出来的");
				// 第一次同步(Default 还是 monostate,值来自存档/手改场景)拿**这次**声明的默认值比 ——
				// 老场景里等于声明默认值的旧材料化值同样按"未设"处理,脚本改了默认值就跟着走。
				const Schema::Value& reference = std::holds_alternative<std::monostate>(oldDefault)
					? declaration.Default : oldDefault;
				if (ValueIsUnsetOrDefault(oldValue, reference))
					property.Value = declaration.Default;
				else
					property.Value = oldValue;
			}

			// D2:一条**场景行**(数组元素 / 映射键值行)→ 按声明刷新。行名/顺序/子行以场景为准,
			// 声明按行名补说明/类型/默认值;类型不兼容的行按"场景独有"处理(不拿旧值按新形状解释)。
			void RefreshSceneRow(ScriptProperty& row, const Declaration& declaration, int depth)
			{
				row.Type = declaration.Type;
				row.Doc = declaration.Doc;
				row.TypeName = declaration.TypeName;
				row.Collection = declaration.Collection;
				row.ElementKind = declaration.ElementKind;
				row.KeyKind = declaration.KeyKind;
				row.ReadOnly = declaration.ReadOnly;
				if (depth >= kMaxObjectDepth)
					return;   // 超护栏:场景行原样保留,不再深挖
				if (declaration.Collection == ScriptPropertyCollection::Array ||
					declaration.Collection == ScriptPropertyCollection::Map)
				{
					if (declaration.FieldsUnknown)
						return;   // 元素行读不出来:场景子行原样保留
					const std::vector<ScriptProperty> nested = row.Children;
					if (row.ShapeFromScene)
						AdoptSceneRows(row.Children, nested, declaration.Fields, depth + 1);
					else
					{
						row.Children.clear();
						ApplyInto(row.Children, declaration.Fields, nested, depth + 1);
					}
					return;
				}
				if (declaration.Type == Schema::Kind::Object)
				{
					// 元素/键值是结构化表:字段以声明为准(按名字对齐,值保留)。
					const std::vector<ScriptProperty> nested = row.Children;
					row.Children.clear();
					if (declaration.Fields.empty())
					{
						row.ReadOnly = true;
						return;
					}
					ApplyInto(row.Children, declaration.Fields, nested, depth + 1);
					return;
				}
				ApplyLeafInto(row, declaration, &row);
			}

			// D2:场景形状的子行 —— 行集合以场景为准(顺序/名字/值),声明按名字补默认值与说明。
			// 场景独有的行(编辑器 `+` 出来的 / 声明里删掉的键)没有声明默认值:值就是场景的。
			void AdoptSceneRows(std::vector<ScriptProperty>& out,
				const std::vector<ScriptProperty>& sceneRows,
				const std::vector<Declaration>& declaredRows, int depth)
			{
				out.clear();
				out.reserve(sceneRows.size());
				for (const ScriptProperty& sceneRow : sceneRows)
				{
					const Declaration* declaration = nullptr;
					for (const Declaration& candidate : declaredRows)
					{
						if (candidate.Name == sceneRow.Name && SameShape(sceneRow, candidate))
						{
							declaration = &candidate;
							break;
						}
					}
					ScriptProperty row = sceneRow;
					if (declaration)
					{
						RefreshSceneRow(row, *declaration, depth);
					}
					else
					{
						row.Default = Schema::Value {};   // 场景独有:没有声明默认值 → 值一定落盘
						if (row.Collection != ScriptPropertyCollection::None)
							row.ShapeFromScene = true;    // 递归层同理:子行以场景为准
					}
					out.push_back(std::move(row));
				}
			}

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
					property.Collection = declaration.Collection;
					property.ElementKind = declaration.ElementKind;
					property.KeyKind = declaration.KeyKind;

					const auto found = previousByName.find(declaration.Name);
					const ScriptProperty* old = (found != previousByName.end()
						&& SameShape(*found->second, declaration)) ? found->second : nullptr;

					if (declaration.Collection == ScriptPropertyCollection::Array ||
						declaration.Collection == ScriptPropertyCollection::Map)
					{
						// C 期:数组/映射 —— 空数组/空映射仍是**可编辑**的(加元素/加键),
						// 只有声明自己标了 ReadOnly(元素类型不支持 / 推断失败 / 超护栏)才降级摘要。
						property.ReadOnly = declaration.ReadOnly;
						// D2:形状归属(场景真的写过这个容器)随属性继承。
						property.ShapeFromScene = old ? old->ShapeFromScene : false;
						if (declaration.FieldsUnknown)
						{
							// 元素行读不出来(无 VM / 脚本表里没有该字段):保留已有子行,不按"空容器"处理。
							if (old)
								property.Children = old->Children;
						}
						else if (!property.ReadOnly && property.ShapeFromScene && old)
						{
							// D2:**场景记录过形状** → 元素个数/键名以场景为准,
							// 声明只按行名补齐说明/类型/默认值(不再把声明里的行塞回来)。
							AdoptSceneRows(property.Children, old->Children, declaration.Fields, depth + 1);
						}
						else if (!property.ReadOnly && depth < kMaxObjectDepth)
						{
							// 场景没记录过形状(新建属性 / 脚本改过的旧场景)→ 用声明的默认形状重建。
							ApplyInto(property.Children, declaration.Fields,
								old ? old->Children : kNoChildren, depth + 1);
						}
					}
					else if (declaration.Type == Schema::Kind::Object)
					{
						// 没有子字段的裸 table = 只读摘要行(看得到、不进存档);有子字段则递归展开。
						property.ReadOnly = declaration.ReadOnly || declaration.Fields.empty();
						if (!property.ReadOnly && depth < kMaxObjectDepth)
							ApplyInto(property.Children, declaration.Fields,
								old ? old->Children : kNoChildren, depth + 1);
					}
					else
					{
						// D1:声明默认值只用于展示/Play 兜底 —— 材料化进 Value,同时记进 Default;
						// 序列化按 IsSceneRecorded 跳过"未设或 == Default"的整条(绝不把默认值当场景值)。
						ApplyLeafInto(property, declaration, old);
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
						declaration.Collection = ScriptPropertyCollection::Struct;   // C 期:结构化表(不是数组/映射)
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

		// D1:variant 同支相等。脚本属性只用到 monostate / 整数 / 浮点 / bool / string / Vec2-4,
		// 向量按分量比(不引 glm 的 operator== 语义),其余变体(脚本属性里不会出现)一律"不相等"。
		bool ValuesEqual(const Schema::Value& left, const Schema::Value& right)
		{
			if (left.index() != right.index())
				return false;
			return std::visit([](const auto& rawLeft, const auto& rawRight) -> bool
			{
				using Left = std::decay_t<decltype(rawLeft)>;
				using Right = std::decay_t<decltype(rawRight)>;
				if constexpr (!std::is_same_v<Left, Right>)
					return false;
				else if constexpr (std::is_same_v<Left, std::monostate>)
					return true;
				else if constexpr (std::is_same_v<Left, glm::vec2>)
					return rawLeft.x == rawRight.x && rawLeft.y == rawRight.y;
				else if constexpr (std::is_same_v<Left, glm::vec3>)
					return rawLeft.x == rawRight.x && rawLeft.y == rawRight.y && rawLeft.z == rawRight.z;
				else if constexpr (std::is_same_v<Left, glm::vec4>)
					return rawLeft.x == rawRight.x && rawLeft.y == rawRight.y
						&& rawLeft.z == rawRight.z && rawLeft.w == rawRight.w;
				else if constexpr (std::is_same_v<Left, glm::quat>)
					return rawLeft.x == rawRight.x && rawLeft.y == rawRight.y
						&& rawLeft.z == rawRight.z && rawLeft.w == rawRight.w;
				else if constexpr (std::is_same_v<Left, glm::mat3> || std::is_same_v<Left, glm::mat4>)
				{
					for (int column = 0; column < rawLeft.length(); ++column)
						for (int row = 0; row < rawLeft[column].length(); ++row)
							if (rawLeft[column][row] != rawRight[column][row])
								return false;
					return true;
				}
				else
					return rawLeft == rawRight;   // 标量 / bool / string
			}, left, right);
		}

		// D1:这条叶子当前值是不是"声明默认值"(Default 为 monostate = 声明没给默认值 → false)。
		bool IsDefaultValue(const ScriptProperty& property)
		{
			if (std::holds_alternative<std::monostate>(property.Default))
				return false;
			return ValuesEqual(property.Value, property.Default);
		}

		// D1/D2:"这条属性在场景里真的记录过" —— 序列化只写它为 true 的(容器另见 ShapeFromScene)。
		bool IsSceneRecorded(const ScriptProperty& property)
		{
			if (property.ReadOnly)
				return false;   // 只读摘要(裸 table / 降级):看得到、不进存档
			if (property.Collection == ScriptPropertyCollection::None)
			{
				if (std::holds_alternative<std::monostate>(property.Value))
					return false;   // 未设(monostate):场景里不写这个字段
				return !IsDefaultValue(property);
			}
			return std::any_of(property.Children.begin(), property.Children.end(),
				[](const ScriptProperty& child) { return IsSceneRecorded(child); });
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

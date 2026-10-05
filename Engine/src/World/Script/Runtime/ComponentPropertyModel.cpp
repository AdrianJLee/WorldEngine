#include "wldpch.h"

#include "World/Script/Runtime/ComponentPropertyModel.h"

#include <algorithm>
#include <unordered_map>

namespace World
{
	namespace ComponentPropertyModel
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
				// CPPT-2:未支持 Kind 的只读摘要行也需要可读名(诊断/摘要行文本)。
				case Schema::Kind::IVec2: return "IVec2";
				case Schema::Kind::IVec3: return "IVec3";
				case Schema::Kind::IVec4: return "IVec4";
				case Schema::Kind::UVec2: return "UVec2";
				case Schema::Kind::UVec3: return "UVec3";
				case Schema::Kind::UVec4: return "UVec4";
				case Schema::Kind::Quat: return "Quat";
				case Schema::Kind::Mat3: return "Mat3";
				case Schema::Kind::Mat4: return "Mat4";
				case Schema::Kind::Enum: return "Enum";
				case Schema::Kind::Asset: return "Asset";
				case Schema::Kind::Name: return "Name";
				case Schema::Kind::Text: return "Text";
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
			if (name == "IVec2") return Schema::Kind::IVec2;
			if (name == "IVec3") return Schema::Kind::IVec3;
			if (name == "IVec4") return Schema::Kind::IVec4;
			if (name == "UVec2") return Schema::Kind::UVec2;
			if (name == "UVec3") return Schema::Kind::UVec3;
			if (name == "UVec4") return Schema::Kind::UVec4;
			if (name == "Quat") return Schema::Kind::Quat;
			if (name == "Mat3") return Schema::Kind::Mat3;
			if (name == "Mat4") return Schema::Kind::Mat4;
			if (name == "Enum") return Schema::Kind::Enum;
			if (name == "Asset") return Schema::Kind::Asset;
			if (name == "Name") return Schema::Kind::Name;
			if (name == "Text") return Schema::Kind::Text;
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
				case Schema::Kind::Name:
				case Schema::Kind::Text:
				case Schema::Kind::Enum:
				case Schema::Kind::Asset:
					return true;
				default:
					return false;
			}
		}

		bool IsSummaryKind(Schema::Kind kind)
		{
			switch (kind)
			{
				case Schema::Kind::IVec2:
				case Schema::Kind::IVec3:
				case Schema::Kind::IVec4:
				case Schema::Kind::UVec2:
				case Schema::Kind::UVec3:
				case Schema::Kind::UVec4:
				case Schema::Kind::Quat:
				case Schema::Kind::Mat3:
				case Schema::Kind::Mat4:
					return true;
				default:
					return false;
			}
		}

		const char* CollectionName(PropertyCollection collection)
		{
			switch (collection)
			{
				case PropertyCollection::Struct: return "Struct";
				case PropertyCollection::Array: return "Array";
				case PropertyCollection::Map: return "Map";
				case PropertyCollection::None:
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
			bool SameShape(const PropertyNode& property, const Declaration& declaration)
			{
				if (property.Type != declaration.Type)
					return false;
				if (property.Collection != declaration.Collection)
					return false;
				if (declaration.Collection == PropertyCollection::Array)
					return property.ElementKind == declaration.ElementKind;
				if (declaration.Collection == PropertyCollection::Map)
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
			void ApplyInto(std::vector<PropertyNode>& out, const std::vector<Declaration>& declarations,
				const std::vector<PropertyNode>& previous, int depth);
			void AdoptSceneRows(std::vector<PropertyNode>& out,
				const std::vector<PropertyNode>& sceneRows,
				const std::vector<Declaration>& declaredRows, int depth);

			// D1:一条叶子属性按声明刷新 —— 场景记录过的值优先;未设/仍等于上次默认值 → 换成新默认值
			// (展示值跟着脚本走;要不要落盘由 IsSceneRecorded 判定)。old 为空 = 新字段 → 只用默认值展示。
			void ApplyLeafInto(PropertyNode& property, const Declaration& declaration,
				const PropertyNode* old)
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
			void RefreshSceneRow(PropertyNode& row, const Declaration& declaration, int depth)
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
				if (declaration.Collection == PropertyCollection::Array ||
					declaration.Collection == PropertyCollection::Map)
				{
					if (declaration.FieldsUnknown)
						return;   // 元素行读不出来:场景子行原样保留
					const std::vector<PropertyNode> nested = row.Children;
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
					const std::vector<PropertyNode> nested = row.Children;
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
			void AdoptSceneRows(std::vector<PropertyNode>& out,
				const std::vector<PropertyNode>& sceneRows,
				const std::vector<Declaration>& declaredRows, int depth)
			{
				out.clear();
				out.reserve(sceneRows.size());
				for (const PropertyNode& sceneRow : sceneRows)
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
					PropertyNode row = sceneRow;
					if (declaration)
					{
						RefreshSceneRow(row, *declaration, depth);
					}
					else
					{
						row.Default = Schema::Value {};   // 场景独有:没有声明默认值 → 值一定落盘
						if (row.Collection != PropertyCollection::None)
							row.ShapeFromScene = true;    // 递归层同理:子行以场景为准
					}
					out.push_back(std::move(row));
				}
			}

			void ApplyInto(std::vector<PropertyNode>& out, const std::vector<Declaration>& declarations,
				const std::vector<PropertyNode>& previous, int depth)
			{
				std::unordered_map<std::string, const PropertyNode*> previousByName;
				previousByName.reserve(previous.size());
				for (const PropertyNode& property : previous)
					previousByName.emplace(property.Name, &property);

				static const std::vector<PropertyNode> kNoChildren;
				static const std::vector<Declaration> kNoDeclarations;

				out.clear();
				out.reserve(declarations.size());
				for (const Declaration& declaration : declarations)
				{
					if (!IsPropertyKind(declaration.Type) && !IsSummaryKind(declaration.Type))
						continue;
					if (std::any_of(out.begin(), out.end(),
							[&declaration](const PropertyNode& item) { return item.Name == declaration.Name; }))
						continue;   // 重复声明以第一次为准(与注解解析同口径)
					if (out.size() >= kMaxObjectFields)
						break;      // 护栏:单层子字段上限(超出部分不展开,由上层出诊断)

					PropertyNode property;
					property.Name = declaration.Name;
					property.Type = declaration.Type;
					property.Doc = declaration.Doc;
					property.TypeName = declaration.TypeName;
					property.Collection = declaration.Collection;
					property.ElementKind = declaration.ElementKind;
					property.KeyKind = declaration.KeyKind;
					property.ReadOnly = declaration.ReadOnly;

					const auto found = previousByName.find(declaration.Name);
					const PropertyNode* old = (found != previousByName.end()
						&& SameShape(*found->second, declaration)) ? found->second : nullptr;

					if (!IsPropertyKind(declaration.Type))
					{
						// CPPT-2:未支持 Kind(IVec*/UVec*/Quat/Mat3/Mat4)—— 只读摘要行:
						// 看得到(类型/说明),不可编辑、不进存档;Value 保持未设。
						property.ReadOnly = true;
						out.push_back(std::move(property));
						continue;
					}
					if (declaration.Collection == PropertyCollection::Array ||
						declaration.Collection == PropertyCollection::Map)
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

			void Apply(std::vector<PropertyNode>& properties, const std::vector<Declaration>& declarations)
			{
				std::vector<PropertyNode> next;
				ApplyInto(next, declarations, properties, 0);
				properties = std::move(next);
			}

			// CPPT-6:集合行 → schema 容器值(Play 应用 / 测试共用)。元素是叶子时原样带出
			// (未设 = monostate,由生成的拆箱回落类型零值);元素是命名 struct 时折成
			// "字段名 → Value" 的 map(递归);嵌套集合(命名 struct 再套容器)递归折。
			// CPPT-6-FIX2:结构化表的判定只看 **Type==Object**(Collection 为 None 或 Struct 同义),
			// 两种读回路径(元素行 / 嵌套 struct 字段)折出同一份 ValueMap。
			Schema::Value ElementValueOf(const PropertyNode& row);

			Schema::Value FoldContainer(const PropertyNode& container)
			{
				if (container.Collection == PropertyCollection::Map)
				{
					Schema::ValueMap fields;
					for (const PropertyNode& row : container.Children)
					{
						if (row.ReadOnly)
							continue;
						fields.emplace(row.Name, ElementValueOf(row));
					}
					return Schema::Value(std::move(fields));
				}
				Schema::ValueList items;
				items.reserve(container.Children.size());
				for (const PropertyNode& row : container.Children)
					items.push_back(ElementValueOf(row));
				return Schema::Value(std::move(items));
			}

			Schema::Value ElementValueOf(const PropertyNode& row)
			{
				// 容器行(数组/映射)递归折成 ValueList / ValueMap。
				if (row.Collection == PropertyCollection::Array ||
					row.Collection == PropertyCollection::Map)
					return FoldContainer(row);
				// CPPT-6-FIX2:结构化表的**唯一** schema 值形态是"字段名 → Value"的 map
				// (读回的元素行 Collection=None、嵌套 struct 字段 Collection=Struct,两者同形;
				// `Schema::WriteStructValue` / 生成的容器访问器只吃 ValueMap)。
				// 旧实现让 Struct 走 FoldContainer → 折成 ValueList,Play 时整条 struct 元素写不进去
				// (字段回落到脚本成员初值)。
				if (row.Type == Schema::Kind::Object)
				{
					Schema::ValueMap fields;
					for (const PropertyNode& child : row.Children)
					{
						if (child.ReadOnly)
							continue;
						fields.emplace(child.Name, ElementValueOf(child));
					}
					return Schema::Value(std::move(fields));
				}
				return row.Value;
			}
		}

		namespace
		{
			// B 期:schema 侧的 Object 字段(嵌套结构体)递归成子声明 —— 与 Luau 侧同一份模型。
			// CPPT-2:Enum(枚举下拉)/Asset(资产下拉)放行为可编辑属性;IVec*/UVec*/Quat/Mat* 降级
			// 只读摘要行(看得到、不进存档);Enum 拿不到枚举 schema 时同样降级,不静默丢弃。
			void AppendSchemaDeclarations(std::vector<Declaration>& out, const Schema::TypeSchema& type, int depth)
			{
				if (depth >= kMaxObjectDepth)
					return;
				for (const Schema::FieldSchema& field : type.Fields)
				{
					if (!IsPropertyKind(field.K) && !IsSummaryKind(field.K))
						continue;
					if (out.size() >= kMaxObjectFields)
						return;
					Declaration declaration;
					declaration.Name = field.Name;
					declaration.Type = field.K;
					declaration.Doc = field.Meta.Doc;     // C++ 脚本的说明来自 schema 的 Doc("…")
					// CPPT-6:C++ 容器字段 → 与 Luau 集合同一份行模型:Type 仍是 Object,
					// Collection/ElementKind/KeyKind 描述元素与键;声明里**没有元素行**
					// (C++ 的元素来自脚本成员初值,schema 看不到)—— 场景记录过形状时按 D2 以场景为准。
					if (field.Collection != Schema::CollectionKind::None)
					{
						declaration.Collection = field.Collection == Schema::CollectionKind::Map
							? PropertyCollection::Map : PropertyCollection::Array;
						declaration.ElementKind = field.ElementKind;
						declaration.KeyKind = field.KeyKind;
						if (field.ElementTypeName)
							declaration.TypeName = field.ElementTypeName;
						if (field.ElementKind == Schema::Kind::Object)
						{
							const Schema::TypeSchema* nested = field.GetElementNested ? field.GetElementNested() : nullptr;
							if (nested)
								declaration.TypeName = nested->Id.Name;
							else
								declaration.ReadOnly = true;   // 元素 schema 拿不到:只读摘要行
						}
						else if (field.ElementKind == Schema::Kind::Enum)
						{
							const Schema::EnumSchema* enumSchema = field.GetEnum ? field.GetEnum() : nullptr;
							if (enumSchema)
								declaration.TypeName = enumSchema->Name;
							else
								declaration.ReadOnly = true;
						}
						else if (field.ElementKind == Schema::Kind::Asset)
						{
							declaration.TypeName = field.AssetTypeName ? field.AssetTypeName : "";
						}
						else if (IsSummaryKind(field.ElementKind))
						{
							declaration.ReadOnly = true;       // 面板没有行控件(与标量摘要行同口径)
						}
						out.push_back(std::move(declaration));
						continue;
					}
					if (field.K == Schema::Kind::Object)
					{
						const Schema::TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
						if (!nested)
							continue;                      // 拿不到嵌套 schema:不进属性表(不静默展开成空表)
						declaration.Collection = PropertyCollection::Struct;   // C 期:结构化表(不是数组/映射)
						declaration.TypeName = nested->Id.Name;
						AppendSchemaDeclarations(declaration.Fields, *nested, depth + 1);
					}
					else if (field.K == Schema::Kind::Enum)
					{
						const Schema::EnumSchema* enumSchema = field.GetEnum ? field.GetEnum() : nullptr;
						if (!enumSchema)
						{
							declaration.ReadOnly = true;   // 枚举 schema 缺失:仍显示一行,不可编辑
						}
						else
						{
							declaration.TypeName = enumSchema->Name;
							declaration.Default = field.Default;
						}
					}
					else if (field.K == Schema::Kind::Asset)
					{
						declaration.TypeName = field.AssetTypeName ? field.AssetTypeName : "";
						declaration.Default = field.Default;
					}
					else if (field.K == Schema::Kind::Name)
					{
						// 名字字段在脚本/存根里就是一个字符串属性。
						declaration.TypeName = "String";
						declaration.Default = field.Default;
					}
					else if (field.K == Schema::Kind::Text)
					{
						// 有界文本在边界同样是字符串(容量是内存侧的实现细节)。
						declaration.TypeName = "String";
						declaration.Default = field.Default;
					}
					else if (IsSummaryKind(field.K))
					{
						declaration.ReadOnly = true;       // 面板无行控件:只读摘要,不进存档
					}
					else
					{
						declaration.Default = field.Default;
					}
					out.push_back(std::move(declaration));
				}
			}
		}

		void SyncFromSchema(std::vector<PropertyNode>& properties, const Schema::TypeSchema& type)
		{
			std::vector<Declaration> declared;
			AppendSchemaDeclarations(declared, type, 0);
			Apply(properties, declared);
		}

		// CPPT-6:容器属性的 Play 应用值(见头文件契约)。"设过"的判据与序列化同源:
		// 场景记录过任一行(IsSceneRecorded)或场景写过这个容器的形状(ShapeFromScene,含空容器)。
		bool BuildContainerValue(const PropertyNode& property, Schema::Value* outValue)
		{
			// 只有数组/映射有"容器值";Struct 行(结构化表)不是容器,不在这里折。
			if (!outValue || property.ReadOnly ||
				(property.Collection != PropertyCollection::Array &&
					property.Collection != PropertyCollection::Map))
				return false;
			if (!IsSceneRecorded(property) && !property.ShapeFromScene)
				return false;
			*outValue = FoldContainer(property);
			return true;
		}

		// PURE-ECS:无条件折叠(见头文件契约)。判据与 BuildContainerValue 的守卫一致,只是去掉
		// "IsSceneRecorded / ShapeFromScene" 那一段 —— 纯 ECS 组件字段的值来自实例本身。
		bool FoldContainerRows(const PropertyNode& property, Schema::Value* outValue)
		{
			if (!outValue || property.ReadOnly ||
				(property.Collection != PropertyCollection::Array &&
					property.Collection != PropertyCollection::Map))
				return false;
			*outValue = FoldContainer(property);
			return true;
		}

		void SyncFromDeclarations(std::vector<PropertyNode>& properties,
			const std::vector<Declaration>& declarations)
		{
			Apply(properties, declarations);
		}

		void SyncFromDeclarations(std::vector<PropertyNode>& properties,
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

		bool IsUnset(const PropertyNode& property)
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
		bool IsDefaultValue(const PropertyNode& property)
		{
			if (std::holds_alternative<std::monostate>(property.Default))
				return false;
			return ValuesEqual(property.Value, property.Default);
		}

		// D1/D2:"这条属性在场景里真的记录过" —— 序列化只写它为 true 的(容器另见 ShapeFromScene)。
		bool IsSceneRecorded(const PropertyNode& property)
		{
			if (property.ReadOnly)
				return false;   // 只读摘要(裸 table / 降级):看得到、不进存档
			if (property.Collection == PropertyCollection::None)
			{
				if (std::holds_alternative<std::monostate>(property.Value))
					return false;   // 未设(monostate):场景里不写这个字段
				return !IsDefaultValue(property);
			}
			return std::any_of(property.Children.begin(), property.Children.end(),
				[](const PropertyNode& child) { return IsSceneRecorded(child); });
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
				// CPPT-6:容器的元素/值可以是一层"字段名 → Value"的 map(命名 struct),
				// 所以 Object 也接受 ValueMap。
				case Schema::Kind::Object:
					return std::holds_alternative<std::monostate>(value)
						|| std::holds_alternative<Schema::ValueMap>(value);
				case Schema::Kind::String: return std::holds_alternative<std::string>(value);
				case Schema::Kind::Name:   // 名字的边界同样是字符串
				case Schema::Kind::Text:   // 有界文本同理
					return std::holds_alternative<std::string>(value);
				// CPPT-2:Enum 存整数(与组件 Enum 字段同一读写器;有符号/无符号按底层类型)、
				// Asset 存逻辑路径字符串。
				case Schema::Kind::Enum:
					return std::holds_alternative<int64_t>(value) || std::holds_alternative<uint64_t>(value);
				case Schema::Kind::Asset: return std::holds_alternative<std::string>(value);
				default: return false;
			}
		}

		PropertyNode* Find(std::vector<PropertyNode>& properties, const std::string& name)
		{
			const auto found = std::find_if(properties.begin(), properties.end(),
				[&name](const PropertyNode& property) { return property.Name == name; });
			return found == properties.end() ? nullptr : &*found;
		}

		const PropertyNode* Find(const std::vector<PropertyNode>& properties, const std::string& name)
		{
			const auto found = std::find_if(properties.begin(), properties.end(),
				[&name](const PropertyNode& property) { return property.Name == name; });
			return found == properties.end() ? nullptr : &*found;
		}
	}
}

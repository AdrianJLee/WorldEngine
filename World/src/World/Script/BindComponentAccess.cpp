#include "wldpch.h"
#include "World/Script/BindComponentAccess.h"

#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Script/ScriptBindingContext.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace World
{
	namespace
	{
		// W3e:嵌套对象字段代理的最大深度(字段段数)。SceneCamera 这类一层嵌套远低于上限;
		// 上限存在的意义是给"自引用/环形嵌套"一个确定的可读错误,而不是无限递归。
		constexpr std::size_t kMaxNestedProxyDepth = 4;

		// 字段代理载荷:只存"弱生命周期实体句柄 + 组件 id + 诊断用类型名 + 嵌套字段路径"。
		// 每次字段访问都重新从根组件沿路径解析 schema 与实例地址,所以实体销毁/组件移除/
		// 场景析构/嵌套链中途换类型之后访问代理只会得到可读错误,不会留下悬垂指针
		// (schema 注册表也用 id/名字反查,不缓存 TypeSchema* —— SchemaRegistry 的条目存在
		// vector 里,追加注册会让旧指针失效)。
		struct ComponentProxy
		{
			Entity Owner;
			uint32_t ComponentId = 0;
			std::string ComponentName;
			// 空 = 组件根代理(Entity:GetComponent 的返回值);非空 = 从 ComponentName 起逐层
			// 解引用的 Object 字段路径(例:{"Camera"})。路径长度 <= kMaxNestedProxyDepth。
			std::vector<std::string> Path;
		};

		struct ResolvedProxy
		{
			ComponentProxy* Proxy = nullptr;
			const Schema::TypeSchema* Type = nullptr;
			void* Instance = nullptr;
			Scene* OwnerScene = nullptr;
		};

		const char* SchemaKindName(Schema::Kind kind)
		{
			switch (kind)
			{
			case Schema::Kind::None: return "None";
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
			case Schema::Kind::Vec2: return "Vec2";
			case Schema::Kind::Vec3: return "Vec3";
			case Schema::Kind::Vec4: return "Vec4";
			case Schema::Kind::IVec2: return "IVec2";
			case Schema::Kind::IVec3: return "IVec3";
			case Schema::Kind::IVec4: return "IVec4";
			case Schema::Kind::UVec2: return "UVec2";
			case Schema::Kind::UVec3: return "UVec3";
			case Schema::Kind::UVec4: return "UVec4";
			case Schema::Kind::Quat: return "Quat";
			case Schema::Kind::Mat3: return "Mat3";
			case Schema::Kind::Mat4: return "Mat4";
			case Schema::Kind::String: return "String";
			case Schema::Kind::Enum: return "Enum";
			case Schema::Kind::Asset: return "Asset";
			case Schema::Kind::Object: return "Object";
			}
			return "Unknown";
		}

		std::string DescribeProxy(const ComponentProxy& proxy)
		{
			std::string name = proxy.ComponentName;
			for (const std::string& segment : proxy.Path)
				name += "." + segment;
			return name;
		}

		[[noreturn]] void Fail(const ComponentProxy& proxy, const std::string& message)
		{
			throw std::logic_error("ComponentProxy(" + DescribeProxy(proxy) + "): " + message);
		}

		const Schema::FieldSchema* FindField(const Schema::TypeSchema& type, const std::string& name)
		{
			for (const Schema::FieldSchema& field : type.Fields)
				if (field.Name == name)
					return &field;
			return nullptr;
		}

		std::string DescribeFields(const Schema::TypeSchema& type)
		{
			std::string list;
			for (const Schema::FieldSchema& field : type.Fields)
			{
				if (!list.empty())
					list += ", ";
				list += field.Name;
			}
			return list.empty() ? std::string("(none)") : list;
		}

		template <typename T>
		ScriptValue MakeMathUserdata(ScriptBindingContext& bindings, const char* typeName, const T& value)
		{
			ScriptValue result = bindings.NewUserdata(typeName);
			T* target = nullptr;
			if (!bindings.Unwrap<T>(typeName, result, &target) || !target)
				throw std::logic_error(std::string(typeName) + ": failed to allocate a script value");
			new (target) T(value);
			return result;
		}

		ResolvedProxy RequireLiveProxy(ScriptBindingContext& bindings, const ScriptValue* args,
			std::size_t argCount, const char* operation)
		{
			ComponentProxy* proxy = nullptr;
			if (argCount < 1 || !bindings.Unwrap<ComponentProxy>(ComponentProxyLuaTypeName, args[0], &proxy) || !proxy)
				throw std::logic_error(std::string("ComponentProxy:") + operation + " expects a component proxy receiver");
			// IsValid 先看场景弱生命周期令牌,不会解引用已经析构的 Scene。
			if (!proxy->Owner.IsValid())
				Fail(*proxy, "the entity handle is invalid or its scene has expired; call Entity:GetComponent again");
			Scene* scene = proxy->Owner.GetScene();
			const Schema::TypeSchema* type = scene->GetContext().Schemas().FindByComponentId(proxy->ComponentId);
			if (!type || !type->Storage)
				Fail(*proxy, "the component type is no longer registered in this scene; call Entity:GetComponent again");
			if (!proxy->Owner.HasComponent(proxy->ComponentId))
				Fail(*proxy, "the component was removed from the entity; call Entity:GetComponent again");
			void* instance = proxy->Owner.GetComponent(proxy->ComponentId);
			if (!instance)
				Fail(*proxy, "the component was removed from the entity; call Entity:GetComponent again");

			// W3e:嵌套代理逐段重解析。链中任一段不再是同一个已注册嵌套类型 → 可读错误
			// (不做静默回退,避免脚本拿到一个语义不明的旧值)。
			for (std::size_t index = 0; index < proxy->Path.size(); ++index)
			{
				const Schema::FieldSchema* field = FindField(*type, proxy->Path[index]);
				if (!field || field->K != Schema::Kind::Object)
					Fail(*proxy, "nested field '" + proxy->Path[index] + "' is no longer a schema object field; call Entity:GetComponent again");
				if (!field->GetPtr || !field->GetPtrConst || !field->GetNested)
					Fail(*proxy, "nested field '" + proxy->Path[index] + "' has no schema accessors; call Entity:GetComponent again");
				const Schema::TypeSchema* nested = field->GetNested();
				if (!nested)
					Fail(*proxy, "nested field '" + proxy->Path[index] + "' has no registered nested type; call Entity:GetComponent again");
				void* nestedInstance = field->GetPtr(instance);
				if (!nestedInstance)
					Fail(*proxy, "nested field '" + proxy->Path[index] + "' is no longer available on the component; call Entity:GetComponent again");
				type = nested;
				instance = nestedInstance;
			}
			return { proxy, type, instance, scene };
		}

		// 字段是否是"可代理的嵌套对象":Object + 已注册嵌套类型 + 有字段。
		// UUID 身份字段的嵌套类型没有字段,因此这里天然为 false(走既有的只读字符串路径)。
		bool IsProxyObject(const ComponentProxy& proxy, const Schema::FieldSchema& field, Scene& scene)
		{
			if (field.K != Schema::Kind::Object)
				return false;
			const Schema::TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
			if (!nested || nested->Fields.empty())
				return false;
			// UUID 身份字段由 DescribeScriptField 判定(只读字符串),永远不做嵌套代理。
			if (nested->Id.Name == "World::UUID" || nested->DisplayName == "UUID")
				return false;
			// 只有当前场景注册表里存在的类型才能被代理:未注册类型保持"可读错误",不静默给 nil 代理。
			const Schema::TypeSchema* registered = scene.GetContext().Schemas().Find(nested->Id.Name);
			if (!registered || registered->Id.Hash != nested->Id.Hash)
				return false;
			// 类型自引用/环形嵌套:深度上限给出确定错误,不做无限解引用。
			if (proxy.Path.size() >= kMaxNestedProxyDepth)
				Fail(proxy, "nested object field '" + field.Name + "' exceeds the maximum nested proxy depth (" +
					std::to_string(kMaxNestedProxyDepth) +
					" field segments); deeply nested or self-referencing schema objects are not exposed to scripts");
			return true;
		}

		// 为 Object 字段构造嵌套代理(路径追加一段)。立即解析一次:结构异常在字段访问处就报错,
		// 而不是留一个后续才失败的"僵尸代理"。
		ScriptValue MakeNestedProxy(ScriptBindingContext& bindings, const ComponentProxy& parent,
			const Schema::FieldSchema& field, const ResolvedProxy& resolved)
		{
			if (!field.GetPtr || !field.GetPtrConst || !field.GetNested)
				Fail(parent, "nested object field '" + field.Name + "' has no schema accessors");
			void* nestedInstance = field.GetPtr(resolved.Instance);
			if (!nestedInstance)
				Fail(parent, "nested object field '" + field.Name + "' is not available on the component");

			ComponentProxy payload;
			payload.Owner = parent.Owner;
			payload.ComponentId = parent.ComponentId;
			payload.ComponentName = parent.ComponentName;
			payload.Path = parent.Path;
			payload.Path.push_back(field.Name);

			const ScriptValue value = bindings.NewUserdata(ComponentProxyLuaTypeName);
			ComponentProxy* target = nullptr;
			if (!bindings.Unwrap<ComponentProxy>(ComponentProxyLuaTypeName, value, &target) || !target)
				throw std::logic_error("ComponentProxy: failed to allocate the nested component proxy userdata");
			new (target) ComponentProxy(std::move(payload));
			return value;
		}

		std::string RequireFieldName(const ScriptValue* args, std::size_t argCount, const ComponentProxy& proxy)
		{
			std::string name;
			if (argCount < 2 || !args[1].AsString(&name))
				Fail(proxy, "field access needs a string field name");
			return name;
		}

		// UUID(实体身份)与 PropertiesPanel 同口径:十进制字符串,脚本侧只读。
		std::string ReadUuidIdentity(const ComponentProxy& proxy, const Schema::FieldSchema& field, const void* instance)
		{
			const Schema::TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
			const void* nestedInstance = field.GetPtrConst ? field.GetPtrConst(instance) : nullptr;
			if (nested && nestedInstance && !nested->Fields.empty() && nested->Fields[0].Get)
			{
				const Schema::Value inner = nested->Fields[0].Get(nestedInstance);
				if (const uint64_t* unsignedValue = std::get_if<uint64_t>(&inner))
					return std::to_string(*unsignedValue);
				if (const int64_t* signedValue = std::get_if<int64_t>(&inner))
					return std::to_string(*signedValue);
			}
			Fail(proxy, "field '" + field.Name + "' cannot be read as an identity value");
		}

		// 字段写入后的派生状态刷新。与 PropertiesPanel::DrawComponentInspector 的既有约定一致:
		// TransformComponent 的 Transform 矩阵是派生缓存(Hierarchy.cpp 用 transform->Transform 计算
		// 世界矩阵),只改 Location/Rotation/Scale 必须重算,否则层次与渲染仍读到旧矩阵。
		// W3e:CameraComponent.Camera(SceneCamera 嵌套结构)的投影矩阵同样要在字段写入后重算,
		// 所以这里按"叶实例的类型"判定 —— 嵌套写入时传的就是解析后的 SceneCamera 实例。
		// 遗留:这是按类型名的表;后续应把 PostSet/OnChanged 钩子放进 schema,见 W3a-1 报告。
		void NotifyComponentFieldsChanged(const Schema::TypeSchema& leafType, void* leafInstance)
		{
			if (leafType.Id.Name == "World::TransformComponent")
				static_cast<TransformComponent*>(leafInstance)->RecalculateTransform();
			else if (leafType.Id.Name == "World::SceneCamera")
				static_cast<SceneCamera*>(leafInstance)->ApplyEdit();
		}

		ScriptValue ComponentProxyIndex(const ScriptValue* args, std::size_t argCount)
		{
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
			const ResolvedProxy resolved = RequireLiveProxy(bindings, args, argCount, ":__index");
			const std::string name = RequireFieldName(args, argCount, *resolved.Proxy);
			const Schema::FieldSchema* field = FindField(*resolved.Type, name);
			if (!field)
				Fail(*resolved.Proxy, "no field '" + name + "'; schema fields: " + DescribeFields(*resolved.Type));
			// UUID 身份字段排在嵌套代理之前:它的 Object 只是值的包装,脚本侧语义仍是只读十进制字符串。
			const ScriptFieldMapping mapping = DescribeScriptField(*field);
			if (mapping.UuidIdentity)
				return ScriptValue::String(ReadUuidIdentity(*resolved.Proxy, *field, resolved.Instance));
			// W3e:嵌套对象字段在读路径上先于叶值映射判定,返回嵌套字段代理。
			if (IsProxyObject(*resolved.Proxy, *field, *resolved.OwnerScene))
				return MakeNestedProxy(bindings, *resolved.Proxy, *field, resolved);
			if (!mapping.LuaTypeName)
				Fail(*resolved.Proxy, "field '" + name + "' has schema kind '" + SchemaKindName(field->K) +
					"' which has no script mapping yet");
			if (!field->Get)
				Fail(*resolved.Proxy, "field '" + name + "' has no schema getter");
			return SchemaValueToScript(bindings, *field, field->Get(resolved.Instance));
		}

		ScriptValue ComponentProxyNewIndex(const ScriptValue* args, std::size_t argCount)
		{
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
			const ResolvedProxy resolved = RequireLiveProxy(bindings, args, argCount, ":__newindex");
			const std::string name = RequireFieldName(args, argCount, *resolved.Proxy);
			if (argCount < 3)
				Fail(*resolved.Proxy, "field assignment needs a value");
			const Schema::FieldSchema* field = FindField(*resolved.Type, name);
			if (!field)
				Fail(*resolved.Proxy, "no field '" + name + "'; schema fields: " + DescribeFields(*resolved.Type));
			// W3e:嵌套对象字段是只读的结构入口(读它拿嵌套代理,写它只会得到可读错误)。
			if (IsProxyObject(*resolved.Proxy, *field, *resolved.OwnerScene))
			{
				if (field->Meta.ReadOnly)
					Fail(*resolved.Proxy, "field '" + name + "' is marked ReadOnly by its schema");
				if (field->Meta.Transient)
					Fail(*resolved.Proxy, "field '" + name + "' is transient (derived state); write its source field instead");
				Fail(*resolved.Proxy, "field '" + name + "' is a nested schema object and is read-only for scripts; " +
					"write one of its leaf fields instead (for example component.<field>.<leaf>)");
			}
			const ScriptFieldMapping mapping = DescribeScriptField(*field);
			if (!mapping.LuaTypeName)
				Fail(*resolved.Proxy, "field '" + name + "' has schema kind '" + SchemaKindName(field->K) +
					"' which has no script mapping yet");
			// W3d:HierarchyComponent.Parent 由 Hierarchy::SetParent/ClearParent 维护双向表,
			// 直接写字段会绕过环检测/自身校验/深度上限;这里给出可读错误并指向正门。
			if (resolved.Type->Id.Name == "World::HierarchyComponent" && field->Name == "Parent")
				Fail(*resolved.Proxy, "field 'Parent' is managed by the hierarchy and is read-only for scripts; use Entity:SetParent or Entity:ClearParent");
			if (mapping.ReadOnly)
				Fail(*resolved.Proxy, "field '" + name + "' is read-only for scripts (identity field)");
			if (field->Meta.ReadOnly)
				Fail(*resolved.Proxy, "field '" + name + "' is marked ReadOnly by its schema");
			if (field->Meta.Transient)
				Fail(*resolved.Proxy, "field '" + name + "' is transient (derived state); write its source field instead");
			if (!field->Set)
				Fail(*resolved.Proxy, "field '" + name + "' has no schema setter");

			Schema::Value value;
			try
			{
				value = ScriptValueToSchemaValue(bindings, *field, args[2]);
			}
			catch (const std::exception& error)
			{
				Fail(*resolved.Proxy, error.what());
			}
			field->Set(resolved.Instance, value);
			// 派生状态按"叶实例的类型"刷新:组件根写入传组件实例,嵌套写入传嵌套实例。
			NotifyComponentFieldsChanged(*resolved.Type, resolved.Instance);
			return ScriptValue::Nil();
		}

		ScriptValue ComponentProxyToString(const ScriptValue* args, std::size_t argCount)
		{
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
			ComponentProxy* proxy = nullptr;
			if (argCount < 1 || !bindings.Unwrap<ComponentProxy>(ComponentProxyLuaTypeName, args[0], &proxy) || !proxy)
				return ScriptValue::String("ComponentProxy");
			return ScriptValue::String("ComponentProxy(" + DescribeProxy(*proxy) + ")");
		}

		bool RequireBool(const Schema::FieldSchema& field, const ScriptValue& value)
		{
			bool result = false;
			if (!value.AsBool(&result))
				throw std::logic_error("field '" + field.Name + "' expects a boolean, got " + value.TypeName());
			return result;
		}

		double RequireFiniteNumber(const Schema::FieldSchema& field, const ScriptValue& value)
		{
			double number = 0.0;
			if (!value.AsNumber(&number) || !std::isfinite(number))
				throw std::logic_error("field '" + field.Name + "' expects a finite number, got " + value.TypeName());
			return number;
		}

		template <typename T>
		T RequireInteger(const Schema::FieldSchema& field, const ScriptValue& value)
		{
			const double number = RequireFiniteNumber(field, value);
			if (std::floor(number) != number)
				throw std::logic_error("field '" + field.Name + "' expects an integer, got " + std::to_string(number));
			if (number < static_cast<double>(std::numeric_limits<T>::lowest()) ||
				number > static_cast<double>(std::numeric_limits<T>::max()))
				throw std::logic_error("field '" + field.Name + "' is outside the range of schema kind '" +
					SchemaKindName(field.K) + "'");
			return static_cast<T>(number);
		}

		std::string RequireString(const Schema::FieldSchema& field, const ScriptValue& value, const char* what)
		{
			std::string text;
			if (!value.AsString(&text))
				throw std::logic_error("field '" + field.Name + "' expects " + what + ", got " + value.TypeName());
			return text;
		}

		template <typename T>
		T RequireMathUserdata(ScriptBindingContext& bindings, const Schema::FieldSchema& field,
			const ScriptValue& value, const char* typeName)
		{
			T* pointer = nullptr;
			if (!bindings.Unwrap<T>(typeName, value, &pointer) || !pointer)
				throw std::logic_error("field '" + field.Name + "' expects a " + std::string(typeName) +
					" userdata, got " + value.TypeName());
			return *pointer;
		}

		std::string DescribeEnumNames(const Schema::EnumSchema& schema)
		{
			std::string list;
			for (const auto& entry : schema.Values)
			{
				if (!list.empty())
					list += ", ";
				list += entry.first;
			}
			return list.empty() ? std::string("(none)") : list;
		}
	}

	ScriptFieldMapping DescribeScriptField(const Schema::FieldSchema& field)
	{
		ScriptFieldMapping mapping;
		switch (field.K)
		{
		case Schema::Kind::Bool:
			mapping.LuaTypeName = "boolean";
			break;
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
		case Schema::Kind::Enum:   // 读 = 数值;写也接受枚举名(见 ScriptValueToSchemaValue)
			mapping.LuaTypeName = "number";
			break;
		case Schema::Kind::Vec2:
			mapping.LuaTypeName = "vec2";
			break;
		case Schema::Kind::Vec3:
			mapping.LuaTypeName = "vec3";
			break;
		case Schema::Kind::Vec4:
			mapping.LuaTypeName = "vec4";
			break;
		case Schema::Kind::Mat3:
			mapping.LuaTypeName = "mat3";
			break;
		case Schema::Kind::Mat4:
			mapping.LuaTypeName = "mat4";
			break;
		case Schema::Kind::String:
			mapping.LuaTypeName = "string";
			break;
		case Schema::Kind::Asset:   // 边界值是资产路径字符串(是否重新加载由资产系统决定)
			mapping.LuaTypeName = "string";
			break;
		case Schema::Kind::Object:
		{
			const Schema::TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
			if (nested && (nested->Id.Name == "World::UUID" || nested->DisplayName == "UUID"))
			{
				mapping.LuaTypeName = "string";
				mapping.ReadOnly = true;
				mapping.UuidIdentity = true;
			}
			// W3e:其它嵌套结构是"可代理"的 Object(具体是否可用还要看嵌套类型是否在当前
			// 场景注册表里);LuaTypeName 保持 nullptr,由代理读路径返回嵌套代理而不是叶值。
			else if (nested && !nested->Fields.empty())
				mapping.NestedObject = true;
			break;   // 空嵌套结构(未注册类型)仍无脚本映射
		}
		default:
			break;   // None / IVec* / UVec* / Quat:本包未映射(读写都会给出可读错误)
		}
		return mapping;
	}

	ScriptValue SchemaValueToScript(ScriptBindingContext& bindings, const Schema::FieldSchema& field,
		const Schema::Value& value)
	{
		return std::visit([&bindings, &field](const auto& payload) -> ScriptValue
		{
			using Payload = std::decay_t<decltype(payload)>;
			if constexpr (std::is_same_v<Payload, std::monostate>)
				return ScriptValue::Nil();
			else if constexpr (std::is_same_v<Payload, bool>)
				return ScriptValue::Boolean(payload);
			else if constexpr (std::is_arithmetic_v<Payload>)
				return ScriptValue::Number(static_cast<double>(payload));
			else if constexpr (std::is_same_v<Payload, glm::vec2>)
				return MakeMathUserdata(bindings, "vec2", payload);
			else if constexpr (std::is_same_v<Payload, glm::vec3>)
				return MakeMathUserdata(bindings, "vec3", payload);
			else if constexpr (std::is_same_v<Payload, glm::vec4>)
				return MakeMathUserdata(bindings, "vec4", payload);
			else if constexpr (std::is_same_v<Payload, glm::mat3>)
				return MakeMathUserdata(bindings, "mat3", payload);
			else if constexpr (std::is_same_v<Payload, glm::mat4>)
				return MakeMathUserdata(bindings, "mat4", payload);
			else if constexpr (std::is_same_v<Payload, std::string>)
				return ScriptValue::String(payload);
			else
				throw std::logic_error("field '" + field.Name + "' has schema kind '" +
					SchemaKindName(field.K) + "' which has no script mapping yet");
		}, value);
	}

	Schema::Value ScriptValueToSchemaValue(ScriptBindingContext& bindings, const Schema::FieldSchema& field,
		const ScriptValue& value)
	{
		switch (field.K)
		{
		case Schema::Kind::Bool:
			return Schema::Value(RequireBool(field, value));
		case Schema::Kind::Int8:
			return Schema::Value(RequireInteger<int8_t>(field, value));
		case Schema::Kind::Int16:
			return Schema::Value(RequireInteger<int16_t>(field, value));
		case Schema::Kind::Int32:
			return Schema::Value(RequireInteger<int32_t>(field, value));
		case Schema::Kind::Int64:
			return Schema::Value(RequireInteger<int64_t>(field, value));
		case Schema::Kind::UInt8:
			return Schema::Value(RequireInteger<uint8_t>(field, value));
		case Schema::Kind::UInt16:
			return Schema::Value(RequireInteger<uint16_t>(field, value));
		case Schema::Kind::UInt32:
			return Schema::Value(RequireInteger<uint32_t>(field, value));
		case Schema::Kind::UInt64:
			return Schema::Value(RequireInteger<uint64_t>(field, value));
		case Schema::Kind::Float:
			return Schema::Value(static_cast<float>(RequireFiniteNumber(field, value)));
		case Schema::Kind::Double:
			return Schema::Value(RequireFiniteNumber(field, value));
		case Schema::Kind::Vec2:
			return Schema::Value(RequireMathUserdata<glm::vec2>(bindings, field, value, "vec2"));
		case Schema::Kind::Vec3:
			return Schema::Value(RequireMathUserdata<glm::vec3>(bindings, field, value, "vec3"));
		case Schema::Kind::Vec4:
			return Schema::Value(RequireMathUserdata<glm::vec4>(bindings, field, value, "vec4"));
		case Schema::Kind::Mat3:
			return Schema::Value(RequireMathUserdata<glm::mat3>(bindings, field, value, "mat3"));
		case Schema::Kind::Mat4:
			return Schema::Value(RequireMathUserdata<glm::mat4>(bindings, field, value, "mat4"));
		case Schema::Kind::String:
			return Schema::Value(RequireString(field, value, "a string"));
		case Schema::Kind::Asset:
			return Schema::Value(RequireString(field, value, "an asset path string"));
		case Schema::Kind::Enum:
		{
			const Schema::EnumSchema* schema = field.GetEnum ? field.GetEnum() : nullptr;
			std::string name;
			if (value.AsString(&name))
			{
				if (!schema)
					throw std::logic_error("field '" + field.Name + "' has no enum metadata");
				for (const auto& [enumName, enumValue] : schema->Values)
					if (enumName == name)
						return schema->IsSigned
							? Schema::Value(static_cast<int64_t>(enumValue))
							: Schema::Value(static_cast<uint64_t>(enumValue));
				throw std::logic_error("field '" + field.Name + "' has no enum name '" + name +
					"'; known names: " + DescribeEnumNames(*schema));
			}
			const int64_t number = RequireInteger<int64_t>(field, value);
			if (schema && !schema->Values.empty() && !schema->FindName(number))
				throw std::logic_error("field '" + field.Name + "' has no enum value " + std::to_string(number) +
					"; known names: " + DescribeEnumNames(*schema));
			return schema && !schema->IsSigned
				? Schema::Value(static_cast<uint64_t>(number))
				: Schema::Value(number);
		}
		default:
			throw std::logic_error("field '" + field.Name + "' has schema kind '" + SchemaKindName(field.K) +
				"' which has no script mapping yet");
		}
	}

	bool RegisterComponentProxyBinding(ScriptBindingContext& bindings, std::string* error)
	{
		const ScriptMetaMethodBinding metaMethods[] = {
			{ "__index", &ComponentProxyIndex },
			{ "__newindex", &ComponentProxyNewIndex },
			{ "__tostring", &ComponentProxyToString },
		};

		ScriptUserTypeDesc desc;
		desc.Name = ComponentProxyLuaTypeName;
		desc.UserdataSize = sizeof(ComponentProxy);
		desc.MetaMethods = metaMethods;
		desc.MetaMethodCount = sizeof(metaMethods) / sizeof(metaMethods[0]);
		// 载荷里有 Entity(std::weak_ptr) 与 std::string:必须登记析构,否则每次 GC 都泄漏。
		desc.Destructor = [](void* data) { static_cast<ComponentProxy*>(data)->~ComponentProxy(); };
		return bindings.RegisterUserType(desc, error);
	}

	ScriptValue MakeComponentProxy(ScriptBindingContext& bindings, const Entity& entity,
		const Schema::TypeSchema& type)
	{
		// 先用注册表确认类型存在,再分配 userdata:分配之后的 Unwrap 不可能失败,
		// 因此不会出现"占位 userdata 从未构造却被析构"的路径。
		if (bindings.UserTypeSize(ComponentProxyLuaTypeName) != sizeof(ComponentProxy))
			throw std::logic_error("ComponentProxy: the component proxy type is not registered in this VM");

		ComponentProxy payload;
		payload.Owner = entity;
		payload.ComponentId = type.Storage ? type.Storage->ComponentId : 0;
		payload.ComponentName = type.DisplayName.empty() ? type.Id.Name : type.DisplayName;

		const ScriptValue value = bindings.NewUserdata(ComponentProxyLuaTypeName);
		ComponentProxy* target = nullptr;
		if (!bindings.Unwrap<ComponentProxy>(ComponentProxyLuaTypeName, value, &target) || !target)
			throw std::logic_error("ComponentProxy: failed to allocate the component proxy userdata");
		new (target) ComponentProxy(std::move(payload));
		return value;
	}

	// W3a-A2:存根注解 —— 类型走 DescribeScriptField(唯一映射表),标注只描述脚本侧读写规则:
	// 未映射 Kind 是占位(unknown)、Transient 与 ReadOnly 是"脚本可读、写会被拒"的既有语义。
	ScriptFieldAnnotation DescribeScriptFieldAnnotation(const Schema::FieldSchema& field)
	{
		const ScriptFieldMapping mapping = DescribeScriptField(field);
		ScriptFieldAnnotation annotation;
		annotation.LuaType = mapping.LuaTypeName ? mapping.LuaTypeName : "unknown";
		if (!mapping.LuaTypeName)
			annotation.Note = std::string("no script mapping for schema kind '") + SchemaKindName(field.K) + "'";
		if (field.Meta.Transient)
			annotation.Note += annotation.Note.empty() ? "transient" : "; transient";
		if (field.Meta.ReadOnly || mapping.ReadOnly)
			annotation.Note += annotation.Note.empty() ? "read-only" : "; read-only";
		return annotation;
	}
}

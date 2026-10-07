#include "wldpch.h"
#include "World/UI/UiBindingSources.h"

#include "World/Core/StringPool.h"
#include "World/Gameplay/Framework/InputMap.h"
#include "World/Gameplay/Framework/LevelService.h"
#include "World/Gameplay/Framework/SaveService.h"
#include "World/Gameplay/Runtime/GameApp.h"
#include "World/Schema/Schema.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/UI/UiBinding.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// `ecs:` / `service:` / `script:` 解析器。前两者 = 工作包 M26,数据源 = `UiBindingContext`(见头文件);
// `script:` = M38,数据源 = `ScriptEngine`(只读查询口 `ReadScriptValue`,无 VM 时给可读 error)。
//
// `service:` 支持的只读查询(其余 = false + 可读 error;报告里列出未接的):
//   * `service:input.<action>`      → 动作是否按下("1"/"0",`InputService::ActionDown`)
//   * `service:input.axis.<axis>`   → 轴值(文本数值,`InputService::Axis`)
//   * `service:input.players`       → 玩家数
//   * `service:level.state`         → 加载状态名(`LevelLoadStateName`)
//   * `service:level.loading`       → 是否正在加载("1"/"0")
//   * `service:level.primary`       → 主关卡 id
//   * `service:level.pending`       → 排队中的关卡 id
//   * `service:level.error`         → 最近一次加载错误
//   * `service:save.exists`         → 会话是否挂了 SaveService("1"/"0")
//   * `service:save.global.<key>`   → 存档全局块的类型化值(未设置 = 可读 error)
// **未接**(需磁盘 I/O 或会发明语义,留给项目):`save.slots`/`save.list`、任何"改状态"的查询。

namespace
{
	using World::Schema::FieldSchema;
	using World::Schema::Kind;
	using World::Schema::TypeSchema;
	using World::Schema::Value;

	bool Fail(std::string* error, std::string message)
	{
		if (error != nullptr)
			*error = std::move(message);
		return false;
	}

	// ---- 值 → 文本(属性文本协议:bool "true"/"false"、数值十进制、向量逗号分隔)----

	std::string FloatToText(double value)
	{
		// 整数数值不带小数点(1.5 → "1.5",-2.0 → "-2");其余用**经典 locale** 的默认精度
		// (6 位有效数字)⇒ 与进程 locale 无关,逐字节确定。
		const double truncated = value < 0.0 ? std::ceil(value) : std::floor(value);
		if (value == truncated && std::fabs(value) < 1.0e15)
			return std::to_string(static_cast<long long>(value));
		std::ostringstream out;
		out.imbue(std::locale::classic());
		out << value;
		return out.str();
	}

	template <typename T>
	std::string NumericToText(T value)
	{
		if constexpr (std::is_same_v<T, bool>)
			return value ? "true" : "false";
		else if constexpr (std::is_floating_point_v<T>)
			return FloatToText(static_cast<double>(value));
		else
			return std::to_string(static_cast<long long>(value));
	}

	template <typename Vector, std::size_t Count>
	std::string VectorToText(const Vector& value)
	{
		std::string out;
		for (std::size_t index = 0; index < Count; ++index)
		{
			if (index > 0)
				out += ',';
			out += NumericToText(value[static_cast<int>(index)]);
		}
		return out;
	}

	template <typename Item>
	void AppendValueText(const Item& item, std::string& out, bool& ok)
	{
		using ValueType = std::decay_t<Item>;
		if constexpr (std::is_same_v<ValueType, std::monostate>)
		{
			ok = false;   // 未设 = 不猜(调用方报可读错误)
		}
		else if constexpr (std::is_same_v<ValueType, bool> || std::is_arithmetic_v<ValueType>)
		{
			out = NumericToText(item);
		}
		else if constexpr (std::is_same_v<ValueType, glm::vec2>) out = VectorToText<glm::vec2, 2>(item);
		else if constexpr (std::is_same_v<ValueType, glm::vec3>) out = VectorToText<glm::vec3, 3>(item);
		else if constexpr (std::is_same_v<ValueType, glm::vec4>) out = VectorToText<glm::vec4, 4>(item);
		else if constexpr (std::is_same_v<ValueType, glm::ivec2>) out = VectorToText<glm::ivec2, 2>(item);
		else if constexpr (std::is_same_v<ValueType, glm::ivec3>) out = VectorToText<glm::ivec3, 3>(item);
		else if constexpr (std::is_same_v<ValueType, glm::ivec4>) out = VectorToText<glm::ivec4, 4>(item);
		else if constexpr (std::is_same_v<ValueType, glm::uvec2>) out = VectorToText<glm::uvec2, 2>(item);
		else if constexpr (std::is_same_v<ValueType, glm::uvec3>) out = VectorToText<glm::uvec3, 3>(item);
		else if constexpr (std::is_same_v<ValueType, glm::uvec4>) out = VectorToText<glm::uvec4, 4>(item);
		else if constexpr (std::is_same_v<ValueType, glm::quat>) out = VectorToText<glm::quat, 4>(item);
		else if constexpr (std::is_same_v<ValueType, glm::mat3> || std::is_same_v<ValueType, glm::mat4>)
		{
			const int dimension = std::is_same_v<ValueType, glm::mat3> ? 3 : 4;
			out.clear();
			for (int column = 0; column < dimension; ++column)
			{
				for (int row = 0; row < dimension; ++row)
				{
					if (!out.empty())
						out += ',';
					out += FloatToText(static_cast<double>(item[column][row]));
				}
			}
		}
		else if constexpr (std::is_same_v<ValueType, std::string>)
		{
			out = item;
		}
		else if constexpr (std::is_same_v<ValueType, World::Schema::ValueList>)
		{
			out.clear();
			for (std::size_t index = 0; index < item.size(); ++index)
			{
				std::string element;
				bool elementOk = true;
				std::visit([&](const auto& nested) { AppendValueText(nested, element, elementOk); }, item[index]);
				if (!elementOk)
				{
					ok = false;
					return;
				}
				if (index > 0)
					out += ';';
				out += element;
			}
		}
		else if constexpr (std::is_same_v<ValueType, World::Schema::ValueMap>)
		{
			out.clear();
			for (const auto& [key, nested] : item)
			{
				std::string element;
				bool elementOk = true;
				std::visit([&](const auto& inner) { AppendValueText(inner, element, elementOk); }, nested);
				if (!elementOk)
				{
					ok = false;
					return;
				}
				if (!out.empty())
					out += ';';
				out += key;
				out += '=';
				out += element;
			}
		}
		else
		{
			ok = false;   // 未覆盖的替代项:不猜,由调用方报可读错误
		}
	}

	bool ValueToText(const Value& value, std::string& out)
	{
		bool ok = true;
		std::string text;
		std::visit([&](const auto& item) { AppendValueText(item, text, ok); }, value);
		if (!ok)
			return false;
		out = std::move(text);
		return true;
	}

	// ---- schema 反射入口(唯一:SchemaRegistry 查类型 + FieldSchema::Get 读值)----

	const FieldSchema* FindFieldByName(const TypeSchema& schema, std::string_view name)
	{
		for (const FieldSchema& field : schema.Fields)
		{
			if (field.Name == name)
				return &field;
			if (!field.Meta.DisplayName.empty() && field.Meta.DisplayName == name)
				return &field;
		}
		return nullptr;
	}

	bool IsDecimalHandle(const std::string& text)
	{
		if (text.empty())
			return false;
		for (const char character : text)
		{
			if (character < '0' || character > '9')
				return false;
		}
		return true;
	}

	entt::entity FindEntityByName(const entt::registry& registry, const std::string& name)
	{
		for (const entt::entity handle : registry.view<World::TagComponent>())
		{
			if (World::StringPool::Get().NameOf(registry.get<World::TagComponent>(handle).Tag) == name)
				return handle;
		}
		return entt::null;
	}

	// `<Entity>` = Tag 名(优先),或十进制实体句柄(与编辑器 AI 通道 `scene.get handle=` 同一口径)。
	entt::entity ResolveEntity(const entt::registry& registry, const std::string& key)
	{
		if (IsDecimalHandle(key))
		{
			const auto handle = static_cast<entt::entity>(std::strtoul(key.c_str(), nullptr, 10));
			if (registry.valid(handle))
				return handle;
		}
		return FindEntityByName(registry, key);
	}

	// 首个带该组件存储的实体(entt 存储迭代序 = packed 顺序;同一帧内确定,删除实体后顺序可变)。
	entt::entity FirstEntityWithComponent(const entt::registry& registry, uint32_t componentId)
	{
		const auto* storage = registry.storage(componentId);
		if (storage != nullptr)
		{
			for (const entt::entity handle : *storage)
				return handle;
		}
		return entt::null;
	}

	bool ResolveEcsBinding(const World::UI::UiBindingSource& source, void* context, std::string& out,
		std::string* error)
	{
		const auto* bindingContext = static_cast<const World::UI::UiBindingContext*>(context);
		if (bindingContext == nullptr || bindingContext->Scene == nullptr)
			return Fail(error, "ecs binding needs a scene (UiBindingContext.Scene is null)");

		const World::Scene& scene = *bindingContext->Scene;
		const entt::registry& registry = scene.GetRegistry();
		const World::Schema::SchemaRegistry& schemas = scene.GetContext().Schemas();

		if (source.Parts.size() != 2 && source.Parts.size() != 3)
			return Fail(error, "ecs binding expects '<Entity>/<Component>/<Field>' or '<Component>/<Field>'");

		const bool hasEntity = source.Parts.size() == 3;
		const std::string entityKey = hasEntity ? source.Parts[0] : std::string();
		const std::string& componentName = source.Parts[hasEntity ? 1 : 0];
		const std::string& fieldName = source.Parts[hasEntity ? 2 : 1];

		const TypeSchema* schema = schemas.Find(componentName);
		if (schema == nullptr)
			return Fail(error, "no component type '" + componentName +
				"' (use the full type id or its short name)");
		if (schema->Storage == nullptr || schema->Storage->ComponentId == 0)
			return Fail(error, "component '" + schema->Id.Name + "' has no storage (schema-only type)");

		entt::entity handle = entt::null;
		if (hasEntity)
		{
			handle = ResolveEntity(registry, entityKey);
			if (handle == entt::null)
				return Fail(error, "no entity named '" + entityKey +
					"' (name = Tag, or a decimal entity handle)");
		}
		else
		{
			handle = FirstEntityWithComponent(registry, schema->Storage->ComponentId);
			if (handle == entt::null)
				return Fail(error, "no entity has component '" + schema->Id.Name + "'");
		}

		const auto* storage = registry.storage(schema->Storage->ComponentId);
		const void* instance = (storage != nullptr && storage->contains(handle))
			? storage->value(handle) : nullptr;
		if (instance == nullptr)
			return Fail(error, "entity " + std::to_string(static_cast<uint32_t>(handle)) +
				" has no component '" + schema->Id.Name + "'");

		const FieldSchema* field = FindFieldByName(*schema, fieldName);
		if (field == nullptr)
			return Fail(error, "component '" + schema->Id.Name + "' has no field '" + fieldName + "'");
		if (field->Get == nullptr)
			return Fail(error, "field '" + field->Name + "' has no reader (non-leaf or unregistered accessor)");

		const Value value = field->Get(instance);
		if (std::holds_alternative<std::monostate>(value))
			return Fail(error, "field '" + field->Name + "' is unset (transient or missing accessor)");

		// 枚举读成名字(比裸数值可读);没有枚举 schema 时退回数值。
		if (field->K == Kind::Enum && field->GetEnum != nullptr)
		{
			if (const int64_t* numeric = std::get_if<int64_t>(&value))
			{
				if (const char* name = field->GetEnum()->FindName(*numeric))
				{
					out = name;
					return true;
				}
			}
		}

		if (!ValueToText(value, out))
			return Fail(error, "field '" + field->Name + "' has a value kind unsupported by text binding");
		return true;
	}

	// `RegisterServiceBindingResolver` 登记的会话回退源;解析时优先用 `UiBindingContext::App`。
	World::Gameplay::GameApp* g_ServiceApp = nullptr;

	bool ResolveServiceBinding(const World::UI::UiBindingSource& source, void* context, std::string& out,
		std::string* error)
	{
		using namespace World::Gameplay;

		const auto* bindingContext = static_cast<const World::UI::UiBindingContext*>(context);
		GameApp* app = bindingContext != nullptr ? bindingContext->App : nullptr;
		if (app == nullptr)
			app = g_ServiceApp;
		if (app == nullptr)
			return Fail(error, "service binding needs an active GameApp session");

		const std::string& query = source.Text;

		// ---- service:input.* ----
		if (query == "input.players")
		{
			out = std::to_string(app->Input().GetPlayerCount());
			return true;
		}
		if (query.rfind("input.axis.", 0) == 0)
		{
			out = FloatToText(static_cast<double>(app->Input().Axis(query.substr(11))));
			return true;
		}
		if (query.rfind("input.", 0) == 0)
		{
			out = app->Input().ActionDown(query.substr(6)) ? "1" : "0";
			return true;
		}

		// ---- service:level.* ----
		if (query == "level.state")
		{
			out = LevelLoadStateName(app->Levels().GetState());
			return true;
		}
		if (query == "level.loading")
		{
			out = app->Levels().IsLoading() ? "1" : "0";
			return true;
		}
		if (query == "level.primary")
		{
			out = app->Levels().GetPrimaryLevel();
			return true;
		}
		if (query == "level.pending")
		{
			out = app->Levels().GetPendingLevelId();
			return true;
		}
		if (query == "level.error")
		{
			out = app->Levels().GetLastError();
			return true;
		}

		// ---- service:save.* ----
		if (query == "save.exists")
		{
			out = app->Saves() != nullptr ? "1" : "0";
			return true;
		}
		if (query.rfind("save.global.", 0) == 0)
		{
			const std::string key = query.substr(12);
			const SaveService* saves = app->Saves();
			if (saves == nullptr)
				return Fail(error, "service '" + query + "': this session has no SaveService");
			SaveService::GlobalValue value;
			if (!saves->TryGetGlobal(key, &value))
				return Fail(error, "service '" + query + "': save global '" + key + "' is not set");
			switch (value.Type)
			{
			case SaveService::GlobalValue::Kind::Int: out = std::to_string(value.Int); break;
			case SaveService::GlobalValue::Kind::Float: out = FloatToText(value.Float); break;
			case SaveService::GlobalValue::Kind::Bool: out = value.Bool ? "true" : "false"; break;
			case SaveService::GlobalValue::Kind::String: out = value.String; break;
			}
			return true;
		}

		return Fail(error, "unknown service query 'service:" + query +
			"' (supported: input.<action>, input.axis.<axis>, input.players, level.state, level.loading, "
			"level.primary, level.pending, level.error, save.exists, save.global.<key>)");
	}

	// `RegisterScriptBindingResolver` 登记的 `script:` 解析器(M38)。源 = `script:<路径>.<字段>`,
	// `UiBinding::Parse` 已按最后一个 '.' 切成 Parts = [路径, 字段]。求值**只读**:
	// 走 `ScriptEngine::ReadScriptValue`(load 脚本模块 + 读返回表字段 → 属性文本协议)。
	// 失败(VM 未初始化 / 路径不存在 / 字段不存在 / 值不可转文本)⇒ false + 可读 error。
	bool ResolveScriptBinding(const World::UI::UiBindingSource& source, void* /*context*/,
		std::string& out, std::string* error)
	{
		if (source.Parts.size() != 2)
			return Fail(error, "script binding '" + source.Text +
				"': expected 'script:<path>.<field>' (a script path and a field name)");
		return World::ScriptEngine::ReadScriptValue(source.Parts[0], source.Parts[1], out, error);
	}
}

namespace World::UI
{
	void RegisterEcsBindingResolver()
	{
		// 同名重复注册 = 覆盖(见 UiBinding::RegisterResolver)⇒ 幂等。
		UiBinding::RegisterResolver("ecs", &ResolveEcsBinding);
	}

	void RegisterServiceBindingResolver(Gameplay::GameApp& app)
	{
		g_ServiceApp = &app;
		UiBinding::RegisterResolver("service", &ResolveServiceBinding);
	}

	void RegisterScriptBindingResolver()
	{
		// 同名重复注册 = 覆盖 ⇒ 幂等;默认实现固定接 ScriptEngine(无 VM ⇒ 可读 error)。
		UiBinding::RegisterResolver("script", &ResolveScriptBinding);
	}

	void RegisterBuiltinBindingSources(Gameplay::GameApp* app)
	{
		RegisterEcsBindingResolver();
		RegisterScriptBindingResolver();
		// service 解析器总是注册:没有会话时返回可读 error("needs an active GameApp session"),
		// 比"no resolver registered"更能说明问题。`app == nullptr` = 清掉上次登记的回退源。
		g_ServiceApp = app;
		UiBinding::RegisterResolver("service", &ResolveServiceBinding);
	}
}

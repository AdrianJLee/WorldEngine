#include "wldpch.h"

#include "World/Ai/SceneAuthoring.h"

#include "World/Ai/CommandRegistry.h"
#include "World/Core/UUID.h"
#include "World/Core/WorldContext.h"
#include "World/Level/Authoring/LevelPackage.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/SceneSerializer.h"
#include "World/Schema/Schema.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiJson.h"

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace World::Ai
{
	namespace fs = std::filesystem;
	using namespace World::Wui;

	namespace
	{
		std::string Arg(const std::map<std::string, std::string>& args, const char* key)
		{
			const auto it = args.find(key);
			return it == args.end() ? std::string() : it->second;
		}

		bool ParseFloats(const std::string& text, std::vector<float>& out)
		{
			std::string cleaned = text;
			for (char& c : cleaned)
			{
				if (c == '[' || c == ']' || c == '(' || c == ')')
					c = ' ';
			}
			std::stringstream ss(cleaned);
			std::string token;
			while (std::getline(ss, token, ','))
			{
				if (token.empty() || token.find_first_not_of(" \t") == std::string::npos)
					continue;
				try { out.push_back(std::stof(token)); }
				catch (...) { return false; }
			}
			return !out.empty();
		}

		bool ParseVec3(const std::string& text, glm::vec3& out)
		{
			std::vector<float> values;
			if (!ParseFloats(text, values) || values.size() != 3)
				return false;
			out = glm::vec3 { values[0], values[1], values[2] };
			return true;
		}

		JsonValue JsonFromSchemaValue(const Schema::Value& value);

		JsonValue JsonFromList(const Schema::ValueList& list)
		{
			JsonValue arr;
			arr.type = JsonValue::Type::Array;
			for (const auto& item : list)
				arr.Array.push_back(JsonFromSchemaValue(item));
			return arr;
		}

		JsonValue JsonFromMap(const Schema::ValueMap& map)
		{
			JsonValue obj;
			obj.type = JsonValue::Type::Object;
			for (const auto& kv : map)
				obj.Object.push_back({ kv.first, JsonFromSchemaValue(kv.second) });
			return obj;
		}
		JsonValue JsonFromSchemaValue(const Schema::Value& value)
		{
			if (std::holds_alternative<std::monostate>(value))
				return JsonValue::Null();
			if (const auto* v = std::get_if<bool>(&value)) return JsonValue::MakeBool(*v);
			if (const auto* v = std::get_if<int8_t>(&value)) return JsonValue::MakeNumber(*v);
			if (const auto* v = std::get_if<int16_t>(&value)) return JsonValue::MakeNumber(*v);
			if (const auto* v = std::get_if<int32_t>(&value)) return JsonValue::MakeNumber(*v);
			if (const auto* v = std::get_if<int64_t>(&value)) return JsonValue::MakeNumber(static_cast<double>(*v));
			if (const auto* v = std::get_if<uint8_t>(&value)) return JsonValue::MakeNumber(*v);
			if (const auto* v = std::get_if<uint16_t>(&value)) return JsonValue::MakeNumber(*v);
			if (const auto* v = std::get_if<uint32_t>(&value)) return JsonValue::MakeNumber(*v);
			if (const auto* v = std::get_if<uint64_t>(&value)) return JsonValue::MakeNumber(static_cast<double>(*v));
			if (const auto* v = std::get_if<float>(&value)) return JsonValue::MakeNumber(*v);
			if (const auto* v = std::get_if<double>(&value)) return JsonValue::MakeNumber(*v);
			if (const auto* v = std::get_if<std::string>(&value)) return JsonValue::MakeString(*v);
			if (const auto* v = std::get_if<glm::vec2>(&value))
			{
				JsonValue arr; arr.type = JsonValue::Type::Array;
				arr.Array.push_back(JsonValue::MakeNumber(v->x));
				arr.Array.push_back(JsonValue::MakeNumber(v->y));
				return arr;
			}
			if (const auto* v = std::get_if<glm::vec3>(&value))
			{
				JsonValue arr; arr.type = JsonValue::Type::Array;
				arr.Array.push_back(JsonValue::MakeNumber(v->x));
				arr.Array.push_back(JsonValue::MakeNumber(v->y));
				arr.Array.push_back(JsonValue::MakeNumber(v->z));
				return arr;
			}
			if (const auto* v = std::get_if<glm::vec4>(&value))
			{
				JsonValue arr; arr.type = JsonValue::Type::Array;
				for (int i = 0; i < 4; ++i) arr.Array.push_back(JsonValue::MakeNumber((*v)[i]));
				return arr;
			}
			if (const auto* v = std::get_if<glm::quat>(&value))
			{
				JsonValue arr; arr.type = JsonValue::Type::Array;
				arr.Array.push_back(JsonValue::MakeNumber(v->x));
				arr.Array.push_back(JsonValue::MakeNumber(v->y));
				arr.Array.push_back(JsonValue::MakeNumber(v->z));
				arr.Array.push_back(JsonValue::MakeNumber(v->w));
				return arr;
			}
			if (const auto* v = std::get_if<Schema::ValueList>(&value))
				return JsonFromList(*v);
			if (const auto* v = std::get_if<Schema::ValueMap>(&value))
				return JsonFromMap(*v);
			return JsonValue::Null();
		}

		bool ParseSchemaValue(const Schema::FieldSchema& field, const std::string& text,
			Schema::Value& out, std::string& error)
		{
			auto parseInt = [&](auto dummy) -> bool
			{
				using T = decltype(dummy);
				try { out = static_cast<T>(std::stoll(text)); return true; }
				catch (...) { error = "value must be an integer (got '" + text + "')"; return false; }
			};
			auto parseUInt = [&](auto dummy) -> bool
			{
				using T = decltype(dummy);
				try { out = static_cast<T>(std::stoull(text)); return true; }
				catch (...) { error = "value must be an unsigned integer (got '" + text + "')"; return false; }
			};

			switch (field.K)
			{
			case Schema::Kind::Bool:
				if (text == "true" || text == "1") { out = true; return true; }
				if (text == "false" || text == "0") { out = false; return true; }
				error = "value must be true/false";
				return false;
			case Schema::Kind::Int8: return parseInt(int8_t {});
			case Schema::Kind::Int16: return parseInt(int16_t {});
			case Schema::Kind::Int32: return parseInt(int32_t {});
			case Schema::Kind::Int64: return parseInt(int64_t {});
			case Schema::Kind::UInt8: return parseUInt(uint8_t {});
			case Schema::Kind::UInt16: return parseUInt(uint16_t {});
			case Schema::Kind::UInt32: return parseUInt(uint32_t {});
			case Schema::Kind::UInt64: return parseUInt(uint64_t {});
			case Schema::Kind::Float:
			case Schema::Kind::Double:
			{
				try
				{
					const double parsed = std::stod(text);
					out = (field.K == Schema::Kind::Float) ? Schema::Value(static_cast<float>(parsed)) : Schema::Value(parsed);
					return true;
				}
				catch (...) { error = "value must be a number (got '" + text + "')"; return false; }
			}
			case Schema::Kind::Vec2:
			case Schema::Kind::Vec3:
			case Schema::Kind::Vec4:
			{
				std::vector<float> values;
				if (!ParseFloats(text, values))
				{
					error = "value must be 'x,y,z' (got '" + text + "')";
					return false;
				}
				const size_t expected = field.K == Schema::Kind::Vec2 ? 2 : (field.K == Schema::Kind::Vec3 ? 3 : 4);
				if (values.size() != expected)
				{
					error = "value needs " + std::to_string(expected) + " components";
					return false;
				}
				if (field.K == Schema::Kind::Vec2) out = glm::vec2 { values[0], values[1] };
				else if (field.K == Schema::Kind::Vec3) out = glm::vec3 { values[0], values[1], values[2] };
				else out = glm::vec4 { values[0], values[1], values[2], values[3] };
				return true;
			}
			case Schema::Kind::Quat:
			{
				std::vector<float> values;
				if (!ParseFloats(text, values) || values.size() != 4)
				{
					error = "value must be 'x,y,z,w'";
					return false;
				}
				out = glm::quat { values[3], values[0], values[1], values[2] };
				return true;
			}
			case Schema::Kind::String:
			case Schema::Kind::Name:
			case Schema::Kind::Text:
			case Schema::Kind::Asset:
			{
				std::string value = text;
				if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
					value = value.substr(1, value.size() - 2);
				out = value;
				return true;
			}
			case Schema::Kind::Enum:
			{
				const Schema::EnumSchema* enumSchema = field.GetEnum();
				if (enumSchema)
				{
					for (const auto& entry : enumSchema->Values)
					{
						if (entry.first == text)
						{
							if (enumSchema->IsSigned) out = static_cast<int64_t>(entry.second);
							else out = static_cast<uint64_t>(entry.second);
							return true;
						}
					}
				}
				try
				{
					if (enumSchema && !enumSchema->IsSigned)
						out = static_cast<uint64_t>(std::stoull(text));
					else
						out = static_cast<int64_t>(std::stoll(text));
					return true;
				}
				catch (...)
				{
					error = "value must be a valid enum name or integer (got '" + text + "')";
					return false;
				}
			}
			default:
				error = "field kind does not support text assignment yet";
				return false;
			}
		}

		const Schema::TypeSchema* FindSchema(Schema::SchemaRegistry& schemas, const std::string& name)
		{
			if (const auto* found = schemas.Find(name))
				return found;
			for (const auto* schema : schemas.List(Schema::TypeCategory::Component))
			{
				if (!schema)
					continue;
				if (schema->Id.Name == name || schema->DisplayName == name)
					return schema;
				const size_t sep = schema->Id.Name.rfind("::");
				if (sep != std::string::npos && schema->Id.Name.substr(sep + 2) == name)
					return schema;
			}
			return nullptr;
		}

		const Schema::FieldSchema* FindField(const Schema::TypeSchema& schema, const std::string& name)
		{
			for (const auto& field : schema.Fields)
			{
				if (field.Name == name || field.Meta.DisplayName == name)
					return &field;
			}
			return nullptr;
		}

		void AppendComponentJson(JsonValue& components, const Schema::TypeSchema& schema, const void* instance)
		{
			const Schema::Value values = Schema::ReadStructValue(schema, instance);
			JsonValue obj;
			obj.type = JsonValue::Type::Object;
			if (const auto* map = std::get_if<Schema::ValueMap>(&values))
			{
				for (const auto& kv : *map)
					obj.Object.push_back({ kv.first, JsonFromSchemaValue(kv.second) });
			}
			components.Object.push_back({ schema.Id.Name, std::move(obj) });
		}

		JsonValue EntitySummary(Scene& scene, entt::entity handle)
		{
			JsonValue obj;
			obj.type = JsonValue::Type::Object;
			obj.Object.push_back({ "handle", JsonValue::MakeNumber(static_cast<uint32_t>(handle)) });

			const auto& registry = scene.GetRegistry();
			if (const auto* tag = registry.try_get<TagComponent>(handle))
				obj.Object.push_back({ "name", JsonValue::MakeString(StringPool::Get().NameOf(tag->Tag)) });
			if (const auto* uuid = registry.try_get<UUIDComponent>(handle))
				obj.Object.push_back({ "uuid", JsonValue::MakeString(std::to_string(static_cast<uint64_t>(uuid->ID))) });
			return obj;
		}
	}

	SceneAuthoring& SceneAuthoring::Get()
	{
		static SceneAuthoring instance;
		return instance;
	}

	void SceneAuthoring::Close()
	{
		m_Scene.reset();
		m_Package = Level::LevelPackage {};
		m_EntryPath.clear();
		m_Dirty = false;
	}

	std::string SceneAuthoring::ResolveEntryPath(const std::string& path) const
	{
		if (path.empty())
			return std::string();

		std::vector<fs::path> candidates;
		const fs::path raw = fs::u8path(path);
		if (raw.is_absolute())
			candidates.push_back(raw);
		else
		{
			candidates.push_back(Paths::ProjectDir() / raw);
			candidates.push_back(Paths::ProjectDir() / "assets" / raw);
			candidates.push_back(Paths::ProjectDir() / "assets" / "scenes" / raw);
			candidates.push_back(fs::current_path() / raw);
		}

		for (const fs::path& candidate : candidates)
		{
			if (fs::exists(candidate) && fs::is_regular_file(candidate))
				return candidate.string();
		}
		if (!raw.has_extension())
		{
			for (const fs::path& candidate : candidates)
			{
				fs::path withExt = candidate;
				withExt += ".wlevel";
				if (fs::exists(withExt) && fs::is_regular_file(withExt))
					return withExt.string();
			}
		}
		return std::string();
	}

	bool SceneAuthoring::Open(const std::string& path, std::string* outError)
	{
		const std::string resolved = ResolveEntryPath(path);
		if (resolved.empty())
		{
			if (outError) *outError = "level entry not found: '" + path + "' (expected assets/scenes/<Id>.wlevel)";
			return false;
		}

		auto context = std::make_unique<WorldContext>();
		Level::LevelPackage package;
		std::string loadError;
		if (!package.Load(fs::u8path(resolved), &loadError))
		{
			if (outError) *outError = "failed to load level package '" + resolved + "': " + loadError;
			return false;
		}

		Ref<Scene> scene = package.Instantiate(*context);
		if (!scene)
		{
			if (outError) *outError = "failed to instantiate level scene from '" + resolved + "'";
			return false;
		}

		m_Context = std::move(context);
		m_Package = std::move(package);
		m_Scene = std::move(scene);
		m_EntryPath = resolved;
		m_Dirty = false;
		return true;
	}

	bool SceneAuthoring::CreateLevel(const std::string& path, const std::string& id, std::string* outError)
	{
		const std::string levelId = id.empty() ? fs::u8path(path).stem().string() : id;
		if (levelId.empty())
		{
			if (outError) *outError = "level id is required";
			return false;
		}

		const fs::path given = fs::u8path(path);
		fs::path entry;
		if (given.is_absolute())
			entry = given;
		else if (given.parent_path().empty())
			entry = Paths::AssetRoot() / "scenes" / given;
		else
			entry = Paths::ProjectDir() / given;
		if (!entry.has_extension())
			entry += ".wlevel";

		std::error_code ec;
		fs::create_directories(entry.parent_path(), ec);

		auto context = std::make_unique<WorldContext>();
		std::string createError;
		if (!Level::LevelPackage::CreateEmpty(*context, entry, levelId, &createError))
		{
			if (outError) *outError = createError.empty() ? "failed to create level" : createError;
			return false;
		}
		return Open(entry.string(), outError);
	}

	bool SceneAuthoring::Save(std::string* outError)
	{
		if (!m_Scene || m_EntryPath.empty())
		{
			if (outError) *outError = "no active level; call scene.open or scene.create_level first";
			return false;
		}

		const fs::path entry = fs::u8path(m_EntryPath);
		m_Package.SetEntryPath(entry);
		const std::string levelName = m_Package.GetName().empty() ? m_Package.GetId() : m_Package.GetName();
		const uint32_t grid = m_Package.GetGridCellSize() == 0 ? 64u : m_Package.GetGridCellSize();
		m_Package.CaptureFromScene(*m_Scene, levelName, grid);

		std::string saveError;
		if (!m_Package.Save(entry, &saveError))
		{
			if (outError) *outError = "failed to save level '" + entry.string() + "': " + saveError;
			return false;
		}
		m_Dirty = false;
		return true;
	}

	Entity SceneAuthoring::ResolveEntity(const std::map<std::string, std::string>& args, std::string* outError)
	{
		if (!m_Scene)
		{
			if (outError) *outError = "no active level; call scene.open first";
			return Entity {};
		}

		const std::string handleText = Arg(args, "handle");
		const std::string uuidText = Arg(args, "uuid");
		const std::string nameText = Arg(args, "name");
		auto& registry = m_Scene->GetRegistry();

		if (!handleText.empty())
		{
			const auto handle = static_cast<entt::entity>(std::strtoul(handleText.c_str(), nullptr, 10));
			if (registry.valid(handle) && registry.all_of<TagComponent>(handle))
				return Entity(m_Scene.get(), handle);
			if (outError) *outError = "entity handle not found: " + handleText;
			return Entity {};
		}

		if (!uuidText.empty())
		{
			try
			{
				const uint64_t wanted = std::stoull(uuidText);
				for (auto handle : registry.view<UUIDComponent>())
				{
					if (static_cast<uint64_t>(registry.get<UUIDComponent>(handle).ID) == wanted)
						return Entity(m_Scene.get(), handle);
				}
			}
			catch (...) {}
			if (outError) *outError = "entity uuid not found: " + uuidText;
			return Entity {};
		}

		if (!nameText.empty())
		{
			std::vector<entt::entity> matches;
			for (auto handle : registry.view<TagComponent>())
			{
				if (StringPool::Get().NameOf(registry.get<TagComponent>(handle).Tag) == nameText)
					matches.push_back(handle);
			}
			if (matches.size() == 1)
				return Entity(m_Scene.get(), matches.front());
			if (matches.empty())
				if (outError) *outError = "entity name not found: " + nameText;
			if (matches.size() > 1)
				if (outError) *outError = "entity name is ambiguous (" + std::to_string(matches.size()) + " matches); use handle or uuid";
			return Entity {};
		}

		if (outError) *outError = "entity selector required: handle=<id>, uuid=<id> or name=<Tag>";
		return Entity {};
	}

	void SceneAuthoring::RegisterCommands(CommandRegistry& reg)
	{
		using Handler = std::function<bool(const std::map<std::string, std::string>&, std::string&, std::string&)>;
		auto add = [&](const char* name, const char* description, CommandEffect effect, const char* params, Handler handler)
		{
			CommandDescriptor desc;
			desc.name = name;
			desc.description = description;
			desc.effect = effect;
			desc.scope = CommandScope::Both;
			desc.owner = "engine";
			desc.paramsSchema = params;
			desc.resultSchema = "{\"type\":\"object\"}";
			desc.handler = std::move(handler);
			reg.RegisterCommand(std::move(desc));
		};
		auto addWrite = [&](const char* name, const char* description, const char* params, Handler handler)
		{
			add(name, description, CommandEffect::Write, params, std::move(handler));
		};
		auto addRead = [&](const char* name, const char* description, const char* params, Handler handler)
		{
			add(name, description, CommandEffect::Read, params, std::move(handler));
		};

		addWrite("scene.open", "Open a real level package (.wlevel) into the headless authoring scene.",
			R"raw({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})raw",
			[](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
			{
				auto& self = SceneAuthoring::Get();
				if (!self.Open(Arg(args, "path"), &error))
					return false;

				JsonValue out; out.type = JsonValue::Type::Object;
				out.Object.push_back({ "opened", JsonValue::MakeString(self.EntryPath()) });
				out.Object.push_back({ "entity_count", JsonValue::MakeNumber(static_cast<double>(self.GetScene()->GetRegistry().view<TagComponent>().size())) });
				result = out.Dump();
				return true;
			});

		addWrite("scene.create_level", "Create and open a new level package (writes .wlevel + entities on disk).",
			R"raw({"type":"object","properties":{"path":{"type":"string"},"id":{"type":"string"}},"required":["path"]})raw",
			[](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
			{
				auto& self = SceneAuthoring::Get();
				if (!self.CreateLevel(Arg(args, "path"), Arg(args, "id"), &error))
					return false;

				JsonValue out; out.type = JsonValue::Type::Object;
				out.Object.push_back({ "created", JsonValue::MakeString(self.EntryPath()) });
				out.Object.push_back({ "entity_count", JsonValue::MakeNumber(static_cast<double>(self.GetScene()->GetRegistry().view<TagComponent>().size())) });
				result = out.Dump();
				return true;
			});

		addWrite("scene.save", "Save the active headless level (regenerates .wlevel + levels/<Id>/entities/*.entity).",
			R"raw({"type":"object","properties":{}})raw",
			[](const std::map<std::string, std::string>&, std::string& result, std::string& error)
			{
				auto& self = SceneAuthoring::Get();
				if (!self.Save(&error))
					return false;

				JsonValue out; out.type = JsonValue::Type::Object;
				out.Object.push_back({ "saved", JsonValue::MakeString(self.EntryPath()) });
				out.Object.push_back({ "entity_count", JsonValue::MakeNumber(static_cast<double>(self.GetScene()->GetRegistry().view<TagComponent>().size())) });
				result = out.Dump();
				return true;
			});

		addRead("scene.list", "List entities of the active headless level.",
			R"raw({"type":"object","properties":{}})raw",
			[](const std::map<std::string, std::string>&, std::string& result, std::string& error)
			{
				auto& self = SceneAuthoring::Get();
				if (!self.IsOpen()) { error = "no active level; call scene.open first"; return false; }

				JsonValue out; out.type = JsonValue::Type::Object;
				out.Object.push_back({ "entry", JsonValue::MakeString(self.EntryPath()) });
				out.Object.push_back({ "dirty", JsonValue::MakeBool(self.IsDirty()) });
				JsonValue arr; arr.type = JsonValue::Type::Array;
				for (auto handle : self.GetScene()->GetRegistry().view<TagComponent>())
					arr.Array.push_back(EntitySummary(*self.GetScene(), handle));
				out.Object.push_back({ "entities", std::move(arr) });
				result = out.Dump();
				return true;
			});

		addRead("scene.query", "List entities carrying a specific component type.",
			R"raw({"type":"object","properties":{"component":{"type":"string"}},"required":["component"]})raw",
			[](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
			{
				auto& self = SceneAuthoring::Get();
				if (!self.IsOpen()) { error = "no active level; call scene.open first"; return false; }

				const std::string componentName = Arg(args, "component");
				const Schema::TypeSchema* schema = FindSchema(self.GetScene()->GetContext().Schemas(), componentName);
				if (!schema || !schema->Storage) { error = "unknown component type: " + componentName; return false; }

				JsonValue out; out.type = JsonValue::Type::Object;
				out.Object.push_back({ "component", JsonValue::MakeString(schema->Id.Name) });
				JsonValue arr; arr.type = JsonValue::Type::Array;
				auto& registry = self.GetScene()->GetRegistry();
				auto* storage = registry.storage(schema->Storage->ComponentId);
				if (storage)
				{
					for (auto handle : registry.view<TagComponent>())
						if (storage->contains(handle))
							arr.Array.push_back(EntitySummary(*self.GetScene(), handle));
				}
				out.Object.push_back({ "entities", std::move(arr) });
				result = out.Dump();
				return true;
			});

		addRead("scene.get", "Get every schema component of an entity (handle/uuid/name selector).",
			R"raw({"type":"object","properties":{"handle":{"type":"string"},"uuid":{"type":"string"},"name":{"type":"string"}}})raw",
			[](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
			{
				auto& self = SceneAuthoring::Get();
				Entity target = self.ResolveEntity(args, &error);
				if (!target.IsValid())
					return false;

				JsonValue out = EntitySummary(*self.GetScene(), static_cast<entt::entity>(target));
				JsonValue components; components.type = JsonValue::Type::Object;
				auto& registry = self.GetScene()->GetRegistry();
				for (const auto* schema : self.GetScene()->GetContext().Schemas().List(Schema::TypeCategory::Component))
				{
					if (!schema || !schema->Storage)
						continue;
					auto* storage = registry.storage(schema->Storage->ComponentId);
					if (storage && storage->contains(static_cast<entt::entity>(target)))
						AppendComponentJson(components, *schema, storage->value(static_cast<entt::entity>(target)));
				}
				out.Object.push_back({ "components", std::move(components) });
				result = out.Dump();
				return true;
			});

		addWrite("scene.set", "Set a component field on an entity.",
			R"raw({"type":"object","properties":{"handle":{"type":"string"},"uuid":{"type":"string"},"name":{"type":"string"},"component":{"type":"string"},"property":{"type":"string"},"value":{"type":"string"}},"required":["property","value"]})raw",
			[](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
			{
				auto& self = SceneAuthoring::Get();
				Entity target = self.ResolveEntity(args, &error);
				if (!target.IsValid())
					return false;

				const std::string property = Arg(args, "property");
				const std::string valueText = Arg(args, "value");
				const std::string componentName = Arg(args, "component");
				if (property.empty())
				{
					error = "property is required";
					return false;
				}

				auto& registry = self.GetScene()->GetRegistry();
				const entt::entity handle = static_cast<entt::entity>(target);

				if (property == "Tag")
				{
					auto* tag = registry.try_get<TagComponent>(handle);
					if (!tag) { error = "entity has no TagComponent"; return false; }
					tag->Tag = StringPool::Get().InternName(valueText);
					self.MarkDirty();
					result = "Tag='" + valueText + "'";
					return true;
				}

				for (const char* transformField : { "Location", "Rotation", "Scale" })
				{
					if (property != transformField)
						continue;
					glm::vec3 vec { 0.0f };
					if (!ParseVec3(valueText, vec)) { error = "value must be 'x,y,z'"; return false; }
					auto* transform = registry.try_get<TransformComponent>(handle);
					if (!transform) { error = "entity has no TransformComponent"; return false; }
					if (std::string(transformField) == "Location") transform->SetLocation(vec);
					else if (std::string(transformField) == "Rotation") transform->SetRotation(vec);
					else transform->SetScale(vec);
					self.MarkDirty();
					result = property + "=(" + valueText + ")";
					return true;
				}

				const Schema::TypeSchema* schema = nullptr;
				if (!componentName.empty())
				{
					schema = FindSchema(self.GetScene()->GetContext().Schemas(), componentName);
					if (!schema)
					{
						error = "unknown component type: " + componentName;
						return false;
					}
					if (!schema->Storage)
					{
						error = "component '" + schema->Id.Name + "' has no storage";
						return false;
					}
				}
				else
				{
					auto* mesh = registry.try_get<MeshRendererComponent>(handle);
					if (mesh)
					{
						if (property == "Material")
						{
							mesh->Material = AssetRef { valueText.empty() ? PathId() : StringPool::Get().InternPath(valueText), AssetId() };
							self.MarkDirty();
							result = "Material='" + valueText + "'";
							return true;
						}
						if (property == "Primitive")
						{
							try { mesh->Primitive = static_cast<MeshRendererComponent::PrimitiveShape>(std::stoi(valueText)); }
							catch (...) { error = "Primitive must be 0(Cube)/1(Sphere)/2(Plane)"; return false; }
							self.MarkDirty();
							result = "Primitive=" + valueText;
							return true;
						}
					}
					error = "component=<type> is required for property '" + property + "'";
					return false;
				}

				const Schema::FieldSchema* field = FindField(*schema, property);
				if (!field)
				{
					error = "component '" + schema->Id.Name + "' has no field '" + property + "'";
					return false;
				}
				auto* storage = registry.storage(schema->Storage->ComponentId);
				void* instance = storage && storage->contains(handle) ? storage->value(handle) : nullptr;
				if (!instance)
				{
					error = "entity does not have component '" + schema->Id.Name + "' (use scene.add_component first)";
					return false;
				}

				Schema::Value parsed;
				std::string parseError;
				if (!ParseSchemaValue(*field, valueText, parsed, parseError))
				{
					error = parseError;
					return false;
				}
				if (!Schema::WriteSchemaField(*field, instance, parsed))
				{
					error = "schema rejected the value for '" + property + "'";
					return false;
				}
				self.MarkDirty();
				result = schema->Id.Name + "." + field->Name + "=" + JsonFromSchemaValue(parsed).Dump();
				return true;
			});

		addWrite("scene.add_component", "Add a schema component (including plugin components) to an entity.",
			R"raw({"type":"object","properties":{"handle":{"type":"string"},"uuid":{"type":"string"},"name":{"type":"string"},"component":{"type":"string"}},"required":["component"]})raw",
			[](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
			{
				auto& self = SceneAuthoring::Get();
				Entity target = self.ResolveEntity(args, &error);
				if (!target.IsValid())
					return false;

				const std::string componentName = Arg(args, "component");
				const Schema::TypeSchema* schema = FindSchema(self.GetScene()->GetContext().Schemas(), componentName);
				if (!schema || !schema->Storage)
				{
					error = "unknown component type: " + componentName;
					return false;
				}
				if (target.HasComponent(schema->Storage->ComponentId))
				{
					result = "already present: " + schema->Id.Name;
					return true;
				}
				target.AddComponent(schema->Storage->ComponentId);
				self.MarkDirty();
				result = "added " + schema->Id.Name;
				return true;
			});

		addWrite("scene.remove_component", "Remove a non-core schema component from an entity.",
			R"raw({"type":"object","properties":{"handle":{"type":"string"},"uuid":{"type":"string"},"name":{"type":"string"},"component":{"type":"string"}},"required":["component"]})raw",
			[](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
			{
				auto& self = SceneAuthoring::Get();
				Entity target = self.ResolveEntity(args, &error);
				if (!target.IsValid())
					return false;

				const std::string componentName = Arg(args, "component");
				const Schema::TypeSchema* schema = FindSchema(self.GetScene()->GetContext().Schemas(), componentName);
				if (!schema || !schema->Storage)
				{
					error = "unknown component type: " + componentName;
					return false;
				}
				if (schema->Core)
				{
					error = "component '" + schema->Id.Name + "' is core and cannot be removed";
					return false;
				}
				std::string reason;
				if (!target.CanRemoveComponent(schema->Storage->ComponentId, &reason))
				{
					error = "cannot remove component: " + reason;
					return false;
				}
				target.RemoveComponent(schema->Storage->ComponentId);
				self.MarkDirty();
				result = "removed " + schema->Id.Name;
				return true;
			});

		addWrite("scene.create_entity", "Create a real entity with Tag/UUID/Transform in the active level.",
			R"raw({"type":"object","properties":{"name":{"type":"string"},"location":{"type":"string"},"rotation":{"type":"string"},"scale":{"type":"string"}}})raw",
			[](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
			{
				auto& self = SceneAuthoring::Get();
				if (!self.IsOpen()) { error = "no active level; call scene.open first"; return false; }

				std::string name = Arg(args, "name");
				if (name.empty())
					name = "Entity";

				glm::vec3 location { 0.0f };
				glm::vec3 rotation { 0.0f };
				glm::vec3 scale { 1.0f };
				if (!Arg(args, "location").empty() && !ParseVec3(Arg(args, "location"), location)) { error = "location must be 'x,y,z'"; return false; }
				if (!Arg(args, "rotation").empty() && !ParseVec3(Arg(args, "rotation"), rotation)) { error = "rotation must be 'x,y,z'"; return false; }
				if (!Arg(args, "scale").empty() && !ParseVec3(Arg(args, "scale"), scale)) { error = "scale must be 'x,y,z'"; return false; }

				Scene* scene = self.GetScene();
				Entity entity = scene->CreateEntityShell(name);
				entity.AddComponent<TransformComponent>(location, rotation, scale);
				self.MarkDirty();

				result = EntitySummary(*scene, static_cast<entt::entity>(entity)).Dump();
				return true;
			});

		addWrite("scene.delete_entity", "Delete an entity from the active level.",
			R"raw({"type":"object","properties":{"handle":{"type":"string"},"uuid":{"type":"string"},"name":{"type":"string"}}})raw",
			[](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
			{
				auto& self = SceneAuthoring::Get();
				Entity target = self.ResolveEntity(args, &error);
				if (!target.IsValid())
					return false;

				const std::string summary = EntitySummary(*self.GetScene(), static_cast<entt::entity>(target)).Dump();
				Entity::DestroyEntity(self.GetScene(), target);
				self.GetScene()->FlushStructuralChanges();
				self.MarkDirty();
				result = summary;
				return true;
			});
	}
}

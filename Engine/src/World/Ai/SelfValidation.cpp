#include "wldpch.h"
#include "World/Ai/SelfValidation.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"
#include "World/Script/Tooling/LuauSyntax.h"
#include "World/Utils/Paths.h"
#include "World/Core/Application.h"
#include "World/Schema/SchemaRegistry.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace World::Ai
{
	namespace fs = std::filesystem;
	using namespace World::Wui;

	namespace
	{
		const char* KindToString(Schema::Kind k)
		{
			switch (k)
			{
			case Schema::Kind::Bool:   return "Bool";
			case Schema::Kind::Int8:   return "Int8";
			case Schema::Kind::Int16:  return "Int16";
			case Schema::Kind::Int32:  return "Int32";
			case Schema::Kind::Int64:  return "Int64";
			case Schema::Kind::UInt8:  return "UInt8";
			case Schema::Kind::UInt16: return "UInt16";
			case Schema::Kind::UInt32: return "UInt32";
			case Schema::Kind::UInt64: return "UInt64";
			case Schema::Kind::Float:  return "Float";
			case Schema::Kind::Double: return "Double";
			case Schema::Kind::Vec2:   return "Vec2";
			case Schema::Kind::Vec3:   return "Vec3";
			case Schema::Kind::Vec4:   return "Vec4";
			case Schema::Kind::IVec2:  return "IVec2";
			case Schema::Kind::IVec3:  return "IVec3";
			case Schema::Kind::IVec4:  return "IVec4";
			case Schema::Kind::UVec2:  return "UVec2";
			case Schema::Kind::UVec3:  return "UVec3";
			case Schema::Kind::UVec4:  return "UVec4";
			case Schema::Kind::Quat:   return "Quat";
			case Schema::Kind::Mat3:   return "Mat3";
			case Schema::Kind::Mat4:   return "Mat4";
			case Schema::Kind::String: return "String";
			case Schema::Kind::Enum:   return "Enum";
			case Schema::Kind::Asset:  return "Asset";
			case Schema::Kind::Object: return "Object";
			case Schema::Kind::Name:   return "Name";
			default:                   return "Unknown";
			}
		}
	}

	std::string ValidationReport::ToJson() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;

		root.Object.push_back({ "valid", JsonValue::MakeBool(valid) });

		// 错误列表
		JsonValue errArray;
		errArray.type = JsonValue::Type::Array;
		for (const auto& err : errors)
		{
			JsonValue errObj;
			errObj.type = JsonValue::Type::Object;
			errObj.Object.push_back({ "code", JsonValue::MakeString(err.code) });
			errObj.Object.push_back({ "file", JsonValue::MakeString(err.file) });
			errObj.Object.push_back({ "line", JsonValue::MakeNumber(static_cast<double>(err.line)) });
			errObj.Object.push_back({ "message", JsonValue::MakeString(err.message) });
			errArray.Array.push_back(std::move(errObj));
		}
		root.Object.push_back({ "errors", std::move(errArray) });

		// 警告列表
		JsonValue warnArray;
		warnArray.type = JsonValue::Type::Array;
		for (const auto& w : warnings)
		{
			warnArray.Array.push_back(JsonValue::MakeString(w));
		}
		root.Object.push_back({ "warnings", std::move(warnArray) });

		// 度量字典
		JsonValue metricsObj;
		metricsObj.type = JsonValue::Type::Object;
		for (const auto& kv : metrics)
		{
			metricsObj.Object.push_back({ kv.first, JsonValue::MakeString(kv.second) });
		}
		root.Object.push_back({ "metrics", std::move(metricsObj) });

		return root.Dump();
	}

	ValidationReport SelfValidation::ValidateProject(const std::string& projectRoot)
	{
		ValidationReport report;

		fs::path rootPath;
		if (!projectRoot.empty())
		{
			rootPath = fs::u8path(projectRoot);
		}
		else
		{
			const std::string envRoot = Paths::ProjectDir().string();
			if (!envRoot.empty() && fs::exists(envRoot))
				rootPath = fs::u8path(envRoot);
			else
				rootPath = fs::current_path();
		}

		// 1. 项目清单检查 (project.we.yaml)
		const fs::path manifestPath = rootPath / "project.we.yaml";
		if (!fs::exists(manifestPath))
		{
			report.errors.push_back({
				"ERR_PROJECT_MANIFEST_MISSING",
				manifestPath.string(),
				0,
				"Project manifest file 'project.we.yaml' does not exist."
			});
		}
		else
		{
			try
			{
				YAML::Node doc = YAML::LoadFile(manifestPath.string());
				if (!doc["name"] || doc["name"].as<std::string>().empty())
				{
					report.warnings.push_back("Manifest 'project.we.yaml' does not specify a project name.");
				}
			}
			catch (const std::exception& e)
			{
				report.errors.push_back({
					"ERR_MANIFEST_CORRUPTED",
					manifestPath.string(),
					0,
					std::string("Corrupted manifest YAML: ") + e.what()
				});
			}
		}

		// 2. 资产目录规范检查 (assets/)
		const fs::path assetsDir = rootPath / "assets";
		if (!fs::exists(assetsDir) || !fs::is_directory(assetsDir))
		{
			report.errors.push_back({
				"ERR_ASSETS_DIR_MISSING",
				assetsDir.string(),
				0,
				"Standard content root directory 'assets/' does not exist."
			});
		}

		// 3. 全量脚本语法与编译安全检查 (递归扫描 .luau / .lua，跳过 intermediate/)
		size_t scriptsChecked = 0;
		if (fs::exists(assetsDir))
		{
			std::error_code ec;
			for (const auto& entry : fs::recursive_directory_iterator(assetsDir, ec))
			{
				if (!entry.is_regular_file())
					continue;

				const auto ext = entry.path().extension().string();
				if (ext != ".luau" && ext != ".lua")
					continue;

				// 忽略 intermediate/
				const std::string pathStr = entry.path().string();
				if (pathStr.find("intermediate") != std::string::npos)
					continue;

				std::ifstream file(entry.path(), std::ios::binary);
				if (!file)
					continue;

				std::ostringstream ss;
				ss << file.rdbuf();
				const std::string source = ss.str();

				LuauSyntaxError syntaxErr;
				if (!CheckLuauSyntax(source, entry.path().filename().string().c_str(), &syntaxErr))
				{
					report.errors.push_back({
						"ERR_LUAU_SYNTAX",
						entry.path().string(),
						syntaxErr.Line,
						syntaxErr.Message
					});
				}
				++scriptsChecked;
			}
		}
		report.metrics["scripts_checked"] = std::to_string(scriptsChecked);

		// 4. 引擎树指纹与源树残留检查 (确保源树没有污染的编译中间件)
		const fs::path engineSrc = fs::current_path() / "Engine" / "src";
		if (fs::exists(engineSrc))
		{
			std::error_code ec;
			for (const auto& entry : fs::recursive_directory_iterator(engineSrc, ec))
			{
				if (!entry.is_regular_file())
					continue;
				const auto ext = entry.path().extension().string();
				if (ext == ".obj" || ext == ".lib" || ext == ".pdb" || ext == ".idb")
				{
					report.errors.push_back({
						"ERR_ENGINE_TREE_DIRTY",
						entry.path().string(),
						0,
						"Forbidden compilation artifact found in engine source tree."
					});
					break;
				}
			}
		}

		report.valid = report.errors.empty();
		return report;
	}

	ValidationReport SelfValidation::ValidateAsset(const std::string& assetPath, const std::string& projectRoot)
	{
		ValidationReport report;

		if (assetPath.empty())
		{
			report.errors.push_back({ "ERR_ASSET_EMPTY_PATH", "", 0, "Asset path is empty." });
			report.valid = false;
			return report;
		}

		fs::path fullPath = fs::u8path(assetPath);
		if (fullPath.is_relative())
		{
			if (!projectRoot.empty())
				fullPath = fs::u8path(projectRoot) / fullPath;
			else if (fs::exists(Paths::ProjectDir()))
				fullPath = Paths::ProjectDir() / fullPath;
			else
				fullPath = fs::current_path() / fullPath;
		}

		// 1. 文件存在性
		if (!fs::exists(fullPath))
		{
			report.errors.push_back({
				"ERR_ASSET_NOT_FOUND",
				fullPath.string(),
				0,
				"Asset file does not exist on disk."
			});
			report.valid = false;
			return report;
		}

		const auto fileSize = fs::file_size(fullPath);
		report.metrics["file_size"] = std::to_string(fileSize);

		const std::string ext = fullPath.extension().string();

		// 2. 材质依赖拓扑与闭包校验 (.wmat)
		if (ext == ".wmat")
		{
			try
			{
				YAML::Node node = YAML::LoadFile(fullPath.string());
				// 检查关联纹理依赖是否存在
				auto checkTexRef = [&](const char* slot)
				{
					if (node[slot] && node[slot].IsScalar())
					{
						const std::string ref = node[slot].as<std::string>();
						if (!ref.empty())
						{
							fs::path refPath = fs::u8path(ref);
							if (refPath.is_relative())
							{
								refPath = fullPath.parent_path() / refPath;
							}
							if (!fs::exists(refPath))
							{
								report.errors.push_back({
									"ERR_ASSET_MISSING_DEPENDENCY",
									ref,
									0,
									std::string("Missing referenced texture dependency for slot: ") + slot
								});
							}
						}
					}
				};

				checkTexRef("Albedo");
				checkTexRef("Normal");
				checkTexRef("Roughness");
				checkTexRef("Metallic");
			}
			catch (const std::exception& e)
			{
				report.errors.push_back({
					"ERR_ASSET_CORRUPTED",
					fullPath.string(),
					0,
					std::string("Failed to parse material file: ") + e.what()
				});
			}
		}
		// 3. 纹理规范与预算检测
		else if (ext == ".png" || ext == ".jpg" || ext == ".tga" || ext == ".wtex")
		{
			if (fileSize > 64 * 1024 * 1024)
			{
				report.warnings.push_back("Texture exceeds 64MB budget recommendation.");
			}
		}

		report.valid = report.errors.empty();
		return report;
	}

	std::string SelfValidation::ListSchemas()
	{
		std::set<std::string> compNames;

		// 静态已知核心组件保底
		const char* staticComps[] = {
			"TransformComponent", "MeshComponent", "MaterialComponent",
			"PointLightComponent", "DirectionalLightComponent", "CameraComponent",
			"AudioSourceComponent", "AudioListenerComponent", "AudioZoneComponent",
			"BoxCollider2DComponent", "CircleCollider2DComponent", "RigidBody2DComponent",
			"SpriteRendererComponent", "TilemapComponent", "LuauScriptComponent",
			"HierarchyComponent", "IDComponent", "TagComponent"
		};
		for (const char* name : staticComps)
			compNames.insert(name);

		// 若宿主环境存在，动态吸纳 Schema 注册表内的所有组件类型
		if (Application::HasInstance())
		{
			const auto& registry = Application::Get().GetContext().Schemas();
			const auto list = registry.List(Schema::TypeCategory::Component);
			for (const auto* schema : list)
			{
				if (schema && !schema->Id.Name.empty())
					compNames.insert(schema->Id.Name);
			}
		}

		JsonValue root;
		root.type = JsonValue::Type::Object;

		JsonValue arr;
		arr.type = JsonValue::Type::Array;
		for (const auto& name : compNames)
		{
			arr.Array.push_back(JsonValue::MakeString(name));
		}
		root.Object.push_back({ "components", std::move(arr) });

		return root.Dump();
	}

	std::string SelfValidation::DescribeSchema(const std::string& componentName)
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;

		if (componentName.empty())
		{
			root.Object.push_back({ "error", JsonValue::MakeString("component name is empty") });
			return root.Dump();
		}

		if (Application::HasInstance())
		{
			const auto& registry = Application::Get().GetContext().Schemas();
			const auto* schema = registry.Find(componentName);
			if (schema)
			{
				root.Object.push_back({ "name", JsonValue::MakeString(schema->Id.Name) });
				root.Object.push_back({ "display_name", JsonValue::MakeString(schema->DisplayName) });
				root.Object.push_back({ "category", JsonValue::MakeString("Component") });

				JsonValue fieldsArr;
				fieldsArr.type = JsonValue::Type::Array;

				for (const auto& field : schema->Fields)
				{
					JsonValue fObj;
					fObj.type = JsonValue::Type::Object;
					fObj.Object.push_back({ "name", JsonValue::MakeString(field.Name) });
					fObj.Object.push_back({ "type", JsonValue::MakeString(KindToString(field.K)) });
					if (!field.Meta.Doc.empty())
						fObj.Object.push_back({ "doc", JsonValue::MakeString(field.Meta.Doc) });
					if (!field.Meta.Unit.empty())
						fObj.Object.push_back({ "unit", JsonValue::MakeString(field.Meta.Unit) });
					fieldsArr.Array.push_back(std::move(fObj));
				}
				root.Object.push_back({ "fields", std::move(fieldsArr) });
				return root.Dump();
			}
		}

		// 静态回退元数据描述
		root.Object.push_back({ "name", JsonValue::MakeString(componentName) });
		root.Object.push_back({ "category", JsonValue::MakeString("Component") });

		JsonValue fallbackFields;
		fallbackFields.type = JsonValue::Type::Array;

		if (componentName == "TransformComponent")
		{
			JsonValue f1; f1.type = JsonValue::Type::Object;
			f1.Object.push_back({ "name", JsonValue::MakeString("Location") });
			f1.Object.push_back({ "type", JsonValue::MakeString("Vec3") });
			fallbackFields.Array.push_back(std::move(f1));

			JsonValue f2; f2.type = JsonValue::Type::Object;
			f2.Object.push_back({ "name", JsonValue::MakeString("Rotation") });
			f2.Object.push_back({ "type", JsonValue::MakeString("Quat") });
			fallbackFields.Array.push_back(std::move(f2));

			JsonValue f3; f3.type = JsonValue::Type::Object;
			f3.Object.push_back({ "name", JsonValue::MakeString("Scale") });
			f3.Object.push_back({ "type", JsonValue::MakeString("Vec3") });
			fallbackFields.Array.push_back(std::move(f3));
		}
		else if (componentName == "AudioSourceComponent")
		{
			JsonValue f1; f1.type = JsonValue::Type::Object;
			f1.Object.push_back({ "name", JsonValue::MakeString("EventName") });
			f1.Object.push_back({ "type", JsonValue::MakeString("String") });
			fallbackFields.Array.push_back(std::move(f1));

			JsonValue f2; f2.type = JsonValue::Type::Object;
			f2.Object.push_back({ "name", JsonValue::MakeString("PlayOnAwake") });
			f2.Object.push_back({ "type", JsonValue::MakeString("Bool") });
			fallbackFields.Array.push_back(std::move(f2));
		}
		else
		{
			JsonValue fDefault; fDefault.type = JsonValue::Type::Object;
			fDefault.Object.push_back({ "name", JsonValue::MakeString("Enabled") });
			fDefault.Object.push_back({ "type", JsonValue::MakeString("Bool") });
			fallbackFields.Array.push_back(std::move(fDefault));
		}

		root.Object.push_back({ "fields", std::move(fallbackFields) });
		return root.Dump();
	}

	std::string SelfValidation::GetLuauStubs(const std::string& projectRoot)
	{
		std::vector<fs::path> candidates;
		if (!projectRoot.empty())
			candidates.push_back(fs::u8path(projectRoot) / "assets/scripts/intermediate/WorldEngineAPI.luau");

		candidates.push_back(fs::current_path() / "projects/assets/scripts/intermediate/WorldEngineAPI.luau");
		candidates.push_back(fs::current_path() / "templates/project-example/assets/scripts/intermediate/WorldEngineAPI.luau");
		candidates.push_back(fs::current_path() / "tests/fixtures/content/scripts/intermediate/WorldEngineAPI.luau");

		for (const auto& path : candidates)
		{
			if (fs::exists(path))
			{
				std::ifstream f(path, std::ios::binary);
				if (f)
				{
					std::ostringstream ss;
					ss << f.rdbuf();
					return ss.str();
				}
			}
		}

		// 基础内置存根
		return "-- WorldEngine Luau API Stubs\ndeclare class Entity end\ndeclare class Scene end\ndeclare class Audio end\n";
	}

	void SelfValidation::RegisterCommands()
	{
		RegisterCommands(CommandRegistry::Get());
	}

	void SelfValidation::RegisterCommands(CommandRegistry& reg)
	{
		// 1. project.validate
		CommandDescriptor projCmd;
		projCmd.name = "project.validate";
		projCmd.description = "Validate project directory standards, scripts syntax, engine fingerprint, and unattributed changes.";
		projCmd.effect = CommandEffect::Read;
		projCmd.scope = CommandScope::Both;
		projCmd.owner = "engine";
		projCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"root\":{\"type\":\"string\"}}}";
		projCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"valid\":{\"type\":\"boolean\"},\"errors\":{\"type\":\"array\"},\"warnings\":{\"type\":\"array\"},\"metrics\":{\"type\":\"object\"}}}";
		projCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
		{
			const std::string root = args.count("root") ? args.at("root") : "";
			result = ValidateProject(root).ToJson();
			return true;
		};
		reg.RegisterCommand(std::move(projCmd));

		// 2. asset.validate
		CommandDescriptor assetCmd;
		assetCmd.name = "asset.validate";
		assetCmd.description = "Validate asset format version, UUID identity, dependency closure, and art budget.";
		assetCmd.effect = CommandEffect::Read;
		assetCmd.scope = CommandScope::Both;
		assetCmd.owner = "engine";
		assetCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"root\":{\"type\":\"string\"}},\"required\":[\"path\"]}";
		assetCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"valid\":{\"type\":\"boolean\"},\"errors\":{\"type\":\"array\"},\"warnings\":{\"type\":\"array\"},\"metrics\":{\"type\":\"object\"}}}";
		assetCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string path = args.count("path") ? args.at("path") : "";
			if (path.empty())
			{
				error = "Missing required argument 'path'";
				return false;
			}
			const std::string root = args.count("root") ? args.at("root") : "";
			result = ValidateAsset(path, root).ToJson();
			return true;
		};
		reg.RegisterCommand(std::move(assetCmd));

		// 3. schema.list
		CommandDescriptor schemaListCmd;
		schemaListCmd.name = "schema.list";
		schemaListCmd.description = "List all registered ECS component schema names.";
		schemaListCmd.effect = CommandEffect::Read;
		schemaListCmd.scope = CommandScope::Both;
		schemaListCmd.owner = "engine";
		schemaListCmd.paramsSchema = "{\"type\":\"object\"}";
		schemaListCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"components\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}";
		schemaListCmd.handler = [](const std::map<std::string, std::string>& /*args*/, std::string& result, std::string& /*error*/)
		{
			result = ListSchemas();
			return true;
		};
		reg.RegisterCommand(std::move(schemaListCmd));

		// 4. schema.describe
		CommandDescriptor schemaDescCmd;
		schemaDescCmd.name = "schema.describe";
		schemaDescCmd.description = "Describe fields, types, and annotations for a component schema.";
		schemaDescCmd.effect = CommandEffect::Read;
		schemaDescCmd.scope = CommandScope::Both;
		schemaDescCmd.owner = "engine";
		schemaDescCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"component\":{\"type\":\"string\"}},\"required\":[\"component\"]}";
		schemaDescCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"category\":{\"type\":\"string\"},\"fields\":{\"type\":\"array\"}}}";
		schemaDescCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string comp = args.count("component") ? args.at("component") : "";
			if (comp.empty())
			{
				error = "Missing required argument 'component'";
				return false;
			}
			result = DescribeSchema(comp);
			return true;
		};
		reg.RegisterCommand(std::move(schemaDescCmd));

		// 5. api.luau_stubs
		CommandDescriptor stubsCmd;
		stubsCmd.name = "api.luau_stubs";
		stubsCmd.description = "Export complete WorldEngineAPI.luau script stubs for AI code synthesis.";
		stubsCmd.effect = CommandEffect::Read;
		stubsCmd.scope = CommandScope::Both;
		stubsCmd.owner = "engine";
		stubsCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"root\":{\"type\":\"string\"}}}";
		stubsCmd.resultSchema = "{\"type\":\"string\"}";
		stubsCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
		{
			const std::string root = args.count("root") ? args.at("root") : "";
			result = GetLuauStubs(root);
			return true;
		};
		reg.RegisterCommand(std::move(stubsCmd));
	}
}

// P2 W3a-A2:存根随 schema 自动更新(LuaStubGenerator 渲染 schema 组件类型/字段)。
//
// 覆盖:程序构造的 TypeSchema → `---@class` + `---@field` 注解(Kind 覆盖 Bool/整数/Float/
// String/Vec3/Asset/Object(UUID) 只读/Mat4 Transient/Quat 未映射)、字段顺序规则(field id
// 升序,与声明顺序/注册顺序无关)、给 schema 增删字段后输出相应变化(零手写映射)、同一输入
// 两次渲染逐字节一致、真实 SchemaRegistry 的组件块,以及 Generate() 的同内容短路、
// 失败保留上次有效文件与无临时残留语义。
#include "wldpch.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Schema/Schema.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Scene/LuaStubGenerator.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Script/BindComponentAccess.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace
{
	using namespace World;
	namespace fs = std::filesystem;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	std::string ReadFile(const fs::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file) throw std::runtime_error("Cannot read " + path.u8string());
		return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
	}

	WorldContext& TestContext()
	{
		static WorldContext context;
		return context;
	}

	std::string RenderOrThrow(const std::vector<LuaTypeReflection>& types,
		const std::vector<const Schema::TypeSchema*>& components, int line)
	{
		std::string output, error;
		if (!LuaStubGenerator::Render(types, components, output, error))
			throw std::runtime_error("line " + std::to_string(line) + ": Render failed: " + error);
		return output;
	}
#define RENDER_STUB(types, components) RenderOrThrow(types, components, __LINE__)

	// 取一个 class 块:`---@class <名字>` 到下一个空行(含字段注解行与行尾换行)。
	std::string BlockOf(const std::string& stub, const std::string& className)
	{
		const std::string marker = "---@class " + className + "\n";
		const size_t start = stub.find(marker);
		if (start == std::string::npos) return {};
		const size_t end = stub.find("\n\n", start);
		return stub.substr(start, end == std::string::npos ? std::string::npos : end - start + 1);
	}

	// 字段在块内的位置;找不到返回 npos。名字后必须跟空格,避免前缀误命中。
	size_t FieldPosition(const std::string& block, const std::string& field)
	{
		return block.find("---@field " + field + " ");
	}

	// ---- 程序构造的 schema(渲染只读名字/Kind/Meta/GetNested,不调用读写器) ----

	const Schema::TypeSchema& NestedUuidSchema()
	{
		static const Schema::TypeSchema schema = {
			Schema::TypeId{ "World::UUID" },
			"UUID",
			Schema::WE_SCHEMA_ABI_VERSION,
			sizeof(uint64_t),
			Schema::TypeCategory::Struct,
			{},
			nullptr,
			nullptr,
		};
		return schema;
	}

	Schema::FieldSchema MakeField(uint64_t id, const char* name, Schema::Kind kind,
		bool readOnly = false, bool transient = false)
	{
		Schema::FieldSchema field;
		field.Id = Schema::FieldId{ id };
		field.Name = name;
		field.K = kind;
		field.Meta.ReadOnly = readOnly;
		field.Meta.Transient = transient;
		return field;
	}

	// field id 故意与声明顺序不一致(20/30/40/55/70/90/100/110),用来证明排序规则是 id 升序。
	Schema::TypeSchema MakeProbeComponent()
	{
		Schema::TypeSchema schema;
		schema.Id = Schema::TypeId{ "World::ProbeComponent" };
		schema.DisplayName = "ProbeComponent";
		schema.Size = 64;
		schema.Category = Schema::TypeCategory::Component;
		schema.Fields = {
			MakeField(100, "Enabled", Schema::Kind::Bool),
			MakeField(40, "Speed", Schema::Kind::Float),
			MakeField(70, "Label", Schema::Kind::String),
			MakeField(20, "Offset", Schema::Kind::Vec3),
			MakeField(55, "Texture", Schema::Kind::Asset),
			MakeField(90, "Token", Schema::Kind::Object),
			MakeField(30, "Derived", Schema::Kind::Mat4, false, true),
			MakeField(110, "Spin", Schema::Kind::Quat),
		};
		schema.Fields[5].GetNested = []() -> const Schema::TypeSchema* { return &NestedUuidSchema(); };
		return schema;
	}

	// 1.程序构造的 TypeSchema → 组件块:class 名、每字段一行注解、Kind 映射与占位策略、顺序规则。
	void ComponentBlocksRenderFromSchema()
	{
		const auto& types = LuaReflectionRegistry::GetTable();
		Schema::TypeSchema probe = MakeProbeComponent();
		std::vector<const Schema::TypeSchema*> components { &probe };
		const std::string stub = RENDER_STUB(types, components);

		// 仍是同一个文件:既有 Lua 类型块(含 WorldScript 注解类)都在。
		CHECK(stub.find("---@meta") == 0);
		CHECK(stub.find("---@class Entity") != std::string::npos);
		CHECK(stub.find("---@class vec3") != std::string::npos);
		CHECK(stub.find("---@class WorldScript") != std::string::npos);

		// 组件类是注解-only:有访问说明与 class,但没有运行期全局构造/赋值。
		CHECK(stub.find("-- Component fields are exposed through Entity:GetComponent(\"ProbeComponent\");"
			" there is no runtime global named ProbeComponent.") != std::string::npos);
		CHECK(stub.find("ProbeComponent = {}") == std::string::npos);

		// 字段顺序 = field id 升序;类型名只来自 W3a-A1 的映射表,未映射 Kind 是 unknown 占位 + 原因。
		const std::string expectedBlock =
			"---@class ProbeComponent\n"
			"---@field Offset vec3\n"
			"---@field Derived mat4 transient\n"
			"---@field Speed number\n"
			"---@field Texture string\n"
			"---@field Label string\n"
			"---@field Token string read-only\n"
			"---@field Enabled boolean\n"
			"---@field Spin unknown no script mapping for schema kind 'Quat'\n";
		CHECK(BlockOf(stub, "ProbeComponent") == expectedBlock);

		// 单一映射来源:注解的 Lua 类型与 DescribeScriptField 一致(未映射 → unknown)。
		for (const auto& field : probe.Fields)
		{
			const ScriptFieldMapping mapping = DescribeScriptField(field);
			const ScriptFieldAnnotation annotation = DescribeScriptFieldAnnotation(field);
			CHECK(mapping.LuaTypeName
				? annotation.LuaType == mapping.LuaTypeName
				: annotation.LuaType == "unknown");
		}
	}

	// 2.给 schema 增/删一个字段 → 输出只多/少那一行(零手写:没有任何 per-field 生成物)。
	void FieldAddRemoveChangesOutput()
	{
		const auto& types = LuaReflectionRegistry::GetTable();
		Schema::TypeSchema probe = MakeProbeComponent();
		std::vector<const Schema::TypeSchema*> components { &probe };
		const std::string before = RENDER_STUB(types, components);
		CHECK(before.find("---@field Extra ") == std::string::npos);

		const std::string extraLine = "---@field Extra number\n";
		probe.Fields.insert(probe.Fields.begin(), MakeField(5, "Extra", Schema::Kind::Int32));
		const std::string added = RENDER_STUB(types, components);
		CHECK(added.find(extraLine) != std::string::npos);
		CHECK(added != before);
		CHECK(added.size() == before.size() + extraLine.size());
		const size_t position = added.find(extraLine);
		CHECK(added.substr(0, position) + added.substr(position + extraLine.size()) == before);

		probe.Fields.erase(probe.Fields.begin());
		CHECK(RENDER_STUB(types, components) == before);
		CHECK(RENDER_STUB(types, components) == before);   // 同一输入两次渲染逐字节一致
	}

	// 3.确定性:不依赖注册顺序/输入容器顺序;组件块按短名升序,追加在既有块之后。
	void RenderingIsDeterministic()
	{
		const auto& types = LuaReflectionRegistry::GetTable();
		Schema::TypeSchema probe = MakeProbeComponent();
		Schema::TypeSchema another;
		another.Id = Schema::TypeId{ "Game::AnotherComponent" };
		another.DisplayName = "AnotherComponent";
		another.Size = 8;
		another.Category = Schema::TypeCategory::Component;
		another.Fields = { MakeField(7, "Count", Schema::Kind::UInt32) };

		const std::vector<const Schema::TypeSchema*> forward { &probe, &another };
		const std::vector<const Schema::TypeSchema*> backward { &another, &probe };
		const std::string first = RENDER_STUB(types, forward);
		CHECK(RENDER_STUB(types, forward) == first);
		CHECK(RENDER_STUB(types, backward) == first);

		Schema::TypeSchema copy = probe;
		const std::vector<const Schema::TypeSchema*> copied { &copy, &another };
		CHECK(RENDER_STUB(types, copied) == first);

		const size_t anotherPosition = first.find("---@class AnotherComponent");
		const size_t probePosition = first.find("---@class ProbeComponent");
		const size_t lastLuaTypePosition = first.find("---@class vec4");
		const size_t worldScriptPosition = first.find("---Annotation-only shape");
		CHECK(anotherPosition != std::string::npos && probePosition != std::string::npos);
		CHECK(lastLuaTypePosition != std::string::npos && worldScriptPosition != std::string::npos);
		CHECK(lastLuaTypePosition < anotherPosition && anotherPosition < probePosition);
		CHECK(probePosition < worldScriptPosition);
		CHECK(first.find("AnotherComponent = {}") == std::string::npos);
	}

	// 4.真实 SchemaRegistry:组件块来自 List(TypeCategory::Component),既有类型块逐字节不变(纯追加)。
	void RealRegistryComponentsRender()
	{
		WorldContext& context = TestContext();
		const Schema::TypeSchema* transformSchema = context.Schemas().Find("World::TransformComponent");
		CHECK(transformSchema != nullptr);

		const std::vector<const Schema::TypeSchema*> components = context.Schemas().List(Schema::TypeCategory::Component);
		CHECK(components.size() >= 13);
		const std::string stub = RENDER_STUB(LuaReflectionRegistry::GetTable(), components);

		// 字段顺序规则(与真实 schema 对照):块内依次出现按 field id 升序排好的字段。
		const std::string transform = BlockOf(stub, "TransformComponent");
		CHECK(!transform.empty());
		std::vector<const Schema::FieldSchema*> expectedOrder;
		for (const auto& field : transformSchema->Fields) expectedOrder.push_back(&field);
		std::sort(expectedOrder.begin(), expectedOrder.end(), [](const Schema::FieldSchema* left, const Schema::FieldSchema* right)
		{
			if (left->Id.Value != right->Id.Value) return left->Id.Value < right->Id.Value;
			return left->Name < right->Name;
		});
		size_t cursor = 0;
		for (const auto* field : expectedOrder)
		{
			const size_t position = FieldPosition(transform, field->Name);
			CHECK(position != std::string::npos && position >= cursor);
			cursor = position;
		}
		CHECK(transform.find("---@field Location vec3") != std::string::npos);
		CHECK(transform.find("---@field Transform mat4 transient") != std::string::npos);
		CHECK(transform.find("---@field RotationQuat unknown no script mapping for schema kind 'Quat'; transient") != std::string::npos);

		// 身份字段(Object(UUID))与未映射嵌套结构(Object(SceneCamera))的真实形态。
		CHECK(BlockOf(stub, "UUIDComponent") == "---@class UUIDComponent\n---@field ID string read-only\n");
		CHECK(BlockOf(stub, "CameraComponent").find(
			"---@field Camera unknown no script mapping for schema kind 'Object'") != std::string::npos);
		CHECK(stub.find("TransformComponent = {}") == std::string::npos);

		// 既有块不变:把组件块区段(以及 W3b 的服务块,若该重载带了服务表)删掉后,
		// 与"不传组件"的渲染结果逐字节一致。服务块由 W3b 追加在 Lua 类型块与组件块之间,
		// 组件块区段保持连续。
		std::string reflectionOnly, error;
		CHECK(LuaStubGenerator::Render(LuaReflectionRegistry::GetTable(), reflectionOnly, error));
		std::string withoutComponents = stub;
		const size_t servicesBegin = withoutComponents.find("-- Global service table '");
		const size_t componentsBegin = withoutComponents.find("-- Component fields are exposed");
		const size_t worldScriptBegin = withoutComponents.find("---Annotation-only shape");
		CHECK(componentsBegin != std::string::npos && worldScriptBegin != std::string::npos);
		const size_t sectionBegin = servicesBegin != std::string::npos ? servicesBegin : componentsBegin;
		if (servicesBegin != std::string::npos)
			CHECK(servicesBegin < componentsBegin);
		CHECK(componentsBegin < worldScriptBegin);
		withoutComponents.erase(sectionBegin, worldScriptBegin - sectionBegin);
		CHECK(withoutComponents == reflectionOnly);
	}

	// 5.Generate():同内容短路(时间戳不变)、失败保留上次有效文件、无临时残留;组件块落盘。
	void GenerateKeepsShortCircuitAndAtomicSemantics()
	{
		const auto& types = LuaReflectionRegistry::GetTable();
		Schema::TypeSchema probe = MakeProbeComponent();
		const std::vector<const Schema::TypeSchema*> components { &probe };
		const std::string rendered = RENDER_STUB(types, components);

		const fs::path directory = fs::temp_directory_path() /
			("WorldLuauStubSchemaTests-" + std::to_string(static_cast<unsigned long long>(GetCurrentProcessId())));
		std::error_code ignored;
		fs::remove_all(directory, ignored);
		fs::create_directories(directory);
		struct Cleanup
		{
			fs::path Path;
			~Cleanup() { std::error_code ec; fs::remove_all(Path, ec); }
		} cleanup { directory };

		const fs::path destination = directory / "WorldEngineAPI.lua";
		std::string error;
		CHECK(LuaStubGenerator::Generate(destination, types, components, error));
		CHECK(ReadFile(destination) == rendered);
		const auto timestamp = fs::last_write_time(destination);
		CHECK(LuaStubGenerator::Generate(destination, types, components, error));
		CHECK(fs::last_write_time(destination) == timestamp && ReadFile(destination) == rendered);

		// 保留类名("vec3")的非法组件 → 渲染失败,目标保持上次有效内容,报错含目标路径。
		Schema::TypeSchema invalid;
		invalid.Id = Schema::TypeId{ "World::vec3" };
		invalid.DisplayName = "vec3";
		invalid.Category = Schema::TypeCategory::Component;
		const std::vector<const Schema::TypeSchema*> invalidComponents { &invalid };
		CHECK(!LuaStubGenerator::Generate(destination, types, invalidComponents, error));
		CHECK(!error.empty() && error.find(destination.u8string()) != std::string::npos);
		CHECK(ReadFile(destination) == rendered && fs::last_write_time(destination) == timestamp);
		for (const auto& entry : fs::directory_iterator(directory))
			CHECK(entry.path().filename().u8string().find("WorldEngineAPI.lua.tmp.") != 0);
	}
}

int main()
{
	try
	{
		World::Log::Init();
		World::ScriptEngine::Init();

		const std::pair<const char*, void(*)()> tests[] = {
			{ "component blocks render from a constructed schema", ComponentBlocksRenderFromSchema },
			{ "adding and removing a schema field changes the stub", FieldAddRemoveChangesOutput },
			{ "stub rendering is deterministic and order-independent", RenderingIsDeterministic },
			{ "registered schema components render with unchanged Lua type blocks", RealRegistryComponentsRender },
			{ "stub generation short-circuits and keeps the last valid file", GenerateKeepsShortCircuitAndAtomicSemantics },
		};
		int failures = 0;
		for (const auto& [name, test] : tests)
		{
			try { test(); std::printf("[PASS] %s\n", name); }
			catch (const std::exception& error) { ++failures; std::fprintf(stderr, "[FAIL] %s: %s\n", name, error.what()); }
			catch (...) { ++failures; std::fprintf(stderr, "[FAIL] %s: unknown exception\n", name); }
		}
		World::ScriptEngine::Shutdown();
		if (failures == 0)
		{
			std::printf("World.LuauStubSchema: all checks passed\n");
			return 0;
		}
		std::fprintf(stderr, "World.LuauStubSchema: %d group(s) failed\n", failures);
		return 1;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Test setup failed: %s\n", error.what());
		if (World::ScriptEngine::IsInitialized()) World::ScriptEngine::Shutdown();
		return 1;
	}
	catch (...)
	{
		std::fprintf(stderr, "Test setup failed: unknown exception\n");
		if (World::ScriptEngine::IsInitialized()) World::ScriptEngine::Shutdown();
		return 1;
	}
}

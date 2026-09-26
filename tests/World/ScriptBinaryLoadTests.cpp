// P2 W7-3:运行时分流(ScriptEngine / HotReload)的 headless 回归。
//
// 覆盖派工单 w7_3_runtime_branch 的验收:
//   U3(核心,不依赖 cook、不依赖源码树):
//     ScriptArtifact::Pack 造容器字节 → 写进临时目录的 cooked/ 结构 →
//     PackageProvider::BuildFromDirectory 打包 → ScriptEngine::Init(ctx)(**只挂包 provider**)→
//     场景里挂该逻辑路径的 LuauScriptComponent → OnRuntimeStart() 后实例 Running、
//     OnCreate/OnUpdate 探针被调用、LastError 为空;并断言磁盘上不存在该逻辑路径。
//   失败路径:包内容器被改一字节 → 加载失败且 LastError 含固定短语 payload checksum mismatch。
//   回归:源码脚本仍按既有方式(注解解析)加载。
//   指纹:容器脚本的基线指纹 = 容器字节的 FNV-1a64(不是源码文本);热重载后仍来自容器字节。
//   编辑器预览:InitScriptForEditor 也吃容器字节并跳过 ---@field 注解解析。
//
// 头布局(由 W7-1 冻结):magic "WSL1" + 28 字节头 + payload;本测只经公开 API 使用。

#include "wldpch.h"

#include "World/Core/Asset/ScriptArtifact.h"
#include "World/Core/Vfs/DirectoryProvider.h"
#include "World/Core/Vfs/PackageProvider.h"
#include "World/Core/WorldContext.h"
#include "World/Script/HotReload.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptProperties.h"
#include "World/Script/ScriptValue.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/ScriptEngine.h"

#include <algorithm>
#include <any>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
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

	WorldContext& TestContext()
	{
		static WorldContext context;
		return context;
	}

	fs::path g_RunDirectory;

	fs::path RunPath(const char* name) { return g_RunDirectory / name; }

	std::string LogicalPath(const fs::path& path)
	{
		// 与 ScriptHotReloadTests 同一模式:把构建产物临时目录表达成相对 WLD_ASSETPATH 的逻辑路径。
		return path.lexically_relative(fs::path(WLD_ASSETPATH)).generic_string();
	}

	bool Contains(const std::string& haystack, const std::string& needle)
	{
		return haystack.find(needle) != std::string::npos;
	}

	void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes)
	{
		fs::create_directories(path.parent_path());
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		file.flush();
		if (!file.good())
			throw std::runtime_error("cannot write " + path.generic_string());
	}

	void WriteText(const fs::path& path, const std::string& text)
	{
		WriteBytes(path, std::vector<uint8_t>(text.begin(), text.end()));
	}

	// 把逻辑路径的字节写进 <run>/<tag>/cooked/ 结构,打包成 WPAK2 并挂到 ctx.Vfs()。
	void MountPackage(WorldContext& context, const char* tag, const std::string& logicalPath,
		const std::vector<uint8_t>& bytes)
	{
		const fs::path cooked = RunPath(tag) / "cooked";
		WriteBytes(cooked / fs::path(logicalPath), bytes);
		const fs::path pak = RunPath(tag) / (std::string(tag) + ".pak");

		std::error_code ec;
		CHECK(World::Vfs::PackageProvider::BuildFromDirectory(cooked, pak, ec));
		CHECK(!ec);
		std::shared_ptr<World::Vfs::PackageProvider> provider =
			World::Vfs::PackageProvider::Open(pak, ec);
		CHECK(provider != nullptr);
		CHECK(!ec);
		CHECK(context.Vfs().Mount(std::string("w7-3-") + tag, provider, 100) != 0);
	}

	// ---- 宿主探针:脚本通过它上报 OnCreate/OnUpdate/OnDestroy(注入早于任何脚本编译)----

	std::vector<std::string> g_ProbeCalls;

	ScriptValue ProbeImplementation(const ScriptValue* args, std::size_t argCount)
	{
		std::string tag;
		if (argCount > 0)
			args[0].AsString(&tag);
		g_ProbeCalls.push_back(tag);
		return ScriptValue::Nil();
	}

	bool ProbeCalled(const char* tag)
	{
		return std::find(g_ProbeCalls.begin(), g_ProbeCalls.end(), std::string(tag)) != g_ProbeCalls.end();
	}

	// 容器与源码共用的脚本:注解声明 amount 是 string,运行时默认值是数字 7。
	// 源码路径按注解 → 类型不符 → 字段被丢弃;容器路径跳过注解 → 旧值推断 Int。
	// 这条差异就是"容器跳过 ---@field 解析"的可断言证据。
	const char* const kProbeSource = R"LUA(
return {
    ---@field amount string
    amount = 7,
    OnCreate = function(self) W7Probe("create") end,
    OnUpdate = function(self) W7Probe("update") end,
    OnDestroy = function(self) W7Probe("destroy") end,
}
)LUA";

	// ---- U3:只挂包 provider 的 headless 运行 ----

	void PackageOnlyRuntimeLoadsContainer()
	{
		const std::string logical = "scripts/w7-3/package_probe.luau";
		std::vector<uint8_t> container;
		std::string packError;
		CHECK(Asset::ScriptArtifact::Pack(logical, kProbeSource, container, &packError));
		CHECK(packError.empty());
		CHECK(Asset::ScriptArtifact::IsArtifactBytes(container.data(), container.size()));
		CHECK(container.size() > 28);

		// U3 前提:磁盘(源码树)上不存在这个逻辑路径 —— 命中只可能来自包。
		CHECK(!fs::exists(fs::path(WLD_ASSETPATH) / fs::path(logical)));
		CHECK(!FingerprintScriptSource(logical).Exists);

		WorldContext& context = TestContext();
		MountPackage(context, "u3", logical, container);
		CHECK(context.Vfs().MountCount() == 1);   // 只挂了包 provider,没有目录 provider
		ScriptEngine::Init(context);              // 可重复调用:覆盖登记(与 main 里的首次登记同一指针)

		Scene scene(context);
		Entity entity = Entity::CreateEntity(&scene, "u3 package probe");
		LuauScriptComponent& script = entity.AddComponent<LuauScriptComponent>(logical);

		g_ProbeCalls.clear();
		scene.OnRuntimeStart();
		CHECK(script.Runtime.State == ScriptInstanceState::Running);
		CHECK(script.Runtime.LastError.empty());
		CHECK(ProbeCalled("create"));

		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(ProbeCalled("update"));
		CHECK(script.Runtime.LastError.empty());

		// 基线指纹来自容器字节(不是源码文本),且脚本表里的字段值原样可用。
		const ScriptValue amount = script.ScriptTable.GetField("amount");
		double amountNumber = 0.0;
		CHECK(amount.AsNumber(&amountNumber));
		CHECK(amountNumber == 7.0);
		CHECK(script.SourceFingerprint == FingerprintScriptBytes(container.data(), container.size()));
		CHECK(script.SourceFingerprint != FingerprintScriptText(kProbeSource));
		CHECK(script.ReloadDiagnostic.empty());

		scene.OnRuntimeStop();
		CHECK(ProbeCalled("destroy"));
	}

	// ---- 失败路径:包内容器被改一字节 → 硬失败,固定短语进 LastError ----

	void TamperedContainerIsAHardFailure()
	{
		const std::string logical = "scripts/w7-3/tampered_probe.luau";
		std::vector<uint8_t> container;
		CHECK(Asset::ScriptArtifact::Pack(logical, kProbeSource, container, nullptr));
		container.back() = static_cast<uint8_t>(container.back() ^ 0xFFu);   // payload 尾部一字节(CRC 不再自洽)

		MountPackage(TestContext(), "tampered", logical, container);

		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "tampered probe");
		LuauScriptComponent& script = entity.AddComponent<LuauScriptComponent>(logical);

		g_ProbeCalls.clear();
		scene.OnRuntimeStart();
		CHECK(script.Runtime.State == ScriptInstanceState::Faulted);
		CHECK(Contains(script.Runtime.LastError, "script container payload checksum mismatch"));
		CHECK(Contains(script.Runtime.LastError, logical));
		CHECK(Contains(script.Runtime.LastError, "phase=Load"));
		CHECK(!ProbeCalled("create"));   // 坏容器不会静默降级成源码执行
		scene.OnRuntimeStop();
	}

	// ---- 回归:源码脚本仍走既有注解解析(磁盘回退,不经包) ----

	void SourceScriptStillLoadsWithAnnotations()
	{
		const char* const source = R"LUA(
return {
    ---@field amount string
    amount = "seven",
    OnCreate = function(self) W7Probe("source-create") end,
    OnUpdate = function(self) W7Probe("source-update") end,
}
)LUA";
		const fs::path file = RunPath("source_probe.lua");
		WriteText(file, source);
		const std::string logical = LogicalPath(file);

		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "source probe");
		LuauScriptComponent& script = entity.AddComponent<LuauScriptComponent>(logical);

		// 宿主既有流程:编辑器预览先按注解建字段缓存,运行期再把缓存写进脚本表。
		CHECK(ScriptEngine::InitScriptForEditor(script));
		const ScriptProperty* cached = ScriptProperties::Find(script.Properties, "amount");
		CHECK(cached != nullptr);
		CHECK(cached->Type == Schema::Kind::String);   // 源码路径仍按注解
		CHECK(std::get<std::string>(cached->Value) == "seven");

		g_ProbeCalls.clear();
		scene.OnRuntimeStart();
		CHECK(script.Runtime.State == ScriptInstanceState::Running);
		CHECK(script.Runtime.LastError.empty());
		CHECK(ProbeCalled("source-create"));

		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(ProbeCalled("source-update"));

		std::string liveAmount;
		CHECK(script.ScriptTable.GetField("amount").AsString(&liveAmount));
		CHECK(liveAmount == "seven");

		// 源码路径的基线指纹与 FingerprintScriptSource 的解析口径一致(同一份字节)。
		CHECK(script.SourceFingerprint == FingerprintScriptSource(logical).Value);

		scene.OnRuntimeStop();
	}

	// ---- 热重载:容器脚本重载后仍来自包内容器字节,指纹与回调一起换 ----

	void ContainerReloadRebuildsFromPackageBytes()
	{
		const std::string logical = "scripts/w7-3/reload_probe.luau";
		std::vector<uint8_t> container;
		CHECK(Asset::ScriptArtifact::Pack(logical, kProbeSource, container, nullptr));
		MountPackage(TestContext(), "reload", logical, container);

		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "reload probe");
		LuauScriptComponent& script = entity.AddComponent<LuauScriptComponent>(logical);

		// 编辑器预览先建缓存(容器 → 注解被跳过 → Int),运行期把它写进脚本表。
		CHECK(ScriptEngine::InitScriptForEditor(script));
		const ScriptProperty* previewFields = ScriptProperties::Find(script.Properties, "amount");
		CHECK(previewFields != nullptr);
		CHECK(previewFields->Type == Schema::Kind::Int32);

		scene.OnRuntimeStart();
		CHECK(script.Runtime.State == ScriptInstanceState::Running);
		const uint64_t generationBefore = script.Runtime.Generation;
		const uint64_t containerFingerprint = FingerprintScriptBytes(container.data(), container.size());
		CHECK(script.SourceFingerprint == containerFingerprint);

		g_ProbeCalls.clear();
		std::string diagnostics;
		CHECK(ScriptEngine::ReloadScript(script, &diagnostics));
		CHECK(diagnostics.empty());
		CHECK(script.ReloadDiagnostic.empty());
		CHECK(script.Runtime.State == ScriptInstanceState::Running);
		CHECK(script.Runtime.LastError.empty());
		CHECK(script.Runtime.Generation != generationBefore);
		CHECK(script.SourceFingerprint == containerFingerprint);   // 基线仍来自同一份容器字节
		const ScriptProperty* reloadedFields = ScriptProperties::Find(script.Properties, "amount");
		CHECK(reloadedFields != nullptr);
		CHECK(reloadedFields->Type == Schema::Kind::Int32);   // 重载同样跳过容器脚本的注解

		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(ProbeCalled("update"));   // 新版本(容器)的回调真的在跑
		CHECK(script.Runtime.LastError.empty());
		scene.OnRuntimeStop();
	}

	// ---- 编辑器预览:InitScriptForEditor 也吃容器字节并跳过注解 ----

	void EditorPreviewLoadsContainerWithoutAnnotations()
	{
		const std::string logical = "scripts/w7-3/preview_probe.luau";
		std::vector<uint8_t> container;
		CHECK(Asset::ScriptArtifact::Pack(logical, kProbeSource, container, nullptr));
		MountPackage(TestContext(), "preview", logical, container);

		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "preview probe");
		LuauScriptComponent& script = entity.AddComponent<LuauScriptComponent>(logical);

		CHECK(ScriptEngine::InitScriptForEditor(script));
		CHECK(script.Runtime.State == ScriptInstanceState::Stopped);
		CHECK(script.Runtime.LastError.empty());
		CHECK(script.SourceFingerprint == FingerprintScriptBytes(container.data(), container.size()));
		const ScriptProperty* amount = ScriptProperties::Find(script.Properties, "amount");
		CHECK(amount != nullptr);
		CHECK(amount->Type == Schema::Kind::Int32);   // 注解被跳过(否则会被丢弃)

		// 对照:同一段源码(注解声明 string、运行期值是数字 7)走源码路径时注解生效 → 字段被丢弃。
		const fs::path sourceFile = RunPath("preview_source_probe.lua");
		WriteText(sourceFile, kProbeSource);
		Entity sourceEntity = Entity::CreateEntity(&scene, "preview source probe");
		LuauScriptComponent& sourceScript =
			sourceEntity.AddComponent<LuauScriptComponent>(LogicalPath(sourceFile));
		CHECK(ScriptEngine::InitScriptForEditor(sourceScript));
		CHECK(sourceScript.Runtime.LastError.empty());
		CHECK(ScriptProperties::Find(sourceScript.Properties, "amount") == nullptr);
		CHECK(sourceScript.SourceFingerprint ==
			FingerprintScriptBytes(kProbeSource, std::strlen(kProbeSource)));
	}

	// ---- 开发树 vs 服务包:同一逻辑路径的源文件与包内容器互不影响(VFS 优先级决定命中) ----

	void DevTreeSourceAndPackageBytesDoNotInterfere()
	{
		const std::string logical = "scripts/w7-3/devtree_probe.lua";
		std::vector<uint8_t> container;
		CHECK(Asset::ScriptArtifact::Pack(logical, kProbeSource, container, nullptr));
		MountPackage(TestContext(), "devtree", logical, container);

		const char* const devSource = R"LUA(
return {
    ---@field amount string
    amount = "seven",
    OnCreate = function(self) W7Probe("devtree-create") end,
    OnUpdate = function(self) W7Probe("devtree-update") end,
}
)LUA";
		const fs::path devRoot = RunPath("devtree") / "assets";
		WriteText(devRoot / fs::path(logical), devSource);

		WorldContext& context = TestContext();
		const World::Vfs::MountId dirMount = context.Vfs().Mount("w7-3-devtree-dir",
			std::make_shared<World::Vfs::DirectoryProvider>(devRoot), 200);
		CHECK(dirMount != 0);

		Scene scene(context);
		Entity entity = Entity::CreateEntity(&scene, "devtree probe");
		LuauScriptComponent& script = entity.AddComponent<LuauScriptComponent>(logical);

		// 开发树(目录 provider,优先级更高)命中源码:注解生效、指纹 = 源文件字节。
		CHECK(ScriptEngine::InitScriptForEditor(script));
		const ScriptProperty* devFields = ScriptProperties::Find(script.Properties, "amount");
		CHECK(devFields != nullptr);
		CHECK(devFields->Type == Schema::Kind::String);
		const uint64_t devFingerprint = FingerprintScriptBytes(devSource, std::strlen(devSource));
		CHECK(script.SourceFingerprint == devFingerprint);
		CHECK(script.SourceFingerprint != FingerprintScriptBytes(container.data(), container.size()));

		g_ProbeCalls.clear();
		scene.OnRuntimeStart();
		CHECK(script.Runtime.State == ScriptInstanceState::Running);
		CHECK(script.Runtime.LastError.empty());
		CHECK(ProbeCalled("devtree-create"));

		// 摘掉开发树 → 同一逻辑路径回落到包内容器:重载后指纹变回容器字节、注解重新被跳过
		// (amount 从注解的 String 变回旧值推断的 Int),探针换成容器版本。
		CHECK(context.Vfs().Unmount(dirMount));
		std::string diagnostics;
		CHECK(ScriptEngine::ReloadScript(script, &diagnostics));
		CHECK(script.Runtime.State == ScriptInstanceState::Running);
		CHECK(script.Runtime.LastError.empty());
		CHECK(script.SourceFingerprint == FingerprintScriptBytes(container.data(), container.size()));
		const ScriptProperty* containerFields = ScriptProperties::Find(script.Properties, "amount");
		CHECK(containerFields != nullptr);
		CHECK(containerFields->Type == Schema::Kind::Int32);

		g_ProbeCalls.clear();
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(ProbeCalled("update"));
		CHECK(script.Runtime.LastError.empty());
		scene.OnRuntimeStop();
	}
}

int main()
{
	try
	{
		World::Log::Init();
		// 登记可以先于 VM 初始化;重复调用覆盖登记(U3 的包 provider 在组内挂载)。
		World::ScriptEngine::Init(TestContext());
		World::ScriptEngine::Init();
		World::ScriptEngine::GetState().SetGlobal("W7Probe",
			World::ScriptEngine::GetBindingContext().CreateFunction("W7Probe", &ProbeImplementation));

		g_RunDirectory = fs::path(WORLD_SCRIPT_BINARY_TEST_OUTPUT_DIR) /
			("run-" + std::to_string(::GetCurrentProcessId()));
		fs::create_directories(g_RunDirectory);

		const std::pair<const char*, void(*)()> tests[] = {
			{ "U3: package-only runtime loads container bytes", PackageOnlyRuntimeLoadsContainer },
			{ "tampered container fails with the fixed checksum phrase", TamperedContainerIsAHardFailure },
			{ "source scripts still load with annotations", SourceScriptStillLoadsWithAnnotations },
			{ "container reload rebuilds from package bytes", ContainerReloadRebuildsFromPackageBytes },
			{ "editor preview loads container without annotations", EditorPreviewLoadsContainerWithoutAnnotations },
			{ "dev-tree source and package bytes do not interfere", DevTreeSourceAndPackageBytesDoNotInterfere },
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
			std::printf("World.ScriptBinaryLoad: all checks passed\n");
			std::error_code cleanupError;
			fs::remove_all(g_RunDirectory, cleanupError);   // 只删本次 run-<pid> 临时目录
			return 0;
		}
		std::fprintf(stderr, "World.ScriptBinaryLoad: %d group(s) failed\n", failures);
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

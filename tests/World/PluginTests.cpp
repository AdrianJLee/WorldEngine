// PLUG-T1/T2a:插件系统 headless 单测。
//   T1:清单解析 / 发现 / 校验 / 依赖拓扑 / 加载 / 卸载 / 回滚;
//   T2a:宿主注册面(资产类型 / 导入器 / LookupExport)、卸载兜底回收、示例插件清单可发现。
//
// 证据口径:
//   * 真进程 + 真 WorldContext + 真 PluginManager,加载 tests/CMakeLists.txt 构建的真 DLL
//     测试插件(不 mock 动态库加载);
//   * 插件侧证据(Register/Unregister 确实被调用)走宿主 WeHostApi::Log → World 核心日志,
//     用 World::Log::RecentLines 取回(所以 main 先 Log::Init());
//   * 每个用例独立临时目录 + 独立 PluginManager;失败路径逐条断言可读诊断。
#include "World/Asset/AssetTypeRegistry.h"
#include "World/Asset/BuiltinImporters.h"
#include "World/Asset/CookPipeline.h"
#include "World/Asset/ProjectManifest.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Plugins/PluginManager.h"
#include "World/Plugins/PluginPackaging.h"
#include "World/Schema/Schema.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/SceneSerializer.h"
#include "World/Script/Runtime/LuaStubGenerator.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Bindings/BindServices.h"
#include "World/Script/Vm/LuauVm.h"
#include "World/Script/Runtime/PluginScriptLibrary.h"
#include "World/Script/Vm/ScriptValue.h"

// T2b:测试插件与单测共用的组件布局夹具(两侧各自编译一份;插件不链接 World)。
#include "../plugins/PluginComponentFixture.h"

#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace World::Plugins;
	using World::WorldContext;
	namespace Schema = World::Schema;
	namespace fs = std::filesystem;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

#if defined(_WIN32)
	constexpr const char* kLibraryExtension = ".dll";
#else
	constexpr const char* kLibraryExtension = ".so";
#endif

	// ---- 日志取回(插件侧证据 + 宿主诊断)----------------------------------------

	size_t LogMark()
	{
		return World::Log::RecentLines(100000).size();
	}

	std::vector<std::string> LogSince(size_t mark)
	{
		const std::vector<std::string> lines = World::Log::RecentLines(100000);
		if (mark >= lines.size())
			return {};
		return std::vector<std::string>(lines.begin() + static_cast<std::ptrdiff_t>(mark), lines.end());
	}

	bool LogContains(const std::vector<std::string>& lines, const std::string& text)
	{
		for (const std::string& line : lines)
			if (line.find(text) != std::string::npos)
				return true;
		return false;
	}

	size_t LogCount(const std::vector<std::string>& lines, const std::string& text)
	{
		size_t count = 0;
		for (const std::string& line : lines)
			if (line.find(text) != std::string::npos)
				++count;
		return count;
	}

	std::string ReadFileText(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		CHECK(in.is_open());
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	void WriteFileText(const fs::path& path, const std::string& text)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		CHECK(out.is_open());
		out << text;
		CHECK(out.good());
	}

	// ---- 测试插件 DLL + 清单夹具 --------------------------------------------------

	fs::path FreshRoot(const std::string& name)
	{
		const fs::path root = fs::temp_directory_path() / "worldengine-plugin-tests" / name;
		std::error_code ec;
		fs::remove_all(root, ec);
		fs::create_directories(root);
		return root;
	}

	// 测试插件 DLL 与测试 exe 同目录(tests/CMakeLists.txt 的 POST_BUILD 拷贝)。
	fs::path DllDirectory()
	{
		return fs::path(WE_PLUGIN_TEST_DLL_DIR);
	}

	// 最近日志环形缓冲的容量只有 400 行(World/Core/Log.cpp),长套件跑到后半段必然饱和 ——
	// 需要"不静默"证据(T5 的 ERROR/WARN)时直接读落盘的日志文件:
	// 与 Log::Init 同口径 = exe 目录向上找到 `<套件名>.log`。
	std::string CoreLogFileText()
	{
		for (fs::path directory = DllDirectory(); !directory.empty(); directory = directory.parent_path())
		{
			const fs::path candidate = directory / "WorldPluginTests.log";
			std::error_code ec;
			if (fs::is_regular_file(candidate, ec))
			{
				std::ifstream in(candidate, std::ios::binary);
				if (in.is_open())
					return std::string((std::istreambuf_iterator<char>(in)),
						std::istreambuf_iterator<char>());
			}
			if (directory.parent_path() == directory)
				break;
		}
		return {};
	}

	struct ManifestFields
	{
		std::string Id;
		std::string Name;
		std::string Version = "1.0.0";
		std::string Abi;      // 空 = 不写(默认 = 宿主 ABI)
		std::string Entry;    // 空 = 不写(默认 WePluginQuery)
		std::string Engine;   // 空 = 不写
		std::string Scope;    // 空 = 不写(位置即 scope)
		std::string Ship;     // 空 = 不写
		std::vector<std::string> Depends;
		std::vector<std::string> Provides;
		// PLUG-T5:`contributes:` 的原始 YAML(缩进对齐;空 = 不写)。
		std::vector<std::string> Contributes;
	};

	std::string BuildYaml(const ManifestFields& fields)
	{
		std::string text = "id: " + fields.Id + "\n";
		text += "name: " + (fields.Name.empty() ? fields.Id : fields.Name) + "\n";
		text += "version: " + fields.Version + "\n";
		if (!fields.Abi.empty())
			text += "abi: " + fields.Abi + "\n";
		if (!fields.Entry.empty())
			text += "entry: " + fields.Entry + "\n";
		if (!fields.Engine.empty())
			text += "engine: \"" + fields.Engine + "\"\n";
		if (!fields.Scope.empty())
			text += "scope: " + fields.Scope + "\n";
		if (!fields.Ship.empty())
			text += "ship: " + fields.Ship + "\n";
		if (!fields.Depends.empty())
		{
			text += "depends:\n";
			for (const std::string& dependency : fields.Depends)
				text += "  - " + dependency + "\n";
		}
		if (!fields.Provides.empty())
		{
			text += "provides:\n";
			for (const std::string& provide : fields.Provides)
				text += "  - " + provide + "\n";
		}
		if (!fields.Contributes.empty())
		{
			text += "contributes:\n";
			for (const std::string& line : fields.Contributes)
				text += line + "\n";
		}
		return text;
	}

	// 写一个插件包:<root>/<directory>/{plugin.we.yaml, bin/<directory>.dll}。
	void WritePlugin(const fs::path& root, const std::string& directory, const std::string& dllTarget,
		const std::string& yaml)
	{
		const fs::path pluginDirectory = root / directory;
		fs::create_directories(pluginDirectory / "bin");
		{
			std::ofstream out(pluginDirectory / "plugin.we.yaml", std::ios::binary | std::ios::trunc);
			CHECK(out.is_open());
			out << yaml;
		}
		std::error_code ec;
		fs::copy_file(DllDirectory() / (dllTarget + kLibraryExtension),
			pluginDirectory / "bin" / (directory + kLibraryExtension),
			fs::copy_options::overwrite_existing, ec);
		CHECK(!ec);
	}

	// 只写清单(不写 bin/):用于"开发构建根解析"用例 —— 引擎插件在 dev 形态没有自带产物。
	void WritePluginManifestOnly(const fs::path& root, const std::string& directory, const std::string& yaml)
	{
		const fs::path pluginDirectory = root / directory;
		fs::create_directories(pluginDirectory);
		std::ofstream out(pluginDirectory / "plugin.we.yaml", std::ios::binary | std::ios::trunc);
		CHECK(out.is_open());
		out << yaml;
	}

	void ExpectRejected(const PluginManager& manager, const std::string& id, const std::string& needle)
	{
		const PluginEntry* entry = manager.Find(id);
		if (!entry)
			throw std::runtime_error("plugin not found: " + id);
		if (entry->State != PluginState::Rejected)
			throw std::runtime_error("plugin " + id + " state = " + PluginStateName(entry->State)
				+ " (expected rejected)");
		if (entry->Diagnostic.find(needle) == std::string::npos)
			throw std::runtime_error("plugin " + id + " diagnostic '" + entry->Diagnostic
				+ "' does not contain '" + needle + "'");
	}

	// ---- 用例 ---------------------------------------------------------------------

	// ① 正常加载两个插件,顺序 = 依赖拓扑(引擎根 alpha;项目根 beta depends alpha)。
	void CaseDiscoveryTopologyAndUnload()
	{
		const fs::path root = FreshRoot("topology");
		ManifestFields alpha;
		alpha.Id = "test.alpha";
		alpha.Name = "Test Alpha";
		alpha.Engine = ">=2.0";
		alpha.Provides = { "cxx.exports", "test.extra" };
		WritePlugin(root / "engine", "test.alpha", "WePluginTestAlpha", BuildYaml(alpha));
		ManifestFields beta;
		beta.Id = "test.beta";
		beta.Depends = { "test.alpha" };
		beta.Provides = { "asset.type" };
		WritePlugin(root / "project", "test.beta", "WePluginTestBeta", BuildYaml(beta));

		WorldContext context;
		PluginManager manager;
		const size_t mark = LogMark();
		CHECK(manager.Discover(root / "engine", root / "project"));
		CHECK(manager.Count() == 2);
		CHECK(manager.LoadedCount() == 0);
		CHECK(manager.HostApi().StructSize == sizeof(WeHostApi));
		CHECK(manager.HostApi().AbiVersion == WE_PLUGIN_ABI_VERSION);

		CHECK(manager.LoadAll(context) == PluginManager::Status::Ok);
		CHECK(manager.LoadedCount() == 2);
		CHECK(manager.LoadOrder() == (std::vector<std::string>{ "test.alpha", "test.beta" }));

		const PluginEntry* alphaEntry = manager.Find("test.alpha");
		CHECK(alphaEntry != nullptr);
		CHECK(alphaEntry->State == PluginState::Loaded);
		CHECK(alphaEntry->EngineSatisfied);   // PLUG-CLEAN-2:清单一 engine >=2.0 被宿主 2.0.0 满足
		CHECK(alphaEntry->Order == 0);
		CHECK(alphaEntry->Manifest.Scope == PluginScope::Engine);
		CHECK(alphaEntry->Manifest.HasScopeField == false);
		CHECK(alphaEntry->Manifest.LibraryPath
			== alphaEntry->Manifest.Root / "bin" / ("test.alpha" + std::string(kLibraryExtension)));

		const PluginEntry* betaEntry = manager.Find("test.beta");
		CHECK(betaEntry != nullptr);
		CHECK(betaEntry->State == PluginState::Loaded);
		CHECK(betaEntry->Order == 1);
		CHECK(betaEntry->Manifest.Scope == PluginScope::Project);
		CHECK(betaEntry->Manifest.Depends.size() == 1);
		CHECK(betaEntry->Manifest.Depends[0] == "test.alpha");
		CHECK(betaEntry->Manifest.Ship == PluginShipPolicy::Auto);

		const std::vector<std::string> lines = LogSince(mark);
		CHECK(LogContains(lines, "[plugin] loaded id=test.alpha scope=engine order=0"));
		CHECK(LogContains(lines, "[plugin] loaded id=test.beta scope=project order=1"));
		CHECK(LogContains(lines, "[plugin] test.alpha: registered"));
		CHECK(LogContains(lines, "[plugin] test.beta: registered"));

		// 已加载时拒绝重新发现(保持现状)。
		CHECK(!manager.Discover(root / "engine", root / "project"));
		CHECK(manager.LoadedCount() == 2);

		// ⑧ 卸载:依赖保护 → Unregister 恰好一次 → 条目离开已加载列表;可重新加载。
		std::string error;
		CHECK(manager.Unload("test.alpha", context, &error) == PluginManager::Status::HasLoadedDependents);
		CHECK(error.find("test.beta") != std::string::npos);
		const size_t unloadMark = LogMark();
		CHECK(manager.Unload("test.beta", context, &error) == PluginManager::Status::Ok);
		CHECK(manager.Load("test.beta", context, &error) == PluginManager::Status::Ok);
		CHECK(manager.Unload("test.beta", context, &error) == PluginManager::Status::Ok);
		CHECK(manager.Unload("test.alpha", context, &error) == PluginManager::Status::Ok);
		CHECK(manager.LoadedCount() == 0);
		CHECK(manager.LoadOrder().empty());
		const PluginEntry* unloaded = manager.Find("test.alpha");
		CHECK(unloaded != nullptr && unloaded->State == PluginState::Unloaded && unloaded->Order == -1);

		const std::vector<std::string> unloadLines = LogSince(unloadMark);
		CHECK(LogCount(unloadLines, "[plugin] test.alpha: unregistered") == 1);
		CHECK(LogCount(unloadLines, "[plugin] test.beta: unregistered") == 2);
		CHECK(LogCount(unloadLines, "[plugin] unloaded id=test.alpha") == 1);
		CHECK(manager.Unload("test.alpha", context, &error) == PluginManager::Status::NotLoaded);
		CHECK(manager.Unload("test.missing", context, &error) == PluginManager::Status::NotFound);
		CHECK(manager.Load("test.missing", context, &error) == PluginManager::Status::NotFound);
	}

	// ②a 清单 abi 与宿主不等值 → 发现期干净拒绝。
	void CaseManifestAbiMismatch()
	{
		const fs::path root = FreshRoot("manifest-abi");
		ManifestFields alpha;
		alpha.Id = "test.alpha";
		alpha.Abi = "2";
		WritePlugin(root / "engine", "test.alpha", "WePluginTestAlpha", BuildYaml(alpha));

		WorldContext context;
		PluginManager manager;
		const size_t mark = LogMark();
		CHECK(manager.Discover(root / "engine", root / "project"));
		ExpectRejected(manager, "test.alpha", "abi");
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Rejected);
		CHECK(manager.LoadedCount() == 0);
		CHECK(error.find("abi") != std::string::npos);
		CHECK(LogContains(LogSince(mark), "[plugin] rejected id=test.alpha"));
		CHECK(!LogContains(LogSince(mark), "[plugin] test.alpha: registered"));
	}

	// ②c PLUG-CLEAN-2:清单 engine: 约束不满足 → 发现期干净拒绝(不进 loaded、不注册),
	// 诊断带声明值与宿主版本;依赖它的插件级联拒绝(不半可用)。
	void CaseEngineRequirementRejected()
	{
		const fs::path root = FreshRoot("engine-gate");
		ManifestFields old;
		old.Id = "test.oldengine";
		old.Engine = ">=99.0";
		WritePlugin(root / "engine", "test.oldengine", "WePluginTestAlpha", BuildYaml(old));
		ManifestFields dependent;
		dependent.Id = "test.dependent";
		dependent.Depends = { "test.oldengine" };
		WritePlugin(root / "project", "test.dependent", "WePluginTestBeta", BuildYaml(dependent));

		WorldContext context;
		PluginManager manager;
		const size_t mark = LogMark();
		CHECK(manager.Discover(root / "engine", root / "project"));
		const PluginEntry* entry = manager.Find("test.oldengine");
		CHECK(entry != nullptr);
		CHECK(entry->EngineSatisfied == false);
		ExpectRejected(manager, "test.oldengine", "engine requirement");
		ExpectRejected(manager, "test.oldengine", ">=99.0");
		ExpectRejected(manager, "test.oldengine",
			std::string("host engine ") + HostEngineVersion());
		ExpectRejected(manager, "test.dependent", "could not be loaded");
		CHECK(manager.LoadedCount() == 0);
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Rejected);
		CHECK(manager.LoadedCount() == 0);
		CHECK(error.find("engine requirement") != std::string::npos);
		CHECK(LogContains(LogSince(mark), "[plugin] rejected id=test.oldengine"));
		CHECK(!LogContains(LogSince(mark), "[plugin] test.oldengine: registered"));
	}

	// ②b 插件自报 ABI 与宿主不等值 → 加载期干净拒绝(Register 不被调用)。
	void CasePluginAbiMismatch()
	{
		const fs::path root = FreshRoot("plugin-abi");
		ManifestFields fields;
		fields.Id = "test.abi";
		WritePlugin(root / "engine", "test.abi", "WePluginTestAbiBad", BuildYaml(fields));

		WorldContext context;
		PluginManager manager;
		const size_t mark = LogMark();
		CHECK(manager.Discover(root / "engine", root / "project"));
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Rejected);
		ExpectRejected(manager, "test.abi", "ABI version mismatch");
		const PluginEntry* entry = manager.Find("test.abi");
		CHECK(entry != nullptr && entry->PluginAbi == WE_PLUGIN_ABI_VERSION + 1);
		CHECK(!LogContains(LogSince(mark), "[plugin] test.abi: registered"));
	}

	// ③a StructSize 小于宿主已知前缀 → 拒绝(不再读后面的字段 / 不调用 Register)。
	void CaseStructSizeMismatch()
	{
		const fs::path root = FreshRoot("struct-size");
		ManifestFields fields;
		fields.Id = "test.size";
		WritePlugin(root / "engine", "test.size", "WePluginTestSizeBad", BuildYaml(fields));

		WorldContext context;
		PluginManager manager;
		CHECK(manager.Discover(root / "engine", root / "project"));
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Rejected);
		ExpectRejected(manager, "test.size", "struct too small");
	}

	// ③b StructSize **更大** = 插件用更新的头编译(尾部字段追加、abi 不变)⇒ 接受并按 v1 前缀工作。
	// 这是 WePluginApi.h「字段只增不改号」的活体回归(2026-09-30 主 agent 裁决)。
	void CaseStructSizeLargerAccepted()
	{
		const fs::path root = FreshRoot("struct-size-large");
		ManifestFields fields;
		fields.Id = "test.size.large";
		WritePlugin(root / "engine", "test.size.large", "WePluginTestSizeLarge", BuildYaml(fields));

		WorldContext context;
		PluginManager manager;
		const size_t mark = LogMark();
		CHECK(manager.Discover(root / "engine", root / "project"));
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Ok);
		const PluginEntry* entry = manager.Find("test.size.large");
		CHECK(entry != nullptr && entry->State == PluginState::Loaded);
		CHECK(entry != nullptr && entry->PluginStructSize == sizeof(WePlugin) + 32);
		CHECK(LogContains(LogSince(mark), "[plugin] test.size.large: registered (larger struct accepted)"));
		manager.UnloadAll(context);
		CHECK(entry != nullptr && entry->State == PluginState::Unloaded);
	}

	// ③c 开发构建根:插件包没有自带 bin/ 时按宿主给的 dev 构建根解析产物(引擎插件在 dev 形态),
	// 并钉住 PluginEntry::ExportNames(插件管理器面板 / plugin.info 的数据源)。
	void CaseDevBinaryRootResolution()
	{
		const fs::path root = FreshRoot("dev-bin-root");
		const fs::path devRoot = root / "dev-build" / "plugins" / "Debug";
		fs::create_directories(devRoot);
		ManifestFields fields;
		fields.Id = "test.alpha";   // 目录名 = 产物名 = 插件 id 对应的 DLL 名
		WritePluginManifestOnly(root / "engine", "test.alpha", BuildYaml(fields));
		const std::string fileName = "test.alpha" + std::string(kLibraryExtension);
		std::error_code copyError;
		fs::copy_file(DllDirectory() / (std::string("WePluginTestAlpha") + kLibraryExtension),
			devRoot / fileName, fs::copy_options::overwrite_existing, copyError);
		CHECK(!copyError);

		WorldContext context;
		PluginManager manager;
		CHECK(manager.Discover(root / "engine", root / "project", { devRoot }));
		const PluginEntry* entry = manager.Find("test.alpha");
		CHECK(entry != nullptr && entry->Manifest.LibraryPath == devRoot / fileName);
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Ok);
		CHECK(entry != nullptr && entry->State == PluginState::Loaded);
		CHECK(entry != nullptr
			&& std::find(entry->ExportNames.begin(), entry->ExportNames.end(), "alpha.ping")
				!= entry->ExportNames.end());
		manager.UnloadAll(context);
	}

	// ④ 重复 id:后发现的拒绝 + 诊断(第一个条目保留)。
	void CaseDuplicateId()
	{
		const fs::path root = FreshRoot("duplicate-id");
		ManifestFields fields;
		fields.Id = "test.dup";
		WritePlugin(root / "engine", "test.dup-a", "WePluginTestAlpha", BuildYaml(fields));
		WritePlugin(root / "engine", "test.dup-b", "WePluginTestAlpha", BuildYaml(fields));

		PluginManager manager;
		CHECK(manager.Discover(root / "engine", root / "project"));
		CHECK(manager.Count() == 2);
		const PluginEntry* first = manager.Find("test.dup");
		CHECK(first != nullptr && first->State == PluginState::Discovered);
		CHECK(first->Manifest.Root.filename() == "test.dup-a");

		size_t rejected = 0;
		std::string diagnostic;
		for (const PluginEntry& entry : manager.Entries())
		{
			if (entry.State != PluginState::Rejected)
				continue;
			++rejected;
			diagnostic = entry.Diagnostic;
			CHECK(entry.Manifest.Root.filename() == "test.dup-b");
		}
		CHECK(rejected == 1);
		CHECK(diagnostic.find("duplicate plugin id 'test.dup'") != std::string::npos);
	}

	// ⑤ 缺依赖 + 依赖级联拒绝(不半可用)。
	void CaseMissingDependency()
	{
		const fs::path root = FreshRoot("missing-dependency");
		ManifestFields alpha;
		alpha.Id = "test.alpha";
		alpha.Depends = { "test.absent" };
		WritePlugin(root / "engine", "test.alpha", "WePluginTestAlpha", BuildYaml(alpha));
		ManifestFields beta;
		beta.Id = "test.beta";
		beta.Depends = { "test.alpha" };
		WritePlugin(root / "engine", "test.beta", "WePluginTestBeta", BuildYaml(beta));

		WorldContext context;
		PluginManager manager;
		CHECK(manager.Discover(root / "engine", root / "project"));
		ExpectRejected(manager, "test.alpha", "missing dependency 'test.absent'");
		ExpectRejected(manager, "test.beta", "dependency 'test.alpha' could not be loaded");
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Rejected);
		CHECK(manager.LoadedCount() == 0);
	}

	// ⑥ 依赖环 → 环内节点全部拒绝,诊断带可读链路。
	void CaseDependencyCycle()
	{
		const fs::path root = FreshRoot("cycle");
		ManifestFields alpha;
		alpha.Id = "test.alpha";
		alpha.Depends = { "test.beta" };
		WritePlugin(root / "engine", "test.alpha", "WePluginTestAlpha", BuildYaml(alpha));
		ManifestFields beta;
		beta.Id = "test.beta";
		beta.Depends = { "test.alpha" };
		WritePlugin(root / "engine", "test.beta", "WePluginTestBeta", BuildYaml(beta));

		WorldContext context;
		PluginManager manager;
		CHECK(manager.Discover(root / "engine", root / "project"));
		ExpectRejected(manager, "test.alpha", "dependency cycle");
		ExpectRejected(manager, "test.beta", "dependency cycle");
		const PluginEntry* entry = manager.Find("test.alpha");
		CHECK(entry != nullptr);
		CHECK(entry->Diagnostic.find("test.alpha") != std::string::npos);
		CHECK(entry->Diagnostic.find("test.beta") != std::string::npos);
		CHECK(entry->Diagnostic.find("->") != std::string::npos);
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Rejected);
		CHECK(manager.LoadedCount() == 0);
	}

	// ⑦ 位置与 scope 不符(引擎根写 project / 项目根写 engine)→ 拒绝。
	void CaseScopeMismatch()
	{
		const fs::path root = FreshRoot("scope");
		ManifestFields alpha;
		alpha.Id = "test.alpha";
		alpha.Scope = "project";
		WritePlugin(root / "engine", "test.alpha", "WePluginTestAlpha", BuildYaml(alpha));
		ManifestFields beta;
		beta.Id = "test.beta";
		beta.Scope = "engine";
		WritePlugin(root / "project", "test.beta", "WePluginTestBeta", BuildYaml(beta));

		WorldContext context;
		PluginManager manager;
		CHECK(manager.Discover(root / "engine", root / "project"));
		ExpectRejected(manager, "test.alpha", "does not match its location");
		ExpectRejected(manager, "test.beta", "does not match its location");
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Rejected);
		CHECK(manager.LoadedCount() == 0);
	}

	// 入口符号缺失(entry 指向不存在的导出)→ 拒绝。
	void CaseMissingEntrySymbol()
	{
		const fs::path root = FreshRoot("missing-entry");
		ManifestFields alpha;
		alpha.Id = "test.alpha";
		alpha.Entry = "WePluginQueryMissing";
		WritePlugin(root / "engine", "test.alpha", "WePluginTestAlpha", BuildYaml(alpha));

		WorldContext context;
		PluginManager manager;
		CHECK(manager.Discover(root / "engine", root / "project"));
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Rejected);
		ExpectRejected(manager, "test.alpha", "missing entry symbol");
	}

	// Register 为空 → 拒绝。
	void CaseNoRegister()
	{
		const fs::path root = FreshRoot("no-register");
		ManifestFields fields;
		fields.Id = "test.noregister";
		WritePlugin(root / "engine", "test.noregister", "WePluginTestNoRegister", BuildYaml(fields));

		WorldContext context;
		PluginManager manager;
		CHECK(manager.Discover(root / "engine", root / "project"));
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Rejected);
		ExpectRejected(manager, "test.noregister", "no Register entry");
	}

	// 清单 id 与 WePlugin.Id 不一致 → 拒绝。
	void CaseIdMismatch()
	{
		const fs::path root = FreshRoot("id-mismatch");
		ManifestFields fields;
		fields.Id = "test.idmismatch";
		WritePlugin(root / "engine", "test.idmismatch", "WePluginTestIdMismatch", BuildYaml(fields));

		WorldContext context;
		PluginManager manager;
		CHECK(manager.Discover(root / "engine", root / "project"));
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Rejected);
		ExpectRejected(manager, "test.idmismatch", "does not match manifest id");
	}

	// ⑨ 单个插件失败不影响其余插件:Register false / Register 抛异常都只拒绝自己。
	void CaseFailureIsolation()
	{
		const fs::path root = FreshRoot("failure-isolation");
		ManifestFields alpha;
		alpha.Id = "test.alpha";
		WritePlugin(root / "engine", "test.alpha", "WePluginTestAlpha", BuildYaml(alpha));
		ManifestFields fail;
		fail.Id = "test.fail";
		WritePlugin(root / "engine", "test.fail", "WePluginTestFailRegister", BuildYaml(fail));
		ManifestFields throwing;
		throwing.Id = "test.throw";
		WritePlugin(root / "engine", "test.throw", "WePluginTestThrowRegister", BuildYaml(throwing));
		ManifestFields beta;
		beta.Id = "test.beta";
		beta.Depends = { "test.alpha" };
		beta.Provides = { "asset.type" };
		WritePlugin(root / "project", "test.beta", "WePluginTestBeta", BuildYaml(beta));

		WorldContext context;
		PluginManager manager;
		const size_t mark = LogMark();
		CHECK(manager.Discover(root / "engine", root / "project"));
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Rejected);
		ExpectRejected(manager, "test.fail", "Register returned false");
		ExpectRejected(manager, "test.throw", "exception");

		// 其余插件照常加载:alpha 成功、依赖它的 beta 也成功(顺序 = 拓扑)。
		const PluginEntry* alphaEntry = manager.Find("test.alpha");
		CHECK(alphaEntry != nullptr && alphaEntry->State == PluginState::Loaded && alphaEntry->Order == 0);
		const PluginEntry* betaEntry = manager.Find("test.beta");
		CHECK(betaEntry != nullptr && betaEntry->State == PluginState::Loaded && betaEntry->Order == 1);
		CHECK(manager.LoadedCount() == 2);

		const std::vector<std::string> lines = LogSince(mark);
		CHECK(LogContains(lines, "[plugin] test.fail: register-attempted"));
		CHECK(!LogContains(lines, "[plugin] test.fail: unregister-called"));   // 契约:返回 false 不算已注册
		CHECK(LogContains(lines, "[plugin] test.throw: register-throw"));
		CHECK(LogContains(lines, "[plugin] test.throw: unregister-after-throw"));  // 抛异常 = best-effort 回滚

		manager.UnloadAll(context);
		CHECK(manager.LoadedCount() == 0);
	}

	// 声明比对是警告:清单 provides 少声明 / struct 自带最低引擎版本 → 不阻断加载。
	void CaseDeclarationWarnings()
	{
		const fs::path root = FreshRoot("warnings");
		ManifestFields alpha;
		alpha.Id = "test.alpha";
		alpha.Provides = { "cxx.exports" };   // struct 还声明了 test.extra + engine >=2.0
		WritePlugin(root / "engine", "test.alpha", "WePluginTestAlpha", BuildYaml(alpha));

		WorldContext context;
		PluginManager manager;
		const size_t mark = LogMark();
		CHECK(manager.Discover(root / "engine", root / "project"));
		CHECK(manager.LoadAll(context) == PluginManager::Status::Ok);
		CHECK(manager.LoadedCount() == 1);
		const std::vector<std::string> lines = LogSince(mark);
		CHECK(LogContains(lines, "provides mismatch"));
		CHECK(LogContains(lines, "engine mismatch"));
		manager.UnloadAll(context);
	}

	// 缺根 / 空根 = 0 个插件(不是错误);零插件 = 零开销。
	void CaseEmptyRoots()
	{
		const fs::path root = FreshRoot("empty");
		WorldContext context;
		PluginManager manager;
		CHECK(manager.Discover(root / "no-such-engine-root", root / "no-such-project-root"));
		CHECK(manager.Count() == 0);
		CHECK(manager.Entries().empty());
		CHECK(manager.LoadAll(context) == PluginManager::Status::Ok);
		CHECK(manager.LoadedCount() == 0);
		CHECK(manager.LoadOrder().empty());
	}

	// T2a①:宿主注册面 —— 资产类型(注册/重复拒绝/Create 真落盘/注销幂等)、
	// 导入器(注册/重复拒绝/Matches/指纹/Import 单产物)、卸载后注册面清零。
	void CaseHostApiRegistrationSurface()
	{
		const fs::path root = FreshRoot("host-api");
		ManifestFields fields;
		fields.Id = "test.hostapi";
		WritePlugin(root / "engine", "test.hostapi", "WePluginTestHostApi", BuildYaml(fields));

		WorldContext context;
		PluginManager manager;
		const size_t mark = LogMark();
		CHECK(manager.Discover(root / "engine", root / "project"));
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Ok);
		CHECK(manager.LoadedCount() == 1);

		const std::vector<std::string> lines = LogSince(mark);
		CHECK(LogContains(lines, "[plugin] test.hostapi: registered asset type 'test.hostapi.type'"));
		CHECK(LogContains(lines, "already registered"));              // 宿主:重复注册被拒(不覆盖)
		CHECK(LogContains(lines, "asset-type-duplicate-rejected"));   // 插件侧看到 false
		CHECK(LogContains(lines, "[plugin] test.hostapi: registered importer 'test.hostapi.importer'"));
		CHECK(LogContains(lines, "importer-duplicate-rejected"));

		// 资产类型:可查、字段完整;Create 经 C ABI 调回插件,userData 原样回传(内容即证据)。
		World::AssetTypeRegistry& registry = World::AssetTypeRegistry::Get();
		const World::AssetTypeDesc* type = registry.Find("test.hostapi.type");
		CHECK(type != nullptr);
		CHECK(type->Label == "Host API Test Type");
		CHECK(type->Extension == ".whostapi");
		CHECK(type->SortOrder == 77);
		CHECK(static_cast<bool>(type->Create));
		const fs::path createDir = FreshRoot("host-api-create");
		std::string createError;
		CHECK(type->Create(createDir, &createError));
		CHECK(fs::is_regular_file(createDir / "hostapi.created"));
		CHECK(ReadFileText(createDir / "hostapi.created") == "created-by-test.hostapi");

		// 导入器:插件导入面只含"插件贡献的那一份"(内置清单仍由 DefaultImporters() 提供)。
		std::vector<std::shared_ptr<World::Asset::IAssetImporter>> importers = manager.PluginImporters();
		CHECK(importers.size() == 1);
		const std::shared_ptr<World::Asset::IAssetImporter>& importer = importers.front();
		CHECK(importer->Name() == "test.hostapi.importer");
		CHECK(importer->Version() == 3);
		const fs::path source = createDir / "sample.whostapi";
		WriteFileText(source, "payload");
		CHECK(importer->Matches(source));
		CHECK(!importer->Matches(createDir / "sample.txt"));
		CHECK(importer->SettingsFingerprint(source) == 7);
		World::Asset::ImportRequest request;
		request.LogicalPath = "sample.whostapi";
		request.Source = source;
		std::error_code importError;
		const World::Asset::ImportResult imported = importer->Import(request, importError);
		CHECK(imported.Ok);
		CHECK(!importError);
		CHECK(imported.Error.empty());
		CHECK(std::string(imported.Data.begin(), imported.Data.end()) == "IMPORTED:payload");
		CHECK(imported.Outputs.empty());

		// 插件导入器接进既有 cook 面:插在内置兜底 PassThrough 之前 → 由它接手 .whostapi。
		{
			const fs::path cookRoot = FreshRoot("host-api-cook");
			const fs::path content = cookRoot / "content";
			fs::create_directories(content);
			WriteFileText(content / "note.whostapi", "cook-me");
			fs::create_directories(content / "plain");
			WriteFileText(content / "plain" / "note.txt", "untouched");

			World::Asset::ProjectManifest manifest;
			manifest.Id = "test.hostapi.cook";
			manifest.ContentRoot = "content";
			manifest.StartScene = "note.whostapi";
			const fs::path manifestPath = cookRoot / "project.we.yaml";
			std::string manifestError;
			CHECK(World::Asset::ProjectManifest::Save(manifestPath, manifest, &manifestError));

			std::vector<std::shared_ptr<World::Asset::IAssetImporter>> composition =
				World::Asset::DefaultImporters();
			const auto passThrough = std::find_if(composition.begin(), composition.end(),
				[](const std::shared_ptr<World::Asset::IAssetImporter>& item)
				{
					return item && item->Name() == "PassThrough";
				});
			CHECK(passThrough != composition.end());
			const std::vector<std::shared_ptr<World::Asset::IAssetImporter>> pluginImporters =
				manager.PluginImporters();
			composition.insert(passThrough, pluginImporters.begin(), pluginImporters.end());

			World::Asset::CookPipeline pipeline(std::move(composition));
			World::Asset::CookSummary summary;
			const fs::path outputDir = cookRoot / "cooked-output";
			pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Total == 2);
			CHECK(summary.Failed == 0);
			CHECK(summary.Changed == 2);
			CHECK(ReadFileText(outputDir / "cooked" / "note.whostapi") == "IMPORTED:cook-me");
			CHECK(ReadFileText(outputDir / "cooked" / "plain" / "note.txt") == "untouched");
		}

		// 卸载:插件自己注销(两次 = 幂等)→ 注册面干净,且没有兜底回收警告。
		const size_t unloadMark = LogMark();
		CHECK(manager.Unload("test.hostapi", context, &error) == PluginManager::Status::Ok);
		CHECK(registry.Find("test.hostapi.type") == nullptr);
		CHECK(manager.PluginImporters().empty());
		const std::vector<std::string> unloadLines = LogSince(unloadMark);
		CHECK(LogContains(unloadLines, "asset-type-unregistered idempotent"));
		CHECK(LogContains(unloadLines, "importer-unregistered idempotent"));
		CHECK(!LogContains(unloadLines, "force-removed"));
	}

	// T2a②:插件不自己注销(违约)→ 宿主在释放 DLL 前兜底回收并记警告;可重新加载(不留脏账)。
	void CaseLeakedRegistrationSweep()
	{
		const fs::path root = FreshRoot("leaky");
		ManifestFields fields;
		fields.Id = "test.leaky";
		WritePlugin(root / "engine", "test.leaky", "WePluginTestLeaky", BuildYaml(fields));

		WorldContext context;
		PluginManager manager;
		CHECK(manager.Discover(root / "engine", root / "project"));
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Ok);

		World::AssetTypeRegistry& registry = World::AssetTypeRegistry::Get();
		CHECK(registry.Find("test.leaky.type") != nullptr);
		CHECK(manager.PluginImporters().size() == 1);

		const size_t unloadMark = LogMark();
		CHECK(manager.Unload("test.leaky", context, &error) == PluginManager::Status::Ok);
		CHECK(registry.Find("test.leaky.type") == nullptr);
		CHECK(manager.PluginImporters().empty());
		const std::vector<std::string> unloadLines = LogSince(unloadMark);
		CHECK(LogContains(unloadLines,
			"asset type 'test.leaky.type' was not unregistered by the plugin; force-removed"));
		CHECK(LogContains(unloadLines,
			"importer 'test.leaky.importer' was not unregistered by the plugin; force-removed"));

		// 兜底回收后可以重新加载/卸载(证明不留脏账,而不是"只清了一半")。
		CHECK(manager.Load("test.leaky", context, &error) == PluginManager::Status::Ok);
		CHECK(registry.Find("test.leaky.type") != nullptr);
		manager.UnloadAll(context);
		CHECK(registry.Find("test.leaky.type") == nullptr);
		CHECK(manager.PluginImporters().empty());
	}

	// T2a③:LookupExport —— 宿主侧 API 与插件侧(经 WeHostApi 函数指针)命中/版本不足/
	// 名字不存在/插件不存在;返回的函数指针真的可调用;卸载后不再可查。
	void CaseLookupExport()
	{
		const fs::path root = FreshRoot("lookup");
		ManifestFields alpha;
		alpha.Id = "test.alpha";
		WritePlugin(root / "engine", "test.alpha", "WePluginTestAlpha", BuildYaml(alpha));
		ManifestFields lookup;
		lookup.Id = "test.lookup";
		lookup.Depends = { "test.alpha" };
		WritePlugin(root / "engine", "test.lookup", "WePluginTestLookup", BuildYaml(lookup));

		WorldContext context;
		PluginManager manager;
		const size_t mark = LogMark();
		CHECK(manager.Discover(root / "engine", root / "project"));
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Ok);
		CHECK(manager.LoadOrder() == (std::vector<std::string>{ "test.alpha", "test.lookup" }));

		// 宿主侧(与 WeHostApi::LookupExport 同一实现):命中 + 三种未命中。
		void* ping = manager.LookupExport("test.alpha", "alpha.ping", 1);
		CHECK(ping != nullptr);
		CHECK(manager.LookupExport("test.alpha", "alpha.ping", 2) == nullptr);
		CHECK(manager.LookupExport("test.alpha", "alpha.absent", 1) == nullptr);
		CHECK(manager.LookupExport("test.missing", "alpha.ping", 1) == nullptr);
		const auto pingFn = reinterpret_cast<int (*)()>(ping);
		CHECK(pingFn != nullptr && pingFn() == 42);

		// 插件侧(test.lookup 在 Register 里经宿主表查同一份导出表)。
		const std::vector<std::string> lines = LogSince(mark);
		CHECK(LogContains(lines, "test.lookup: lookup alpha.ping v1 hit"));
		CHECK(LogContains(lines, "test.lookup: lookup alpha.ping v2 miss (min version)"));
		CHECK(LogContains(lines, "test.lookup: lookup alpha.absent miss"));
		CHECK(LogContains(lines, "test.lookup: lookup test.absent miss"));

		manager.UnloadAll(context);
		CHECK(manager.LookupExport("test.alpha", "alpha.ping", 1) == nullptr);
	}

	// T2a④:示例引擎插件目录(plugins/hello-import)—— 真实清单被 Discover 解析;
	// 构建期可见示例插件目标时(tests/CMakeLists.txt 的 WE_PLUGIN_EXAMPLE_DLL)再做**真实加载**:
	// 注册的资产类型 / 导入器 / 导出表在宿主侧可见,卸载后回收干净。
	void CaseExamplePluginDiscoverable()
	{
		const fs::path example = fs::path(WE_PLUGIN_EXAMPLE_DIR);
		CHECK(fs::is_regular_file(example / "plugin.we.yaml"));
		CHECK(fs::is_regular_file(example / "src" / "HelloImport.cpp"));
		CHECK(fs::is_regular_file(example / "CMakeLists.txt"));

		const fs::path root = FreshRoot("example-plugin");
		fs::create_directories(root / "engine" / "hello-import");
		std::error_code copyError;
		fs::copy_file(example / "plugin.we.yaml",
			root / "engine" / "hello-import" / "plugin.we.yaml",
			fs::copy_options::overwrite_existing, copyError);
		CHECK(!copyError);

		PluginManager manager;
		CHECK(manager.Discover(root / "engine", root / "project"));
		CHECK(manager.Count() == 1);
		const PluginEntry* entry = manager.Find("engine.hello-import");
		CHECK(entry != nullptr);
		CHECK(entry->State == PluginState::Discovered);
		CHECK(entry->Manifest.Name == "Hello Import");
		CHECK(entry->Manifest.Abi == WE_PLUGIN_ABI_VERSION);
		CHECK(entry->Manifest.Entry == "WePluginQuery");
		CHECK(entry->Manifest.Scope == PluginScope::Engine);
		CHECK(entry->Manifest.Ship == PluginShipPolicy::Auto);
		CHECK(entry->Manifest.Provides == (std::vector<std::string>{
			"asset.type", "asset.importer", "cxx.exports" }));
		CHECK(entry->Manifest.LibraryPath
			== entry->Manifest.Root / "bin" / ("hello-import" + std::string(kLibraryExtension)));

#if defined(WE_PLUGIN_EXAMPLE_DLL)
		// 真 DLL 装进临时包的 bin/ 后按清单的 LibraryPath 约定加载(等价于发布布局)。
		const fs::path exampleDll = fs::path(WE_PLUGIN_EXAMPLE_DLL);
		CHECK(fs::is_regular_file(exampleDll));
		fs::create_directories(root / "engine" / "hello-import" / "bin");
		fs::copy_file(exampleDll,
			root / "engine" / "hello-import" / "bin" / ("hello-import" + std::string(kLibraryExtension)),
			fs::copy_options::overwrite_existing, copyError);
		CHECK(!copyError);

		WorldContext context;
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Ok);
		CHECK(manager.LoadOrder() == (std::vector<std::string>{ "engine.hello-import" }));
		const PluginEntry* loaded = manager.Find("engine.hello-import");
		CHECK(loaded != nullptr && loaded->State == PluginState::Loaded);

		// 资产类型(hello / .whello):Create 回调真落盘。
		World::AssetTypeRegistry& registry = World::AssetTypeRegistry::Get();
		const World::AssetTypeDesc* type = registry.Find("hello");
		CHECK(type != nullptr);
		CHECK(type->Extension == ".whello");
		CHECK(static_cast<bool>(type->Create));
		const fs::path createDir = FreshRoot("example-plugin-create");
		std::string createError;
		CHECK(type->Create(createDir, &createError));
		CHECK(fs::is_regular_file(createDir / "hello.whello"));
		CHECK(ReadFileText(createDir / "hello.whello").find("engine.hello-import") != std::string::npos);

		// 导入器(hello.whello):单产物 = "WHELLO1\n" + 源字节。
		const std::vector<std::shared_ptr<World::Asset::IAssetImporter>> pluginImporters =
			manager.PluginImporters();
		CHECK(pluginImporters.size() == 1);
		CHECK(pluginImporters.front()->Name() == "hello.whello");
		CHECK(pluginImporters.front()->Version() == 1);
		const fs::path source = createDir / "sample.whello";
		WriteFileText(source, "abc");
		CHECK(pluginImporters.front()->Matches(source));
		World::Asset::ImportRequest request;
		request.LogicalPath = "sample.whello";
		request.Source = source;
		std::error_code importError;
		const World::Asset::ImportResult imported = pluginImporters.front()->Import(request, importError);
		CHECK(imported.Ok);
		CHECK(std::string(imported.Data.begin(), imported.Data.end()) == "WHELLO1\nabc");

		// C++ 导出表(hello.version):宿主按 id + name + 版本查到真函数。
		void* version = manager.LookupExport("engine.hello-import", "hello.version", 1);
		CHECK(version != nullptr);
		CHECK(manager.LookupExport("engine.hello-import", "hello.version", 2) == nullptr);
		const auto versionFn = reinterpret_cast<uint32_t (*)()>(version);
		CHECK(versionFn != nullptr && versionFn() == 1);

		manager.UnloadAll(context);
		CHECK(registry.Find("hello") == nullptr);
		CHECK(manager.PluginImporters().empty());
		CHECK(manager.LookupExport("engine.hello-import", "hello.version", 1) == nullptr);
#else
		// 示例插件目标未参与本次构建(WORLD_BUILD_ENGINE_PLUGINS=OFF):只验证清单位于加载序列。
		CHECK(manager.LoadOrder().empty());   // LoadOrder 只含已加载插件
#endif
	}

	// T2b①:组件 schema 注册(ABI-only 测试插件 → SchemaRegistry)。
	//   * 注册可见:Find / ListByModule / List(Component) 命中,字段顺序与元数据(ReadOnly /
	//     Color / Choices / Doc)按插件描述映射;
	//   * 坏描述干净拒绝(不支持的 Kind / Size 与 Kind 不符 / ComponentId != 0),不半注册;
	//   * 重复 Id 拒绝(不覆盖);注销单个类型不影响同模块其余类型;注销不存在 = 幂等;
	//   * 序列化往返:与 `.wd` 写入/读回同一条 API(ReadStructValue → WriteStructValue),
	//     并按插件用 offsetof 声明的偏移逐字节断言宿主访问器写在了哪里;
	//   * 卸载后类型与注册账本无残留,可以重新加载。
	void CaseComponentSchemaRegistration()
	{
		const fs::path root = FreshRoot("component-schema");
		ManifestFields fields;
		fields.Id = "test.component";
		WritePlugin(root / "engine", "test.component", "WePluginTestComponent", BuildYaml(fields));

		WorldContext context;
		Schema::SchemaRegistry& schemas = context.Schemas();
		PluginManager manager;
		const size_t mark = LogMark();
		CHECK(manager.Discover(root / "engine", root / "project"));
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Ok);
		CHECK(manager.LoadedCount() == 1);

		const std::vector<std::string> lines = LogSince(mark);
		// 坏描述:宿主逐条给可读诊断 + 插件侧看到 false。
		CHECK(LogContains(lines, "unsupported kind"));
		CHECK(LogContains(lines, "does not match its kind"));
		CHECK(LogContains(lines, "no storage bridge"));
		CHECK(LogContains(lines, "unsupported-kind-rejected"));
		CHECK(LogContains(lines, "size-mismatch-rejected"));
		CHECK(LogContains(lines, "component-id-rejected"));
		// 正例:两个类型各自一条注册日志;同类型第二次 = 拒绝(不覆盖)。
		CHECK(LogContains(lines,
			"[plugin] test.component: registered component 'test.component.Health' (5 field(s))"));
		CHECK(LogContains(lines,
			"[plugin] test.component: registered component 'test.component.Shield' (1 field(s))"));
		CHECK(LogContains(lines, "already registered by this plugin; registration ignored"));
		CHECK(LogContains(lines, "component-duplicate-rejected"));

		// 注册表可见性 + 元数据映射。
		const Schema::TypeSchema* health = schemas.Find("test.component.Health");
		CHECK(health != nullptr);
		CHECK(health->Category == Schema::TypeCategory::Component);
		CHECK(health->DisplayName == "Plugin Health");
		CHECK(health->Storage == nullptr);   // T2b:schema-only(存储桥不在本版 ABI)
		const Schema::TypeSchema* shield = schemas.Find("test.component.Shield");
		CHECK(shield != nullptr && shield->Category == Schema::TypeCategory::Component);
		CHECK(schemas.ListByModule("test.component").size() == 2);
		CHECK(schemas.List(Schema::TypeCategory::Component).size() >= 2);

		CHECK(health->Fields.size() == 5);
		CHECK(health->Fields[0].Name == "Enabled");
		CHECK(health->Fields[0].K == Schema::Kind::Bool);
		CHECK(health->Fields[0].Id.Value == Schema::Fnv1a64("Enabled"));
		CHECK(health->Fields[0].Meta.ReadOnly == true);
		CHECK(health->Fields[0].Meta.Color == false);
		CHECK(health->Fields[1].Name == "Charges");
		CHECK(health->Fields[1].K == Schema::Kind::Int32);
		CHECK(health->Fields[2].Name == "Health");
		CHECK(health->Fields[2].K == Schema::Kind::Float);
		CHECK(health->Fields[2].Meta.Doc == "Hit points.");
		CHECK(health->Fields[3].Name == "Offset");
		CHECK(health->Fields[3].K == Schema::Kind::Vec3);
		CHECK(health->Fields[3].Meta.Color == true);
		CHECK(health->Fields[4].Name == "Tier");
		CHECK(health->Fields[4].K == Schema::Kind::UInt8);
		CHECK(health->Fields[4].Meta.Choices == (std::vector<std::string>{ "light", "heavy" }));

		// 序列化往返(.wd 的写/读走的就是这一对 API):先读成 Value,再写进一个新的实例。
		WePluginComponentFixture::HealthFixture source;
		source.Enabled = true;
		source.Charges = 7;
		source.Health = 42.5f;
		source.Offset = { 1.0f, 2.0f, 3.0f };
		source.Tier = 1;
		const Schema::Value written = Schema::ReadStructValue(*health, &source);
		const Schema::ValueMap* map = std::get_if<Schema::ValueMap>(&written);
		CHECK(map != nullptr);
		CHECK(std::get<bool>(map->at("Enabled")) == true);
		CHECK(std::get<int32_t>(map->at("Charges")) == 7);
		CHECK(std::get<float>(map->at("Health")) == 42.5f);
		CHECK(std::get<glm::vec3>(map->at("Offset")) == glm::vec3(1.0f, 2.0f, 3.0f));
		CHECK(std::get<uint8_t>(map->at("Tier")) == 1);

		WePluginComponentFixture::HealthFixture restored;
		CHECK(Schema::WriteStructValue(*health, &restored, written));
		CHECK(restored.Enabled == true && restored.Charges == 7);
		CHECK(restored.Health == 42.5f);
		CHECK(restored.Offset.X == 1.0f && restored.Offset.Y == 2.0f && restored.Offset.Z == 3.0f);
		CHECK(restored.Tier == 1);
		// 逐字节:宿主访问器写的是插件用 offsetof 声明的偏移(不是"自洽但错位"的往返)。
		const auto* raw = reinterpret_cast<const uint8_t*>(&restored);
		float rawHealth = 0.0f;
		std::memcpy(&rawHealth, raw + offsetof(WePluginComponentFixture::HealthFixture, Health),
			sizeof(float));
		CHECK(rawHealth == 42.5f);
		int32_t rawCharges = 0;
		std::memcpy(&rawCharges, raw + offsetof(WePluginComponentFixture::HealthFixture, Charges),
			sizeof(int32_t));
		CHECK(rawCharges == 7);
		CHECK(raw[offsetof(WePluginComponentFixture::HealthFixture, Enabled)] == 1);
		CHECK(raw[offsetof(WePluginComponentFixture::HealthFixture, Tier)] == 1);

		// 单字段类型同样可往返(用第二个类型,避免"只有一个类型能跑"的假象)。
		WePluginComponentFixture::ShieldFixture shieldSource;
		shieldSource.Shield = 12.5f;
		const Schema::Value shieldValue = Schema::ReadStructValue(*shield, &shieldSource);
		WePluginComponentFixture::ShieldFixture shieldRestored;
		CHECK(Schema::WriteStructValue(*shield, &shieldRestored, shieldValue));
		CHECK(shieldRestored.Shield == 12.5f);

		// 注销单个类型(经插件的导出回调,时机由单测控制):同模块其余类型不受影响。
		const auto unregisterFn = reinterpret_cast<bool (*)(const char*)>(
			manager.LookupExport("test.component", "component.unregister", 1));
		CHECK(unregisterFn != nullptr);
		const size_t partialMark = LogMark();
		CHECK(unregisterFn("test.component.Health"));
		CHECK(schemas.Find("test.component.Health") == nullptr);
		CHECK(schemas.Find("test.component.Shield") != nullptr);
		CHECK(schemas.ListByModule("test.component").size() == 1);
		CHECK(LogContains(LogSince(partialMark),
			"[plugin] test.component: unregistered component 'test.component.Health'"));

		// 注销幂等:本插件没注册过的 id = true 且不报错(别人的类型才是拒绝,见 T2b②)。
		const size_t idempotentMark = LogMark();
		CHECK(unregisterFn("test.component.Health"));
		CHECK(!LogContains(LogSince(idempotentMark), "is not owned by this plugin"));

		// 卸载:插件自己的 Unregister 会幂等地再注销两个类型;注册表与账本都不留残留。
		const size_t unloadMark = LogMark();
		CHECK(manager.Unload("test.component", context, &error) == PluginManager::Status::Ok);
		CHECK(schemas.Find("test.component.Health") == nullptr);
		CHECK(schemas.Find("test.component.Shield") == nullptr);
		CHECK(schemas.ListByModule("test.component").empty());
		const std::vector<std::string> unloadLines = LogSince(unloadMark);
		CHECK(LogContains(unloadLines, "component-unregistered idempotent"));
		CHECK(!LogContains(unloadLines, "force-removed"));

		// 不留脏账:重新加载后两个类型都能再注册(槽位/账本都可复用)。
		CHECK(manager.Load("test.component", context, &error) == PluginManager::Status::Ok);
		CHECK(schemas.Find("test.component.Health") != nullptr);
		CHECK(schemas.Find("test.component.Shield") != nullptr);
		CHECK(schemas.ListByModule("test.component").size() == 2);
		manager.UnloadAll(context);
		CHECK(schemas.ListByModule("test.component").empty());
	}

	// T2b②:组件注册账本的边界与兜底回收。
	//   * 别人的类型不能被本插件顺手注销(拒绝 + 类型保留);
	//   * 违约插件(不自己注销)→ 卸载时整模块移除 + WARN,且可重新加载(不留脏账);
	//   * 管理器析构(宿主没先 UnloadAll)同样必须清掉组件类型 —— 不留指向已释放 DLL 的类型。
	void CaseLeakedComponentSweep()
	{
		const fs::path root = FreshRoot("leaky-component");
		ManifestFields component;
		component.Id = "test.component";
		WritePlugin(root / "engine", "test.component", "WePluginTestComponent", BuildYaml(component));
		ManifestFields leaky;
		leaky.Id = "test.leakycomponent";
		WritePlugin(root / "engine", "test.leakycomponent", "WePluginTestLeakyComponent", BuildYaml(leaky));

		WorldContext context;
		Schema::SchemaRegistry& schemas = context.Schemas();
		{
			PluginManager manager;
			CHECK(manager.Discover(root / "engine", root / "project"));
			std::string error;
			CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Ok);
			CHECK(manager.LoadedCount() == 2);
			CHECK(schemas.Find("test.leakycomponent.Shield") != nullptr);

			// 归属保护:test.component 不能注销 test.leakycomponent 的类型。
			const auto unregisterFn = reinterpret_cast<bool (*)(const char*)>(
				manager.LookupExport("test.component", "component.unregister", 1));
			CHECK(unregisterFn != nullptr);
			const size_t crossMark = LogMark();
			CHECK(!unregisterFn("test.leakycomponent.Shield"));
			CHECK(LogContains(LogSince(crossMark), "is not owned by this plugin; unregister ignored"));
			CHECK(schemas.Find("test.leakycomponent.Shield") != nullptr);

			// 违约插件卸载:兜底回收整模块 + 可读警告。
			const size_t unloadMark = LogMark();
			CHECK(manager.Unload("test.leakycomponent", context, &error) == PluginManager::Status::Ok);
			CHECK(schemas.Find("test.leakycomponent.Shield") == nullptr);
			CHECK(schemas.ListByModule("test.leakycomponent").empty());
			const std::vector<std::string> unloadLines = LogSince(unloadMark);
			CHECK(LogContains(unloadLines,
				"component 'test.leakycomponent.Shield' was not unregistered by the plugin; force-removed"));

			// 兜底回收后可以重新加载(证明不留脏账)。
			CHECK(manager.Load("test.leakycomponent", context, &error) == PluginManager::Status::Ok);
			CHECK(schemas.Find("test.leakycomponent.Shield") != nullptr);

			// 管理器析构路径(故意不 UnloadAll):两个插件的组件类型都必须被移除。
		}
		CHECK(schemas.ListByModule("test.component").empty());
		CHECK(schemas.ListByModule("test.leakycomponent").empty());
		CHECK(schemas.Find("test.component.Health") == nullptr);
		CHECK(schemas.Find("test.leakycomponent.Shield") == nullptr);
	}

	// PLUG-T2c:用例级的核心日志静音。
	//
	// World.Plugins 的证据约定是"插件侧走宿主日志、用 World::Log::RecentLines 取回",
	// 而那份环形缓冲只有 400 行(Engine/src/World/Core/Log.cpp),既有用例的 mark/窗口
	// 断言依赖它 —— 新用例的日志会把窗口挤走。T2c 用例改为**全部走 API 断言**
	// (返回值 / 注册表 / 场景状态 / Unload 的可读 error),因此把核心日志临时关掉:
	// 既不挤掉既有用例的窗口,也不让本用例自己依赖日志(异常路径由 RAII 恢复)。
	class CoreLogMute
	{
	public:
		CoreLogMute()
		{
			if (const std::shared_ptr<spdlog::logger>& logger = World::Log::GetCoreLogger())
			{
				m_Logger = logger;
				m_Level = logger->level();
				logger->set_level(spdlog::level::off);
			}
		}
		~CoreLogMute()
		{
			if (m_Logger)
				m_Logger->set_level(m_Level);
		}
		CoreLogMute(const CoreLogMute&) = delete;
		CoreLogMute& operator=(const CoreLogMute&) = delete;

	private:
		std::shared_ptr<spdlog::logger> m_Logger;
		spdlog::level::level_enum m_Level = spdlog::level::trace;
	};

	// T2c:组件存储桥(WeComponentDesc::Size / Alignment → 宿主合成 entt blob 存储)。
	//   * 注册产物:TypeSchema.Storage != nullptr、Size = 插件声明的结构大小、组件 id 落在
	//     保留高位段,FindByComponentId 自洽;
	//   * 场景实体:走 schema/Entity 的按 id 通路新增 → HasComponent;blob 初始清零
	//     (与引擎组件的 AddComponent 同语义);
	//   * FieldSchema::Set/Get 在 blob 内按插件用 offsetof 声明的偏移读写(逐字节对照布局);
	//   * Copy(Scene::DuplicateEntity)/ CopyAll(Scene::CopyScene)与引擎组件同语义;
	//   * `.wd` 保存 → 重新加载 → 字段值一致;插件缺失时既有"未知组件 YAML 片段保留"不退化;
	//   * 移除组件 → 类型仍在、实例消失;
	//   * 有活实例时 Unload 干净拒绝(可读原因),清掉实例后 Unload 成功(槽位/绑定可复用);
	//   * 未声明 Size(0)的旧式 schema-only 组件行为不变(Storage == nullptr、Size == 0)。
	void CaseComponentStorageBridge()
	{
		// 本用例的核心日志全部静音(见 CoreLogMute):断言只看 API 结果。
		//   副作用提示:插件侧"坏声明被拒绝"的证据 = 插件 Register 整体成功
		//   (RegisterBadDescriptors 里任一坏声明被接受 ⇒ 插件拒绝加载 ⇒ LoadAll != Ok)。
		const CoreLogMute mute;
		const fs::path root = FreshRoot("component-storage");
		ManifestFields storage;
		storage.Id = "test.storage";
		WritePlugin(root / "engine", "test.storage", "WePluginTestComponentStorage", BuildYaml(storage));

		WorldContext context;
		Schema::SchemaRegistry& schemas = context.Schemas();
		PluginManager manager;
		CHECK(manager.Discover(root / "engine", root / "project"));
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Ok);
		CHECK(manager.LoadedCount() == 1);
		CHECK(schemas.ListByModule("test.storage").size() == 3);

		// ① 注册产物:存储绑定 + 声明大小 + 保留段 id。
		const Schema::TypeSchema* health = schemas.Find("test.storage.Health");
		CHECK(health != nullptr);
		CHECK(health->Category == Schema::TypeCategory::Component);
		CHECK(health->Size == sizeof(WePluginComponentFixture::HealthFixture));
		CHECK(health->Storage != nullptr);
		CHECK(health->Storage->Add != nullptr && health->Storage->Copy != nullptr
			&& health->Storage->CopyAll != nullptr);
		CHECK((health->Storage->ComponentId & 0x80000000u) != 0);
		CHECK(schemas.FindByComponentId(health->Storage->ComponentId) == health);
		const Schema::TypeSchema* shield = schemas.Find("test.storage.Shield");
		CHECK(shield != nullptr && shield->Storage != nullptr);
		CHECK(shield->Size == sizeof(WePluginComponentFixture::ShieldFixture));
		CHECK(shield->Storage->ComponentId != health->Storage->ComponentId);
		const uint32_t healthId = health->Storage->ComponentId;

		// ⑦ 旧式 schema-only 组件行为不变(Size/Alignment = 0 ⇒ 没有存储、不能挂到实体上)。
		const Schema::TypeSchema* legacySchema = schemas.Find("test.storage.Legacy");
		CHECK(legacySchema != nullptr);
		CHECK(legacySchema->Storage == nullptr);
		CHECK(legacySchema->Size == 0);
		CHECK(schemas.ListByModule("test.storage").size() == 3);

		// ② 场景实体:按 id 通路新增,blob 初始全零。
		World::Ref<World::Scene> scene = World::CreateRef<World::Scene>(context);
		World::Entity entity = World::Entity::CreateEntity(scene.get(), "Storage Probe");
		CHECK(!entity.HasComponent(healthId));
		entity.AddComponent(healthId);
		CHECK(entity.HasComponent(healthId));
		void* instance = entity.GetComponent(healthId);
		CHECK(instance != nullptr);
		const auto fieldValue = [&](uint32_t index)
		{
			return health->Fields[index].Get(instance);
		};
		CHECK(std::get<bool>(fieldValue(0)) == false);
		CHECK(std::get<int32_t>(fieldValue(1)) == 0);
		CHECK(std::get<float>(fieldValue(2)) == 0.0f);
		CHECK(std::get<uint8_t>(fieldValue(4)) == 0);

		// ③ FieldSchema::Set/Get 在 blob 内按插件声明的 offset 读写(逐字节对照插件布局)。
		WePluginComponentFixture::HealthFixture expected;
		expected.Enabled = true;
		expected.Charges = 7;
		expected.Health = 42.5f;
		expected.Offset = { 1.0f, 2.0f, 3.0f };
		expected.Tier = 1;
		health->Fields[0].Set(instance, Schema::Value(expected.Enabled));
		health->Fields[1].Set(instance, Schema::Value(expected.Charges));
		health->Fields[2].Set(instance, Schema::Value(expected.Health));
		health->Fields[3].Set(instance, Schema::Value(glm::vec3(1.0f, 2.0f, 3.0f)));
		health->Fields[4].Set(instance, Schema::Value(expected.Tier));
		CHECK(std::get<bool>(fieldValue(0)) == true);
		CHECK(std::get<int32_t>(fieldValue(1)) == 7);
		CHECK(std::get<float>(fieldValue(2)) == 42.5f);
		CHECK(std::get<glm::vec3>(fieldValue(3)) == glm::vec3(1.0f, 2.0f, 3.0f));
		CHECK(std::get<uint8_t>(fieldValue(4)) == 1);
		WePluginComponentFixture::HealthFixture raw;
		std::memcpy(&raw, instance, sizeof(raw));
		CHECK(raw.Enabled == true && raw.Charges == 7 && raw.Health == 42.5f && raw.Tier == 1);
		CHECK(raw.Offset.X == 1.0f && raw.Offset.Y == 2.0f && raw.Offset.Z == 3.0f);

		// ④ `.wd` 保存 → 重新加载 → 字段值一致(既有 schema 字段通路)。
		const fs::path scenePath = fs::temp_directory_path() / "worldengine-plugin-storage-test.wd";
		{
			World::SceneSerializer writer(scene);
			CHECK(writer.Serialize(scenePath.string()));
		}
		World::Ref<World::Scene> loaded = World::CreateRef<World::Scene>(context);
		{
			World::SceneSerializer reader(loaded);
			CHECK(reader.Deserialize(scenePath.string()));
			CHECK(reader.GetLastError().empty());
		}
		void* loadedInstance = nullptr;
		{
			const entt::registry& loadedRegistry = static_cast<const World::Scene&>(*loaded).GetRegistry();
			const auto* storage = loadedRegistry.storage(healthId);
			CHECK(storage != nullptr && storage->size() == 1);
			for (const entt::entity handle : loadedRegistry.view<World::UUIDComponent>())
				if (storage->contains(handle))
					loadedInstance = const_cast<void*>(storage->value(handle));
		}
		CHECK(loadedInstance != nullptr);
		CHECK(std::get<bool>(health->Fields[0].Get(loadedInstance)) == true);
		CHECK(std::get<int32_t>(health->Fields[1].Get(loadedInstance)) == 7);
		CHECK(std::get<float>(health->Fields[2].Get(loadedInstance)) == 42.5f);
		CHECK(std::get<glm::vec3>(health->Fields[3].Get(loadedInstance)) == glm::vec3(1.0f, 2.0f, 3.0f));
		CHECK(std::get<uint8_t>(health->Fields[4].Get(loadedInstance)) == 1);

		// ⑤ Copy / CopyAll 通路(与引擎组件同语义)+ 活实例查询本身。
		scene->DuplicateEntity(entity);                 // Storage->Copy
		World::Ref<World::Scene> cloned = World::CreateRef<World::Scene>(context);
		World::Scene::CopyScene(scene, cloned);         // Storage->CopyAll
		// 活实例 = 源场景 2(源实体 + DuplicateEntity 副本)+ 读档场景 1 + 克隆场景 2
		CHECK(World::Scene::CountLiveComponentInstances(healthId, &context) == 5);
		std::size_t copied = 0;
		{
			const entt::registry& sceneRegistry = static_cast<const World::Scene&>(*scene).GetRegistry();
			const auto* storage = sceneRegistry.storage(healthId);
			for (const entt::entity handle : sceneRegistry.view<World::UUIDComponent>())
			{
				if (!storage || !storage->contains(handle))
					continue;
				++copied;
				CHECK(std::get<float>(health->Fields[2].Get(storage->value(handle))) == 42.5f);
			}
		}
		CHECK(copied == 2);   // 源实体 + DuplicateEntity 副本
		{
			const entt::registry& cloneRegistry = static_cast<const World::Scene&>(*cloned).GetRegistry();
			const auto* storage = cloneRegistry.storage(healthId);
			CHECK(storage != nullptr && storage->size() == 2);
		}

		// ⑥ 有活实例时 Unload 干净拒绝(可读原因;类型/插件状态与插件回调都保持不变)。
		CHECK(manager.Unload("test.storage", context, &error) == PluginManager::Status::HasLiveInstances);
		CHECK(error.find("live component instances") != std::string::npos);
		CHECK(error.find("test.storage.Health") != std::string::npos);
		CHECK(schemas.Find("test.storage.Health") != nullptr);
		CHECK(schemas.Find("test.storage.Shield") != nullptr);
		CHECK(manager.LoadedCount() == 1);

		// 移除组件 → 类型仍在、实例消失(源场景)。
		entity.RemoveComponent(healthId);
		CHECK(!entity.HasComponent(healthId));
		CHECK(schemas.Find("test.storage.Health") != nullptr);
		// 源场景 1(副本)+ 读档场景 1 + 克隆场景 2 = 4
		CHECK(World::Scene::CountLiveComponentInstances(healthId, &context) == 4);

		// 销毁副本与读档场景;源场景还剩 DuplicateEntity 副本 —— 销毁它,但存储本身留在
		// 注册表里(空存储),这正是"注销类型后重新注册"要覆盖的复用路径。
		cloned.reset();
		loaded.reset();
		{
			const entt::registry& sceneRegistry = static_cast<const World::Scene&>(*scene).GetRegistry();
			const auto* storage = sceneRegistry.storage(healthId);
			std::vector<entt::entity> withHealth;
			for (const entt::entity handle : sceneRegistry.view<World::UUIDComponent>())
				if (storage && storage->contains(handle))
					withHealth.push_back(handle);
			CHECK(withHealth.size() == 1);
			for (const entt::entity handle : withHealth)
				World::Entity::DestroyEntity(scene.get(), World::Entity(scene.get(), handle));
		}
		CHECK(World::Scene::CountLiveComponentInstances(healthId, &context) == 0);

		// 单类型注销 + 重新注册(经插件导出;不重载 DLL,避免日志刷屏):
		// 槽位/绑定/存储 id 复用 —— 同档位同槽位 ⇒ 同 id,先前的空存储照样能用。
		const auto unregisterFn = reinterpret_cast<bool (*)(const char*)>(
			manager.LookupExport("test.storage", "storage.unregister", 1));
		const auto registerFn = reinterpret_cast<bool (*)()>(
			manager.LookupExport("test.storage", "storage.register-health", 1));
		CHECK(unregisterFn != nullptr && registerFn != nullptr);
		CHECK(unregisterFn("test.storage.Health"));
		CHECK(schemas.Find("test.storage.Health") == nullptr);
		CHECK(schemas.ListByModule("test.storage").size() == 2);   // Shield + schema-only Legacy
		CHECK(registerFn());
		const Schema::TypeSchema* reloaded = schemas.Find("test.storage.Health");
		CHECK(reloaded != nullptr && reloaded->Storage != nullptr);
		CHECK(reloaded->Storage->ComponentId == healthId);
		World::Entity probe = World::Entity::CreateEntity(scene.get(), "Second Probe");
		probe.AddComponent(reloaded->Storage->ComponentId);
		CHECK(probe.HasComponent(reloaded->Storage->ComponentId));
		reloaded->Fields[2].Set(probe.GetComponent(reloaded->Storage->ComponentId), Schema::Value(1.5f));
		CHECK(std::get<float>(reloaded->Fields[2].Get(probe.GetComponent(reloaded->Storage->ComponentId)))
			== 1.5f);

		// 注销同模块的另一个类型:其余类型保持可用(存储绑定的地址稳定)。
		CHECK(unregisterFn("test.storage.Shield"));
		CHECK(schemas.Find("test.storage.Shield") == nullptr);
		const Schema::TypeSchema* remaining = schemas.Find("test.storage.Health");
		CHECK(remaining != nullptr && remaining->Storage != nullptr);
		CHECK(remaining->Storage->ComponentId == healthId);
		CHECK(schemas.ListByModule("test.storage").size() == 2);   // Health + schema-only Legacy
		remaining->Fields[2].Set(probe.GetComponent(remaining->Storage->ComponentId),
			Schema::Value(2.5f));
		CHECK(std::get<float>(remaining->Fields[2].Get(
			probe.GetComponent(remaining->Storage->ComponentId))) == 2.5f);

		// 清实例后 Unload 成功(类型/回调/账本全部回收;插件自己的 Unregister 幂等)。
		World::Entity::DestroyEntity(scene.get(), probe);
		CHECK(World::Scene::CountLiveComponentInstances(healthId, &context) == 0);
		CHECK(manager.Unload("test.storage", context, &error) == PluginManager::Status::Ok);
		CHECK(schemas.Find("test.storage.Health") == nullptr);
		CHECK(schemas.Find("test.storage.Legacy") == nullptr);
		CHECK(schemas.ListByModule("test.storage").empty());
		CHECK(manager.LoadedCount() == 0);
		scene.reset();

		// 插件缺失:已存 `.wd` 里的插件组件 YAML 片段走既有"未知组件保留"路径(不退化)。
		World::Ref<World::Scene> missing = World::CreateRef<World::Scene>(context);
		{
			World::SceneSerializer reader(missing);
			CHECK(reader.Deserialize(scenePath.string()));
		}
		const fs::path missingPath = fs::temp_directory_path() / "worldengine-plugin-storage-unknown.wd";
		{
			World::SceneSerializer writer(missing);
			CHECK(writer.Serialize(missingPath.string()));
		}
		CHECK(ReadFileText(missingPath).find("test.storage.Health") != std::string::npos);
		fs::remove(missingPath);
		missing.reset();
		fs::remove(scenePath);
	}

	// ---- PLUG-T3b:编辑器扩展面(命令 / 面板)----------------------------------------
	//
	// 单测里的宿主替身:实现的接口与 Editor/src/WUI/PluginEditorHost.* 完全一致
	// (Engine 侧只声明接口,编辑器实现细节不在本套件的依赖里)。它验证的是
	// PluginManager 的账本 + 四条回收路径,不是编辑器渲染。
	class TestEditorHost final : public PluginEditorHost
	{
	public:
		bool RegisterEditorCommand(const std::string& pluginId, const WeEditorCommandDesc& desc,
			WeEditorCommandCallback callback, void* userData) override
		{
			const std::string name = "plugin.command." + pluginId + "." + (desc.Id ? desc.Id : "");
			if (Commands.count(name) > 0)
				return false;
			Commands.emplace(name, TestCommand { pluginId, desc.Id ? desc.Id : "", callback, userData });
			return true;
		}
		bool UnregisterEditorCommand(const std::string& pluginId, const std::string& id) override
		{
			Commands.erase("plugin.command." + pluginId + "." + id);
			return true;
		}
		bool RegisterEditorPanel(const std::string& pluginId, const WeEditorPanelDesc& desc,
			PluginEditorPanelRenderFn render, void* userData) override
		{
			const std::string id = "plugin.panel." + pluginId + "." + (desc.Id ? desc.Id : "");
			if (Panels.count(id) > 0)
				return false;
			Panels.emplace(id, TestPanel { pluginId, desc.Id ? desc.Id : "", desc.Title ? desc.Title : "", render, userData });
			return true;
		}
		bool UnregisterEditorPanel(const std::string& pluginId, const std::string& id) override
		{
			Panels.erase("plugin.panel." + pluginId + "." + id);
			return true;
		}
		int RenderEditorPanelSurface(const PluginEditorPanel& panel, WeEditorPanelDrawFn draw,
			void* userData) override
		{
			if (!draw)
				return -1;
			// 最小小组件表:探针不画真实 UI,只证明宿主→插件 Draw 的调用路径与句柄有效。
			WeEditorUiApi ui;
			ui.PluginId = panel.PluginId.c_str();
			ui.UserData = this;
			ui.Label = &TestEditorHost::UiLabel;
			ui.Separator = &TestEditorHost::UiSeparator;
			ui.Checkbox = &TestEditorHost::UiCheckbox;
			ui.Button = &TestEditorHost::UiButton;
			ui.InvokeCommand = &TestEditorHost::UiInvokeCommand;
			RenderedPanels.push_back(panel.DisplayId);
			return (draw)(userData, &ui, this);
		}

		struct TestCommand
		{
			std::string PluginId;
			std::string Id;
			WeEditorCommandCallback Callback = nullptr;
			void* UserData = nullptr;
		};
		struct TestPanel
		{
			std::string PluginId;
			std::string Id;
			std::string Title;
			PluginEditorPanelRenderFn Render = nullptr;
			void* UserData = nullptr;
		};

		std::map<std::string, TestCommand> Commands;
		std::map<std::string, TestPanel> Panels;
		std::vector<std::string> RenderedPanels;

	private:
		// 面板 Draw 收到的小组件表:这些回调只记录"被插件调到过",不做真实绘制。
		static void UiLabel(void*, const char*) {}
		static void UiSeparator(void*) {}
		static bool UiCheckbox(void*, const char*, const char*, bool*) { return false; }
		static bool UiButton(void*, const char*, const char*) { return false; }
		static bool UiInvokeCommand(void*, const char*) { return false; }
	};

	// T3b①:命令 / 面板注册 + 账本可见 + 重复拒绝 + 触发计数 + 注销幂等。
	void CaseEditorExtensionRegistration()
	{
		const fs::path root = FreshRoot("editor-ui");
		ManifestFields fields;
		fields.Id = "test.editorui";
		WritePlugin(root / "engine", "test.editorui", "WePluginTestEditorUi", BuildYaml(fields));

		WorldContext context;
		std::string error;
		// 宿主先于管理器声明 = 比管理器活得久(PluginManager 的宿主指针生命周期契约;
		// 生产侧由 EditorLayer::ShutdownPlugins 的 SetEditorHost(nullptr) 保证同一件事)。
		TestEditorHost host;
		{
			PluginManager manager;
			// ①a 未接线编辑器宿主:注册被干净拒绝(false + 可读诊断),插件加载失败。
			{
				const size_t mark = LogMark();
				CHECK(manager.Discover(root / "engine", root / "project"));
				CHECK(manager.Load("test.editorui", context, &error) == PluginManager::Status::Rejected);
				CHECK(LogContains(LogSince(mark), "no editor host is wired"));
				CHECK(manager.EditorCommands().empty());
				CHECK(manager.EditorPanels().empty());
			}

			manager.SetEditorHost(&host);
			const size_t mark = LogMark();
			CHECK(manager.Load("test.editorui", context, &error) == PluginManager::Status::Ok);
			CHECK(manager.LoadedCount() == 1);

			const std::vector<std::string> lines = LogSince(mark);
			CHECK(LogContains(lines, "editor-size-rejected"));
			CHECK(LogContains(lines, "editor-empty-rejected"));
			CHECK(LogContains(lines, "editor-nocallback-rejected"));
			CHECK(LogContains(lines, "editor-command-duplicate-rejected"));
			CHECK(LogContains(lines, "editor-panel-duplicate-rejected"));
			CHECK(LogContains(lines, "[plugin] test.editorui: registered editor command 'test.editorui.ping'"));
			CHECK(LogContains(lines, "[plugin] test.editorui: registered editor panel 'test.editorui.panel'"));

			// 账本可见(命令 / 面板各 2 / 1 条;命令名带完整命名空间)。
			const std::vector<PluginEditorCommand> commands = manager.EditorCommands();
			CHECK(commands.size() == 2);
			CHECK(commands[0].PluginId == "test.editorui");
			CHECK(commands[0].Id == "test.editorui.ping");
			CHECK(commands[0].Label == "Editor UI Test Ping");
			CHECK(commands[0].CommandName == "plugin.command.test.editorui.test.editorui.ping");
			CHECK(commands[0].InvokeCount == 0);
			const std::vector<PluginEditorPanel> panels = manager.EditorPanels();
			CHECK(panels.size() == 1);
			CHECK(panels[0].DisplayId == "plugin.panel.test.editorui.test.editorui.panel");
			CHECK(panels[0].Title == "Editor UI Test Panel");
			CHECK(host.Commands.size() == 2);
			CHECK(host.Panels.size() == 1);

			// 触发命令(两种写法都命中同一条);计数 +1,插件回调被真调到。
			const size_t invokeMark = LogMark();
			CHECK(manager.InvokeEditorCommand("plugin.command.test.editorui.test.editorui.ping"));
			CHECK(manager.InvokeEditorCommand("test.editorui.ping"));
			const std::vector<std::string> invokeLines = LogSince(invokeMark);
			CHECK(LogContains(invokeLines, "plugin command invoked count=1"));
			CHECK(LogContains(invokeLines, "plugin command invoked count=2"));
			CHECK(LogContains(invokeLines,
				"[plugin] test.editorui: editor command 'test.editorui.ping' invoked (count=2)"));
			CHECK(manager.InvokeEditorCommand("no.such.command") == false);
			const std::vector<PluginEditorCommand> afterInvoke = manager.EditorCommands();
			CHECK(afterInvoke.size() == 2);
			CHECK(afterInvoke[0].InvokeCount == 2);

			// 面板渲染路径:宿主 Draw 被调(小组件表非空;插件面板回调不违约)。
			CHECK(manager.RenderEditorPanel("plugin.panel.test.editorui.test.editorui.panel") == 0);
			CHECK(host.RenderedPanels.size() == 1);
			CHECK(manager.RenderEditorPanel("plugin.panel.test.editorui.nope") != 0);

			// 导出注销:按需注销一条命令 + 面板,再次调用幂等(宿主注册表同步移除)。
			const auto unregisterFn = reinterpret_cast<bool (*)(const char*)>(
				manager.LookupExport("test.editorui", "editorui.unregister", 1));
			CHECK(unregisterFn != nullptr);
			CHECK(unregisterFn("test.editorui.ping"));
			CHECK(host.Commands.count("plugin.command.test.editorui.test.editorui.ping") == 0);
			CHECK(manager.EditorCommands().size() == 1);
			CHECK(unregisterFn("panel:test.editorui.panel"));
			CHECK(host.Panels.empty());
			CHECK(manager.EditorPanels().empty());

			// 卸载:剩下的那条命令由插件自己注销(幂等),账本清空。
			const size_t unloadMark = LogMark();
			CHECK(manager.Unload("test.editorui", context, &error) == PluginManager::Status::Ok);
			CHECK(manager.EditorCommands().empty());
			CHECK(manager.EditorPanels().empty());
			CHECK(host.Commands.empty());
			CHECK(host.Panels.empty());
			const std::vector<std::string> unloadLines = LogSince(unloadMark);
			CHECK(LogContains(unloadLines, "editor-unregistered idempotent"));

			// 重新加载:注册面仍可重入(证明不留脏账)。
			CHECK(manager.Load("test.editorui", context, &error) == PluginManager::Status::Ok);
			CHECK(manager.EditorCommands().size() == 2);
			CHECK(manager.EditorPanels().size() == 1);
			manager.UnloadAll(context);
			manager.SetEditorHost(nullptr);
			CHECK(manager.EditorCommands().empty());
			CHECK(manager.EditorPanels().empty());
		}
		CHECK(host.Commands.empty());
		CHECK(host.Panels.empty());
	}

	// T3b②:违约回收三条路径 —— Register 返回 false / Register 抛异常 / 卸载与管理器析构。
	void CaseLeakedEditorExtensionSweep()
	{
		const fs::path root = FreshRoot("leaky-editor-ui");
		ManifestFields leaky;
		leaky.Id = "test.leakyeditorui";
		WritePlugin(root / "engine", "test.leakyeditorui", "WePluginTestLeakyEditorUi", BuildYaml(leaky));
		ManifestFields throwing;
		throwing.Id = "test.throwingeditorui";
		WritePlugin(root / "engine", "test.throwingeditorui", "WePluginTestThrowEditorUi",
			BuildYaml(throwing));

		WorldContext context;
		PluginManager manager;
		TestEditorHost host;
		manager.SetEditorHost(&host);
		std::string error;
		CHECK(manager.Discover(root / "engine", root / "project"));

		// ②a Register 抛异常:best-effort Unregister + 兜底回收,注册表不留条目。
		const size_t throwMark = LogMark();
		CHECK(manager.Load("test.throwingeditorui", context, &error) == PluginManager::Status::Rejected);
		CHECK(error.find("exception") != std::string::npos);
		CHECK(host.Commands.empty());
		CHECK(host.Panels.empty());
		CHECK(manager.EditorCommands().empty());
		CHECK(manager.EditorPanels().empty());
		const std::vector<std::string> throwLines = LogSince(throwMark);
		CHECK(LogContains(throwLines, "registered then throwing"));
		CHECK(LogContains(throwLines, "unregister-after-editor-throw"));
		CHECK(LogContains(throwLines,
			"editor command 'test.throwingeditorui.ping' was not unregistered by the plugin; force-removed"));
		CHECK(LogContains(throwLines,
			"editor panel 'test.throwingeditorui.panel' was not unregistered by the plugin; force-removed"));

		// ②b 违约插件(Unregister 不清):卸载时兜底回收 + 可读警告。
		CHECK(manager.Load("test.leakyeditorui", context, &error) == PluginManager::Status::Ok);
		CHECK(manager.EditorCommands().size() == 1);
		CHECK(manager.EditorPanels().size() == 1);
		const size_t unloadMark = LogMark();
		CHECK(manager.Unload("test.leakyeditorui", context, &error) == PluginManager::Status::Ok);
		const std::vector<std::string> unloadLines = LogSince(unloadMark);
		CHECK(LogContains(unloadLines,
			"editor command 'test.leakyeditorui.ping' was not unregistered by the plugin; force-removed"));
		CHECK(LogContains(unloadLines,
			"editor panel 'test.leakyeditorui.panel' was not unregistered by the plugin; force-removed"));
		CHECK(manager.EditorCommands().empty());
		CHECK(manager.EditorPanels().empty());
		CHECK(host.Commands.empty());
		CHECK(host.Panels.empty());

		// ②c 管理器析构路径(故意不 UnloadAll):命令 / 面板同样必须被移除。
		{
			// 声明顺序 = 宿主先于管理器(宿主活得久;见上面的生命周期契约)。
			TestEditorHost scopedHost;
			PluginManager scoped;
			scoped.SetEditorHost(&scopedHost);
			CHECK(scoped.Discover(root / "engine", root / "project"));
			CHECK(scoped.Load("test.leakyeditorui", context, &error) == PluginManager::Status::Ok);
			CHECK(scoped.EditorCommands().size() == 1);
			CHECK(scoped.EditorPanels().size() == 1);
		}
		CHECK(host.Commands.empty());
		CHECK(host.Panels.empty());
	}

	// ---- PLUG-T4:脚本函数库(全局 Luau 函数)-------------------------------------------
	//
	// 覆盖:
	//   * 两个 ABI-only 测试插件注册脚本函数 → 账本可见、坏描述/重复干净拒绝、
	//     跨插件归属保护(别的插件不能注销不属于自己的函数);
	//   * 同一进程内用真 Luau VM 调用:`hello.ping(2,3)==5`、`hello.echo`、共享命名空间
	//     `hello.two`、`zeta.mul`;插件抛异常 = Lua error(pcall 可捕,不把 VM 带走);
	//   * 装载顺序 ≠ 渲染顺序:目录名让 test.scripttwo 先加载,存根仍按 (插件 id, 函数名) 升序;
	//   * 存根确定性:两次渲染逐字节一致;零插件时"带插件表的重载"与"不带插件表的重载"
	//     逐字节一致(入库夹具的漂移门禁由 World.ScriptWorkflow 守着);
	//   * 装卸与 VM 全局表联动:卸载 = 成员消失、命名空间清空后整表移除;重载 = 立即重绑。
	void CaseScriptLibraryRegistration()
	{
		using World::ScriptServiceBinding;
		const fs::path root = FreshRoot("script-library");
		ManifestFields lib;
		lib.Id = "test.scriptlib";
		// 目录名故意排在 scripttwo 之后:装载顺序 = scripttwo → scriptlib,
		// 而存根/绑定顺序必须仍按 (插件 id, 函数名) 升序。
		WritePlugin(root / "engine", "zzz-scriptlib", "WePluginTestScriptLib", BuildYaml(lib));
		ManifestFields two;
		two.Id = "test.scripttwo";
		WritePlugin(root / "engine", "aaa-scripttwo", "WePluginTestScriptTwo", BuildYaml(two));

		WorldContext context;
		PluginManager manager;
		const size_t mark = LogMark();
		CHECK(manager.Discover(root / "engine", root / "project"));
		std::string error;
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Ok);
		CHECK(manager.LoadOrder() == (std::vector<std::string>{ "test.scripttwo", "test.scriptlib" }));

		// ① 坏描述全部干净拒绝 + 真注册成功(插件侧证据经 host.Log 回传)。
		const std::vector<std::string> lines = LogSince(mark);
		CHECK(LogContains(lines, "scriptlib-size-rejected"));
		CHECK(LogContains(lines, "scriptlib-abi-rejected"));
		CHECK(LogContains(lines, "scriptlib-empty-rejected"));
		CHECK(LogContains(lines, "scriptlib-nonamespace-rejected"));
		CHECK(LogContains(lines, "scriptlib-badsig-rejected"));
		CHECK(LogContains(lines, "scriptlib-nocallback-rejected"));
		CHECK(LogContains(lines, "scriptlib-duplicate-rejected"));
		CHECK(LogContains(lines, "[plugin] test.scriptlib: registered script function 'hello.ping'"));
		CHECK(LogContains(lines, "[plugin] test.scriptlib: registered script function 'hello.echo'"));
		CHECK(LogContains(lines, "[plugin] test.scriptlib: registered script function 'hello.boom'"));
		CHECK(LogContains(lines, "[plugin] test.scripttwo: registered script function 'hello.two'"));
		CHECK(LogContains(lines, "[plugin] test.scripttwo: registered script function 'zeta.mul'"));

		// ② 账本可见 + 确定性快照顺序 =(插件 id, 函数名)。
		CHECK(World::PluginScriptLibrary::Count() == 5);
		CHECK(World::PluginScriptLibrary::OwnerOf("hello.ping") == "test.scriptlib");
		CHECK(World::PluginScriptLibrary::OwnerOf("hello.two") == "test.scripttwo");
		CHECK(manager.ScriptFunctions().size() == 5);
		PluginManager::PluginScriptFunction info;
		CHECK(manager.FindScriptFunction("test.scriptlib", "hello.ping", &info));
		CHECK(info.Namespace == "hello" && info.Member == "ping");
		CHECK(info.Signature == "(left: number, right: number): number");
		CHECK(!info.Doc.empty());
		CHECK(!manager.FindScriptFunction("test.scriptlib", "hello.two", &info));
		const std::vector<World::PluginScriptFunctionInfo> snapshot = World::PluginScriptLibrary::Snapshot();
		CHECK(snapshot.size() == 5);
		CHECK(snapshot.front().PluginId == "test.scriptlib");
		CHECK(snapshot.front().Name == "hello.boom");   // (pluginId, name) 升序的第一条

		// ③ 存根渲染:插件块取账本快照(与运行时绑定同一份),顺序确定、两次逐字节一致。
		std::vector<World::LuaTypeReflection> types;
		const std::vector<const Schema::TypeSchema*> noComponents;
		const std::vector<const ScriptServiceBinding*> noServices;
		std::string first;
		std::string second;
		CHECK(World::LuaStubGenerator::Render(types, noComponents, noServices, noServices,
			World::PluginScriptLibrary::StubTables(), first, error));
		CHECK(World::LuaStubGenerator::Render(types, noComponents, noServices, noServices,
			World::PluginScriptLibrary::StubTables(), second, error));
		CHECK(first == second);
		CHECK(first.find("---@class hello") != std::string::npos);
		CHECK(first.find("---@class zeta") != std::string::npos);
		CHECK(first.find("function hello.ping(left, right) end") != std::string::npos);
		CHECK(first.find("---@param left number") != std::string::npos);
		CHECK(first.find("---@return number") != std::string::npos);
		CHECK(first.find("function hello.two() end") != std::string::npos);
		CHECK(first.find("function zeta.mul(left, right) end") != std::string::npos);
		// 顺序 =(插件 id, 函数名)升序,而不是装载顺序(scripttwo 先加载)。
		CHECK(first.find("function hello.boom") < first.find("function hello.echo"));
		CHECK(first.find("function hello.echo") < first.find("function hello.ping"));
		CHECK(first.find("function hello.ping") < first.find("function hello.two"));
		CHECK(first.find("function hello.two") < first.find("function zeta.mul"));

		// ④ 真 Luau VM(VM 在插件注册之后才初始化 ⇒ Init 统一绑定账本里的函数)。
		World::ScriptEngine::Init();
		CHECK(World::ScriptEngine::IsInitialized());
		auto runLua = [](const std::string& source, const char* chunk)
		{
			std::string luaError;
			if (!World::ScriptEngine::GetState().RunString(source, chunk, &luaError))
				throw std::runtime_error(std::string(chunk) + ": " + luaError);
		};
		auto readNumber = [](const char* name)
		{
			double value = 0.0;
			CHECK(World::ScriptEngine::GetState().GetGlobal(name).AsNumber(&value));
			return value;
		};
		auto readBool = [](const char* name)
		{
			bool value = false;
			CHECK(World::ScriptEngine::GetState().GetGlobal(name).AsBool(&value));
			return value;
		};
		runLua(
			"plugin_ping = hello.ping(2, 3)\n"
			"plugin_echo = hello.echo(\"hi\")\n"
			"plugin_two = hello.two()\n"
			"plugin_mul = zeta.mul(6, 7)\n"
			"local ok, err = pcall(hello.boom)\n"
			"plugin_boom_ok = ok\n"
			"plugin_boom_error = err\n",
			"plugin-script-test");
		CHECK(readNumber("plugin_ping") == 5.0);
		std::string echoed;
		CHECK(World::ScriptEngine::GetState().GetGlobal("plugin_echo").AsString(&echoed));
		CHECK(echoed == "hi!");
		CHECK(readNumber("plugin_two") == 2.0);
		CHECK(readNumber("plugin_mul") == 42.0);
		CHECK(readBool("plugin_boom_ok") == false);
		std::string boomError;
		CHECK(World::ScriptEngine::GetState().GetGlobal("plugin_boom_error").AsString(&boomError));
		CHECK(boomError.find("boom from the script library plugin") != std::string::npos);

		// ⑤ 卸载(VM 存活):违约插件(不自己注销)被兜底回收,VM 全局表同步清成员;
		//    共享命名空间在还有成员时保留。
		const size_t unloadTwoMark = LogMark();
		CHECK(manager.Unload("test.scripttwo", context, &error) == PluginManager::Status::Ok);
		const std::vector<std::string> unloadTwoLines = LogSince(unloadTwoMark);
		CHECK(LogContains(unloadTwoLines, "scripttwo-leaves-its-functions-registered (contract violation test)"));
		CHECK(LogContains(unloadTwoLines,
			"script function 'hello.two' was not unregistered by the plugin; force-removed"));
		CHECK(LogContains(unloadTwoLines,
			"script function 'zeta.mul' was not unregistered by the plugin; force-removed"));
		CHECK(World::PluginScriptLibrary::Count() == 3);
		runLua(
			"plugin_ping_after = hello.ping(1, 1)\n"
			"plugin_two_gone = (hello.two == nil)\n"
			"plugin_zeta_gone = (zeta == nil)\n",
			"plugin-script-unload");
		CHECK(readNumber("plugin_ping_after") == 2.0);
		CHECK(readBool("plugin_two_gone"));
		CHECK(readBool("plugin_zeta_gone"));

		// ⑥ VM 存活时重载:注册即绑定(不等 Init)。
		CHECK(manager.Load("test.scripttwo", context, &error) == PluginManager::Status::Ok);
		CHECK(manager.FindScriptFunction("test.scripttwo", "hello.two", &info));
		runLua(
			"plugin_two_reloaded = hello.two()\n"
			"plugin_mul_reloaded = zeta.mul(2, 5)\n",
			"plugin-script-reload");
		CHECK(readNumber("plugin_two_reloaded") == 2.0);
		CHECK(readNumber("plugin_mul_reloaded") == 10.0);

		// ⑦ 归属保护:test.scripttwo 不能注销 test.scriptlib 的 hello.ping。
		const auto unregisterForeign = reinterpret_cast<bool (*)()>(
			manager.LookupExport("test.scripttwo", "scripttwo.unregister-foreign", 1));
		CHECK(unregisterForeign != nullptr);
		const size_t crossMark = LogMark();
		CHECK(unregisterForeign());
		CHECK(LogContains(LogSince(crossMark), "scripttwo-cross-owner-rejected"));
		CHECK(LogContains(LogSince(crossMark),
			"[plugin] test.scripttwo: script function 'hello.ping' is not owned by this plugin; unregister ignored"));
		CHECK(World::PluginScriptLibrary::OwnerOf("hello.ping") == "test.scriptlib");

		// ⑧ 插件自己的按需注销(经导出回调)+ 幂等;VM 全局表同步清成员。
		const auto unregisterEcho = reinterpret_cast<bool (*)(const char*)>(
			manager.LookupExport("test.scriptlib", "scriptlib.unregister", 1));
		CHECK(unregisterEcho != nullptr);
		const size_t echoMark = LogMark();
		CHECK(unregisterEcho("hello.echo"));
		CHECK(LogContains(LogSince(echoMark), "scriptlib.unregister hello.echo -> idempotent-ok"));
		CHECK(!manager.FindScriptFunction("test.scriptlib", "hello.echo", &info));
		runLua("plugin_echo_gone = (hello.echo == nil)\n", "plugin-script-unregister");
		CHECK(readBool("plugin_echo_gone"));

		// ⑨ 收尾:scripttwo(违约)再卸载一次 → 强删;scriptlib 自己注销干净(无 force-removed);
		//    hello 表在最后一个成员离开后整体移除。
		CHECK(manager.Unload("test.scripttwo", context, &error) == PluginManager::Status::Ok);
		const size_t unloadLibMark = LogMark();
		CHECK(manager.Unload("test.scriptlib", context, &error) == PluginManager::Status::Ok);
		const std::vector<std::string> unloadLibLines = LogSince(unloadLibMark);
		CHECK(LogContains(unloadLibLines, "scriptlib-unregistered idempotent"));
		CHECK(!LogContains(unloadLibLines, "was not unregistered by the plugin"));
		CHECK(World::PluginScriptLibrary::Count() == 0);
		CHECK(manager.ScriptFunctions().empty());
		runLua(
			"plugin_hello_gone = (hello == nil)\n"
			"plugin_zeta_gone_final = (zeta == nil)\n",
			"plugin-script-final");
		CHECK(readBool("plugin_hello_gone"));
		CHECK(readBool("plugin_zeta_gone_final"));

		// ⑩ 零插件等价路径:两个重载逐字节一致(漂移门禁的"无插件"口径不受影响)。
		CHECK(World::PluginScriptLibrary::StubTables().empty());
		std::string withoutTables;
		std::string withEmptyTables;
		CHECK(World::LuaStubGenerator::Render(types, noComponents, noServices, noServices, withoutTables, error));
		CHECK(World::LuaStubGenerator::Render(types, noComponents, noServices, noServices,
			World::PluginScriptLibrary::StubTables(), withEmptyTables, error));
		CHECK(withoutTables == withEmptyTables);

		World::ScriptEngine::Shutdown();
		CHECK(!World::ScriptEngine::IsInitialized());
	}

	// T4②:脚本函数兜底回收的三条路径 —— Register 抛异常 / 卸载 / 管理器析构。
	void CaseLeakedScriptFunctionSweep()
	{
		const fs::path root = FreshRoot("leaky-script-library");
		ManifestFields throwing;
		throwing.Id = "test.throwscript";
		WritePlugin(root / "engine", "test.throwscript", "WePluginTestThrowScriptLib", BuildYaml(throwing));
		ManifestFields leaky;
		leaky.Id = "test.scripttwo";
		WritePlugin(root / "engine", "aaa-scripttwo", "WePluginTestScriptTwo", BuildYaml(leaky));

		WorldContext context;
		{
			PluginManager manager;
			CHECK(manager.Discover(root / "engine", root / "project"));
			std::string error;
			// ① Register 抛异常:best-effort Unregister + 兜底回收(不留脏账)。
			const size_t throwMark = LogMark();
			CHECK(manager.Load("test.throwscript", context, &error) == PluginManager::Status::Rejected);
			CHECK(error.find("exception") != std::string::npos);
			const std::vector<std::string> throwLines = LogSince(throwMark);
			CHECK(LogContains(throwLines, "throw-scriptlib-registered"));
			CHECK(LogContains(throwLines, "unregister-after-script-throw"));
			CHECK(LogContains(throwLines,
				"script function 'throwlib.ping' was not unregistered by the plugin; force-removed"));
			CHECK(World::PluginScriptLibrary::Count() == 0);
			CHECK(manager.ScriptFunctions().empty());
		}
		CHECK(World::PluginScriptLibrary::Count() == 0);

		// ② 管理器析构路径(故意不 UnloadAll):违约插件注册的函数同样必须被移除 ——
		//    不留指向已释放 DLL 的回调。
		{
			PluginManager scoped;
			CHECK(scoped.Discover(root / "engine", root / "project"));
			std::string error;
			CHECK(scoped.Load("test.scripttwo", context, &error) == PluginManager::Status::Ok);
			CHECK(scoped.ScriptFunctions().size() == 2);
			CHECK(World::PluginScriptLibrary::Count() == 2);
		}
		CHECK(World::PluginScriptLibrary::Count() == 0);
		CHECK(World::PluginScriptLibrary::OwnerOf("hello.two").empty());
		CHECK(World::PluginScriptLibrary::OwnerOf("zeta.mul").empty());

		// ③ 卸载 + 重载:兜底回收后不留脏账(可以再注册同一批名字)。
		{
			PluginManager manager;
			CHECK(manager.Discover(root / "engine", root / "project"));
			std::string error;
			CHECK(manager.Load("test.scripttwo", context, &error) == PluginManager::Status::Ok);
			const size_t unloadMark = LogMark();
			CHECK(manager.Unload("test.scripttwo", context, &error) == PluginManager::Status::Ok);
			CHECK(LogContains(LogSince(unloadMark),
				"script function 'hello.two' was not unregistered by the plugin; force-removed"));
			CHECK(manager.Load("test.scripttwo", context, &error) == PluginManager::Status::Ok);
			CHECK(manager.ScriptFunctions().size() == 2);
			manager.UnloadAll(context);
			CHECK(World::PluginScriptLibrary::Count() == 0);
		}
	}

	// ---- PLUG-T5:打包(随包闭包 / 引用完整性硬门 / tolerate_missing / 产物拷贝)----

	// T5①:随包闭包 = 显式启用(引擎插件)+ 项目插件默认随包(除 `ship: never`)+
	//       `depends` 传递闭包(拓扑序);全程不加载插件 DLL(打包是静态步骤)。
	void CasePluginPackagingClosure()
	{
		const fs::path root = FreshRoot("pack-closure");
		ManifestFields alpha;
		alpha.Id = "com.probe.alpha";
		alpha.Depends = { "com.probe.beta" };
		WritePluginManifestOnly(root / "engine", "alpha", BuildYaml(alpha));
		ManifestFields beta;
		beta.Id = "com.probe.beta";
		WritePluginManifestOnly(root / "engine", "beta", BuildYaml(beta));
		ManifestFields gamma;
		gamma.Id = "com.probe.gamma";
		WritePluginManifestOnly(root / "engine", "gamma", BuildYaml(gamma));
		ManifestFields delta;
		delta.Id = "com.probe.delta";
		WritePluginManifestOnly(root / "project", "delta", BuildYaml(delta));
		ManifestFields epsilon;
		epsilon.Id = "com.probe.epsilon";
		epsilon.Ship = "never";
		WritePluginManifestOnly(root / "project", "epsilon", BuildYaml(epsilon));

		World::Plugins::PluginPackRequest request;
		request.EnginePluginsRoot = root / "engine";
		request.ProjectPluginsRoot = root / "project";
		request.ContentRoot = root / "content";   // 不存在 = 0 命中(不是错误)
		request.Enabled = { "com.probe.alpha" };
		request.CopyLibraries = false;

		const World::Plugins::PluginPackResult result = World::Plugins::PackagePlugins(request);
		CHECK(result.Ok);
		CHECK(result.Shipped == (std::vector<std::string>{
			"com.probe.beta", "com.probe.alpha", "com.probe.delta" }));
		CHECK(result.Skipped == (std::vector<std::string>{ "com.probe.gamma", "com.probe.epsilon" }));
		CHECK(result.MissingReferences == 0);
		CHECK(result.SummaryLine == "[plugins] shipped=com.probe.beta,com.probe.alpha,com.probe.delta"
			" skipped=com.probe.gamma,com.probe.epsilon missing-references=0");

		// 显式启用一个不存在的插件 id = 配置错误(不静默):cook 失败。
		request.Enabled = { "com.probe.missing" };
		const World::Plugins::PluginPackResult missing = World::Plugins::PackagePlugins(request);
		CHECK(!missing.Ok);
		CHECK(missing.Error.find("com.probe.missing") != std::string::npos);
		CHECK(missing.MissingReferences == 1);

		// `tolerate_missing` = 唯一例外:仍是缺件(计数 > 0)但不再让 cook 失败。
		request.TolerateMissing = { "com.probe.missing" };
		const World::Plugins::PluginPackResult tolerated = World::Plugins::PackagePlugins(request);
		CHECK(tolerated.Ok);
		CHECK(tolerated.MissingReferences == 1);

		// 缺依赖 = 硬失败(依赖不在闭包里跑不起来);被 tolerate 时降级。
		request.Enabled = { "com.probe.alpha" };
		request.TolerateMissing.clear();
		fs::remove_all(root / "engine" / "beta");
		const World::Plugins::PluginPackResult missingDependency = World::Plugins::PackagePlugins(request);
		CHECK(!missingDependency.Ok);
		CHECK(missingDependency.Error.find("com.probe.beta") != std::string::npos);
		request.TolerateMissing = { "com.probe.beta" };
		const World::Plugins::PluginPackResult toleratedDependency = World::Plugins::PackagePlugins(request);
		CHECK(toleratedDependency.Ok);
		CHECK(toleratedDependency.MissingReferences == 1);
	}

	// T5②:四类引用面的命中 + "引用落在闭包外 ⇒ cook 失败" + tolerate_missing 降级。
	void CasePluginPackagingReferenceGate()
	{
		const fs::path root = FreshRoot("pack-gate");
		ManifestFields blocked;
		blocked.Id = "com.probe.blocked";
		blocked.Contributes = {
			"  components:",
			"    - com.probe.blocked.Health",
			"  asset_types:",
			"    - id: probe.asset",
			"      extensions: [.wprobe]",
			"  importers:",
			"    - id: probe.import",
			"      extensions: [.wprobesrc]",
			"  script_namespaces: [probe]",
		};
		WritePluginManifestOnly(root / "engine", "blocked", BuildYaml(blocked));

		const fs::path content = root / "content";
		fs::create_directories(content / "scenes");
		fs::create_directories(content / "scripts");
		WriteFileText(content / "scenes" / "probe.wd",
			"FormatVersion: 2\nScene: Probe\nEntities:\n  - World.UUID: 1\n"
			"    com.probe.blocked.Health: 3\n");
		WriteFileText(content / "probe.wprobe", "asset\n");
		WriteFileText(content / "probe.wprobesrc", "source\n");
		WriteFileText(content / "scripts" / "probe.luau",
			"-- probe.ping is only mentioned in a comment\nlocal value = probe.ping(1)\n");

		World::Plugins::PluginPackRequest request;
		request.EnginePluginsRoot = root / "engine";
		request.ProjectPluginsRoot = root / "project";
		request.ContentRoot = content;
		request.CopyLibraries = false;

		// (a) 闭包外:硬门失败,四类面各命中 1,每条诊断都带"引用者 → 引用面 → 建议"。
		const World::Plugins::PluginPackResult blockedResult = World::Plugins::PackagePlugins(request);
		CHECK(!blockedResult.Ok);
		CHECK(blockedResult.MissingReferences == 4);
		CHECK(blockedResult.ComponentReferences == 1);
		CHECK(blockedResult.AssetTypeReferences == 1);
		CHECK(blockedResult.ImporterReferences == 1);
		CHECK(blockedResult.ScriptReferences == 1);
		CHECK(blockedResult.RenderHookReferences == 0);   // ⑤ 没有注册面 = 显式跳过并记 0
		CHECK(blockedResult.Diagnostics.size() == 4);
		bool sawComponent = false;
		bool sawAssetType = false;
		bool sawImporter = false;
		bool sawScript = false;
		for (const std::string& diagnostic : blockedResult.Diagnostics)
		{
			CHECK(diagnostic.find(" → ") != std::string::npos);
			CHECK(diagnostic.find("enable plugin 'com.probe.blocked'") != std::string::npos);
			if (diagnostic.find("component 'com.probe.blocked.Health'") != std::string::npos)
				sawComponent = true;
			if (diagnostic.find("asset.type '.wprobe'") != std::string::npos)
				sawAssetType = true;
			if (diagnostic.find("asset.importer '.wprobesrc'") != std::string::npos)
				sawImporter = true;
			if (diagnostic.find("script.namespace 'probe'") != std::string::npos)
				sawScript = true;
		}
		CHECK(sawComponent && sawAssetType && sawImporter && sawScript);
		CHECK(blockedResult.Error.find("com.probe.blocked") != std::string::npos);

		// (b) 启用该插件 ⇒ 引用落在闭包内 = 通过(静态打包仍不加载 DLL)。
		request.Enabled = { "com.probe.blocked" };
		const World::Plugins::PluginPackResult enabled = World::Plugins::PackagePlugins(request);
		CHECK(enabled.Ok);
		CHECK(enabled.MissingReferences == 0);
		CHECK(enabled.Shipped == (std::vector<std::string>{ "com.probe.blocked" }));

		// (c) tolerate_missing = 唯一例外:缺件计数保留(> 0),cook 不失败。
		request.Enabled.clear();
		request.TolerateMissing = { "com.probe.blocked" };
		const World::Plugins::PluginPackResult tolerated = World::Plugins::PackagePlugins(request);
		CHECK(tolerated.Ok);
		CHECK(tolerated.MissingReferences == 4);

		// (d) 注释里的 probe.ping 不算调用:只剩三类缺件。
		WriteFileText(content / "scripts" / "probe.luau",
			"-- probe.ping is only mentioned in a comment\n");
		request.TolerateMissing.clear();
		const World::Plugins::PluginPackResult commentOnly = World::Plugins::PackagePlugins(request);
		CHECK(commentOnly.ScriptReferences == 0);
		CHECK(commentOnly.MissingReferences == 3);
	}

	// T5③:闭包内 DLL 拷进 <publish>/bin/plugins/,并按发行清单顺序做发行形态加载
	//      (缺件 / 多余 DLL 都不静默)。
	void CasePluginPackagingCopiesAndPackagedLoad()
	{
		const fs::path root = FreshRoot("pack-copy");
		ManifestFields alpha;
		alpha.Id = "test.alpha";
		WritePlugin(root / "engine", "test.alpha", "WePluginTestAlpha", BuildYaml(alpha));
		ManifestFields beta;
		beta.Id = "test.beta";
		beta.Depends = { "test.alpha" };
		WritePlugin(root / "project", "test.beta", "WePluginTestBeta", BuildYaml(beta));

		const fs::path publish = root / "publish";
		World::Plugins::PluginPackRequest request;
		request.EnginePluginsRoot = root / "engine";
		request.ProjectPluginsRoot = root / "project";
		request.ContentRoot = root / "content";
		request.PublishDir = publish;
		request.Enabled = { "test.alpha" };

		const World::Plugins::PluginPackResult result = World::Plugins::PackagePlugins(request);
		CHECK(result.Ok);
		CHECK(result.Shipped == (std::vector<std::string>{ "test.alpha", "test.beta" }));
		CHECK(result.CopiedLibraries == 2);
		const fs::path plugbin = publish / "bin" / "plugins";
		CHECK(fs::is_regular_file(plugbin / ("test.alpha" + std::string(kLibraryExtension))));
		CHECK(fs::is_regular_file(plugbin / ("test.beta" + std::string(kLibraryExtension))));

		// 发行形态加载:按清单顺序(拓扑序)加载同一批 DLL。
		WorldContext context;
		{
			PluginManager manager;
			std::string error;
			CHECK(manager.LoadPackaged(plugbin, result.Shipped, context, &error) == PluginManager::Status::Ok);
			CHECK(manager.LoadedCount() == 2);
			CHECK(manager.LoadOrder() == result.Shipped);
			manager.UnloadAll(context);
		}

		// 清单列了、目录里没有 ⇒ ERROR + 失败;单条失败不影响其余(不静默)。
		{
			PluginManager manager;
			std::string error;
			CHECK(manager.LoadPackaged(plugbin, { "test.alpha", "test.missing" }, context, &error)
				== PluginManager::Status::Rejected);
			CHECK(error.find("test.missing") != std::string::npos);
			CHECK(CoreLogFileText().find("test.missing") != std::string::npos);
			CHECK(manager.LoadedCount() == 1);
			manager.UnloadAll(context);
		}

		// 目录里多余的 DLL(清单没列)⇒ WARN 后忽略。
		{
			PluginManager manager;
			std::string error;
			CHECK(manager.LoadPackaged(plugbin, { "test.alpha" }, context, &error)
				== PluginManager::Status::Ok);
			CHECK(CoreLogFileText().find("is not listed in the release manifest; ignored") != std::string::npos);
			CHECK(manager.LoadedCount() == 1);
			CHECK(manager.Find("test.beta") == nullptr);
			manager.UnloadAll(context);
		}
	}

	// T5④:清单契约 —— project.we.yaml 的 `plugins:` 块(向后兼容 / 解析 / 写回 / 校验)与
	//       plugin.we.yaml 的 `contributes:`(形态校验 + 声明/注册不一致 = WARN)。
	void CasePackagingManifestContracts()
	{
		const fs::path root = FreshRoot("pack-manifest");
		std::string error;

		// ---- project.we.yaml:老清单(没有 plugins: 块)= 三项全空,保存后也不新增该块 ----
		const fs::path plainPath = root / "plain" / "project.we.yaml";
		fs::create_directories(plainPath.parent_path());
		WriteFileText(plainPath, "id: com.probe.proj\nversion: 1.0.0\ncontent_root: assets\n"
			"start_scene: scenes/main.wd\nrenderer: opengl\npackages:\n  - content.wpak\n");
		World::Asset::ProjectManifest plain;
		CHECK(World::Asset::ProjectManifest::Load(plainPath, &plain, &error));
		CHECK(plain.Plugins.IsDefault());
		const fs::path plainOut = root / "plain-out" / "project.we.yaml";
		CHECK(World::Asset::ProjectManifest::Save(plainOut, plain, &error));
		CHECK(ReadFileText(plainOut).find("plugins:") == std::string::npos);

		// ---- 解析 + 写回(shipped 是 cook 写的发行清单字段;注释保留)----
		const fs::path manifestPath = root / "with-plugins" / "project.we.yaml";
		fs::create_directories(manifestPath.parent_path());
		WriteFileText(manifestPath, "id: com.probe.proj\nversion: 1.0.0\ncontent_root: assets\n"
			"start_scene: scenes/main.wd\nrenderer: opengl\npackages:\n  - content.wpak\n"
			"# 随包插件设置(手写注释必须保留)\n"
			"plugins:\n  enabled:\n    - com.probe.alpha\n"
			"  tolerate_missing:\n    - com.probe.optional\n");
		World::Asset::ProjectManifest withPlugins;
		CHECK(World::Asset::ProjectManifest::Load(manifestPath, &withPlugins, &error));
		CHECK(withPlugins.Plugins.Enabled == (std::vector<std::string>{ "com.probe.alpha" }));
		CHECK(withPlugins.Plugins.TolerateMissing == (std::vector<std::string>{ "com.probe.optional" }));
		CHECK(withPlugins.Plugins.Shipped.empty());
		withPlugins.Plugins.Shipped = { "com.probe.beta", "com.probe.alpha" };
		CHECK(World::Asset::ProjectManifest::Save(manifestPath, withPlugins, &error));
		World::Asset::ProjectManifest reloaded;
		CHECK(World::Asset::ProjectManifest::Load(manifestPath, &reloaded, &error));
		CHECK(reloaded.Plugins.Shipped == (std::vector<std::string>{ "com.probe.beta", "com.probe.alpha" }));
		CHECK(reloaded.Plugins.Enabled == withPlugins.Plugins.Enabled);
		CHECK(reloaded.Plugins.TolerateMissing == withPlugins.Plugins.TolerateMissing);
		CHECK(ReadFileText(manifestPath).find("# 随包插件设置(手写注释必须保留)") != std::string::npos);

		// ---- 空 id = 加载期干净拒绝 ----
		const fs::path badPath = root / "bad" / "project.we.yaml";
		fs::create_directories(badPath.parent_path());
		WriteFileText(badPath, "id: com.probe.proj\nversion: 1.0.0\ncontent_root: assets\n"
			"start_scene: scenes/main.wd\nrenderer: opengl\npackages:\n  - content.wpak\n"
			"plugins:\n  enabled:\n    - \"\"\n");
		CHECK(!World::Asset::ProjectManifest::Load(badPath, &reloaded, &error));
		CHECK(error.find("plugins.enabled") != std::string::npos);

		// ---- plugin.we.yaml:contributes 解析(扩展名默认小写 + 补前导点)----
		const fs::path goodPlugin = root / "plugins" / "good" / "plugin.we.yaml";
		fs::create_directories(goodPlugin.parent_path());
		WriteFileText(goodPlugin, "id: com.probe.good\n"
			"contributes:\n  components: [com.probe.good.Health]\n"
			"  asset_types:\n    - id: probe.asset\n      extensions: [WPROBE]\n"
			"  importers:\n    - id: probe.import\n      extensions: [.wprobesrc]\n"
			"  script_namespaces: [probe]\n");
		PluginManifest manifest;
		CHECK(PluginManifest::Load(goodPlugin, PluginScope::Engine, &manifest, &error));
		CHECK(manifest.Contributions.size() == 4);
		CHECK(manifest.Contributions[1].Extensions == (std::vector<std::string>{ ".wprobe" }));

		// 未知面 / 组件面写扩展名 = 干净拒绝(不猜语义)。
		const fs::path badPlugin = root / "plugins" / "bad" / "plugin.we.yaml";
		fs::create_directories(badPlugin.parent_path());
		WriteFileText(badPlugin, "id: com.probe.bad\ncontributes:\n  widgets: [x]\n");
		CHECK(!PluginManifest::Load(badPlugin, PluginScope::Engine, &manifest, &error));
		CHECK(error.find("widgets") != std::string::npos);
		WriteFileText(badPlugin, "id: com.probe.bad\n"
			"contributes:\n  components:\n    - id: com.probe.bad.Health\n      extensions: [.x]\n");
		CHECK(!PluginManifest::Load(badPlugin, PluginScope::Engine, &manifest, &error));
		CHECK(error.find("extensions") != std::string::npos);

		// ---- 声明了 contributes 的插件:声明与实际注册不一致 = WARN(打包索引漏报必须可观测)----
		ManifestFields drift;
		drift.Id = "test.alpha";
		drift.Contributes = { "  components:", "    - test.alpha.NotRegistered" };
		WritePlugin(root / "drift" / "engine", "test.alpha", "WePluginTestAlpha", BuildYaml(drift));
		WorldContext context;
		PluginManager manager;
		CHECK(manager.Discover(root / "drift" / "engine", root / "drift" / "project"));
		std::string loadError;
		CHECK(manager.Load("test.alpha", context, &loadError) == PluginManager::Status::Ok);
		CHECK(CoreLogFileText().find("did not register it") != std::string::npos);
		manager.UnloadAll(context);
	}

	// ---- PLUG-T6:两段式热重载 + 账本审计 ---------------------------------------------
	//
	// 复用 T2c 的真插件 DLL(WePluginTestComponentStorage):写字段值 → 第一段(快照 + 卸载)
	// → "外部重编"(把原 DLL 重新拷回包内;文件锁真的释放了才可能成功)→ 第二段(载入 +
	// 写回)→ 坏 DLL 触发回滚(旧 DLL + 原状态)→ 活实例门(unload / 单类型注销都被拒)
	// → 清实例后连续 load/unload 账本零增长。
	void CasePluginHotReloadAndLedgerAudit()
	{
		// 与 T2c 同一理由:证据全部走 API(注册表 / 场景 / 返回状态 / 结果字段),日志静音。
		const CoreLogMute mute;
		const fs::path root = FreshRoot("plugin-reload");
		ManifestFields storage;
		storage.Id = "test.storage";
		WritePlugin(root / "engine", "test.storage", "WePluginTestComponentStorage", BuildYaml(storage));

		WorldContext context;
		Schema::SchemaRegistry& schemas = context.Schemas();
		PluginManager manager;
		std::string error;
		CHECK(manager.Discover(root / "engine", root / "project"));
		CHECK(manager.LoadAll(context, &error) == PluginManager::Status::Ok);
		CHECK(manager.LoadedCount() == 1);

		const Schema::TypeSchema* health = schemas.Find("test.storage.Health");
		CHECK(health != nullptr && health->Storage != nullptr);
		const uint32_t healthId = health->Storage->ComponentId;

		// 账本在加载后分类可见;活实例计数从 0 开始。
		const PluginLedgerCounts loadedLedgers = manager.LedgerCounts("test.storage");
		CHECK(loadedLedgers.Components == 3);       // Health + Shield + schema-only Legacy
		CHECK(loadedLedgers.AssetTypes == 0 && loadedLedgers.Importers == 0);
		CHECK(loadedLedgers.EditorCommands == 0 && loadedLedgers.EditorPanels == 0);
		CHECK(loadedLedgers.ScriptFunctions == 0);
		CHECK(loadedLedgers.Total() == 3);
		CHECK(manager.LedgerTotals().Total() == 3);
		CHECK(manager.LiveComponentInstances("test.storage", &context) == 0);

		// 场景 + 实例 + 字段值(经宿主访问器写进插件声明布局的真实字节)。
		World::Ref<World::Scene> scene = World::CreateRef<World::Scene>(context);
		World::Entity entity = World::Entity::CreateEntity(scene.get(), "Reload Probe");
		entity.AddComponent(healthId);
		void* instance = entity.GetComponent(healthId);
		CHECK(instance != nullptr);
		health->Fields[0].Set(instance, Schema::Value(true));
		health->Fields[1].Set(instance, Schema::Value(7));
		health->Fields[2].Set(instance, Schema::Value(42.5f));
		health->Fields[3].Set(instance, Schema::Value(glm::vec3(1.0f, 2.0f, 3.0f)));
		health->Fields[4].Set(instance, Schema::Value(static_cast<uint8_t>(1)));
		CHECK(manager.LiveComponentInstances("test.storage", &context) == 1);

		// 单类型注销的活实例门(T6 对称补口):有实例 = false,类型/插件状态不动。
		const auto unregisterFn = reinterpret_cast<bool (*)(const char*)>(
			manager.LookupExport("test.storage", "storage.unregister", 1));
		CHECK(unregisterFn != nullptr);
		CHECK(!unregisterFn("test.storage.Health"));
		CHECK(schemas.Find("test.storage.Health") != nullptr);
		CHECK(manager.LoadedCount() == 1);

		// 第一段:快照 + 从场景移除 + 卸载(释放 DLL 文件锁)。
		PluginReloadResult first;
		CHECK(manager.UnloadForReload("test.storage", context, scene.get(), &first)
			== PluginManager::Status::Ok);
		CHECK(first.ResultPhase == PluginReloadResult::Phase::Unloaded);
		CHECK(first.Ok && first.Unloaded);
		CHECK(first.InstancesSnapshotted == 1);
		CHECK(manager.HasPendingReload("test.storage"));
		CHECK(manager.LoadedCount() == 0);
		CHECK(schemas.Find("test.storage.Health") == nullptr);
		CHECK(manager.LedgerTotals().Total() == 0);                 // 卸载后宿主侧账本全 0
		CHECK(manager.LiveComponentInstances("test.storage", &context) == 0);
		CHECK(!entity.HasComponent(healthId));                      // 实例已从场景移除(值在快照里)
		CHECK(!first.LibraryPath.empty() && !first.RollbackPath.empty());
		CHECK(fs::is_regular_file(first.RollbackPath));             // 卸载前拷的旧 DLL(回滚用)

		// "外部重编":已卸载 ⇒ 覆盖包内 DLL 必须成功(仍然加载着时 Windows 会拒绝写)。
		const fs::path library = first.LibraryPath;
		CHECK(fs::is_regular_file(library));
		std::error_code copyError;
		fs::copy_file(DllDirectory() / (std::string("WePluginTestComponentStorage") + kLibraryExtension),
			library, fs::copy_options::overwrite_existing, copyError);
		CHECK(!copyError);

		// 第二段:载入新 DLL + 按字段 id 写回快照。
		PluginReloadResult second;
		CHECK(manager.LoadForReload("test.storage", context, scene.get(), &second)
			== PluginManager::Status::Ok);
		CHECK(second.ResultPhase == PluginReloadResult::Phase::Loaded);
		CHECK(second.Ok && !second.RolledBack);
		CHECK(second.InstancesRestored == 1 && second.InstancesSkipped == 0);
		CHECK(!manager.HasPendingReload("test.storage"));
		CHECK(manager.LoadedCount() == 1);
		const Schema::TypeSchema* restored = schemas.Find("test.storage.Health");
		CHECK(restored != nullptr && restored->Storage != nullptr);
		CHECK(restored->Storage->ComponentId == healthId);          // 同档位同槽位 ⇒ id 复用
		CHECK(entity.HasComponent(healthId));
		void* restoredInstance = entity.GetComponent(healthId);
		CHECK(restoredInstance != nullptr);
		CHECK(std::get<bool>(restored->Fields[0].Get(restoredInstance)) == true);
		CHECK(std::get<int32_t>(restored->Fields[1].Get(restoredInstance)) == 7);
		CHECK(std::get<float>(restored->Fields[2].Get(restoredInstance)) == 42.5f);
		CHECK(std::get<glm::vec3>(restored->Fields[3].Get(restoredInstance))
			== glm::vec3(1.0f, 2.0f, 3.0f));
		CHECK(std::get<uint8_t>(restored->Fields[4].Get(restoredInstance)) == 1);
		CHECK(manager.LiveComponentInstances("test.storage", &context) == 1);
		CHECK(manager.LedgerCounts("test.storage").Components == 3);

		// 活实例时普通 Unload 干净拒绝(不偷数据);第一段才是显式带快照的卸载。
		CHECK(manager.Unload("test.storage", context, &error)
			== PluginManager::Status::HasLiveInstances);
		CHECK(error.find("live component instances") != std::string::npos);
		CHECK(manager.LoadedCount() == 1);

		// 坏 DLL ⇒ 第二段失败但回滚到旧 DLL,原状态保留(实体 + 字段值都还在)。
		PluginReloadResult brokenFirst;
		CHECK(manager.UnloadForReload("test.storage", context, scene.get(), &brokenFirst)
			== PluginManager::Status::Ok);
		CHECK(brokenFirst.InstancesSnapshotted == 1);
		WriteFileText(brokenFirst.LibraryPath, "PLUG-T6 deliberately broken library");
		PluginReloadResult brokenSecond;
		const PluginManager::Status brokenStatus =
			manager.LoadForReload("test.storage", context, scene.get(), &brokenSecond);
		CHECK(brokenStatus != PluginManager::Status::Ok);
		CHECK(brokenSecond.RolledBack);
		CHECK(brokenSecond.ResultPhase == PluginReloadResult::Phase::RolledBack);
		CHECK(brokenSecond.InstancesRestored == 1);
		CHECK(brokenSecond.Message.find("rolled back") != std::string::npos);
		CHECK(!manager.HasPendingReload("test.storage"));
		CHECK(manager.LoadedCount() == 1);
		CHECK(manager.LoadedLibraryPath("test.storage") == brokenFirst.RollbackPath);
		const Schema::TypeSchema* rolledBack = schemas.Find("test.storage.Health");
		CHECK(rolledBack != nullptr && rolledBack->Storage != nullptr);
		CHECK(entity.HasComponent(rolledBack->Storage->ComponentId));
		void* rolledBackInstance = entity.GetComponent(rolledBack->Storage->ComponentId);
		CHECK(rolledBackInstance != nullptr);
		CHECK(std::get<float>(rolledBack->Fields[2].Get(rolledBackInstance)) == 42.5f);
		CHECK(std::get<int32_t>(rolledBack->Fields[1].Get(rolledBackInstance)) == 7);
		CHECK(manager.LedgerTotals().Total() == 3);

		// 把规范路径上的 DLL 恢复成好产物(下一次 reload 仍指向"用户重编的那份")。
		fs::copy_file(DllDirectory() / (std::string("WePluginTestComponentStorage") + kLibraryExtension),
			brokenFirst.LibraryPath, fs::copy_options::overwrite_existing, copyError);
		CHECK(!copyError);

		// 清实例 → 普通卸载成功、账本归零;连续 N 轮 load/unload 账本不得增长。
		entity.RemoveComponent(rolledBack->Storage->ComponentId);
		CHECK(manager.LiveComponentInstances("test.storage", &context) == 0);
		CHECK(manager.Unload("test.storage", context, &error) == PluginManager::Status::Ok);
		CHECK(manager.LedgerTotals().Total() == 0);
		CHECK(schemas.ListByModule("test.storage").empty());
		const size_t entriesAfterLoad = manager.Count();
		for (int round = 0; round < 5; ++round)
		{
			CHECK(manager.Load("test.storage", context, &error) == PluginManager::Status::Ok);
			CHECK(manager.LedgerTotals().Total() == 3);
			CHECK(manager.Unload("test.storage", context, &error) == PluginManager::Status::Ok);
			CHECK(manager.LedgerTotals().Total() == 0);
			CHECK(manager.LedgerCounts("test.storage").Total() == 0);
			CHECK(manager.Count() == entriesAfterLoad);   // 条目数不随轮次增长
		}
		scene.reset();
	}
}

	int main()
{
	try
	{
		// 插件侧证据(Register/Unregister 的日志)要经核心日志取回,所以先初始化日志。
		// 日志文件落在 WLD_OUTPUT_DIR(= build 目录)内,不写工作区外的产物。
		World::Log::Init();

		CaseDiscoveryTopologyAndUnload();   // ① 拓扑顺序 / ⑧ 卸载与 Unregister
		CaseManifestAbiMismatch();          // ②a 清单 abi 不符
		CaseEngineRequirementRejected();    // ②c 清单 engine 约束不满足 = 硬拒绝 + 级联
		CasePluginAbiMismatch();            // ②b 插件 ABI 不符
		CaseStructSizeMismatch();           // ③ StructSize 不符
		CaseStructSizeLargerAccepted();     // ③b StructSize 更大(尾部追加)= 接受
		CaseDevBinaryRootResolution();      // ③c 开发构建根解析 + ExportNames
		CaseDuplicateId();                  // ④ 重复 id
		CaseMissingDependency();            // ⑤ 缺依赖 + 级联
		CaseDependencyCycle();              // ⑥ 依赖环
		CaseScopeMismatch();                // ⑦ 位置与 scope 不符
		CaseMissingEntrySymbol();           // entry 符号缺失
		CaseNoRegister();                   // Register 为空
		CaseIdMismatch();                   // 清单 id 与 WePlugin.Id 不一致
		CaseFailureIsolation();             // ⑨ 单个失败不影响其余
		CaseDeclarationWarnings();          // 声明比对 = 警告
		CaseEmptyRoots();                   // 零插件
		CaseHostApiRegistrationSurface();   // T2a① 宿主注册面(资产类型 / 导入器 / cook 接线)
		CaseLeakedRegistrationSweep();      // T2a② 卸载兜底回收(违约插件不留悬空回调)
		CaseLookupExport();                 // T2a③ LookupExport(宿主 + 插件两侧)
		CaseExamplePluginDiscoverable();    // T2a④ plugins/hello-import 清单可发现
		CaseComponentSchemaRegistration();  // T2b① 组件 schema 注册 / 序列化往返 / 单类型注销
		CaseLeakedComponentSweep();         // T2b② 归属保护 / 卸载与析构兜底回收
		CaseComponentStorageBridge();       // T2c 组件存储桥(Size/Alignment → blob 存储 / 活实例门)
		CaseEditorExtensionRegistration();  // T3b① 编辑器命令 / 面板注册 / 触发计数 / 注销幂等
		CaseLeakedEditorExtensionSweep();   // T3b② 抛异常 / 卸载 / 析构三条回收路径
		CaseScriptLibraryRegistration();    // T4① 脚本函数账本 / VM 调用 / 存根确定性 / 归属保护
		CaseLeakedScriptFunctionSweep();    // T4② 抛异常 / 卸载 / 析构三条兜底回收路径
		CasePluginPackagingClosure();          // T5① 随包闭包(显式启用 + depends 传递 + ship 策略)
		CasePluginPackagingReferenceGate();    // T5② 四类引用面 + 闭包外 ⇒ 失败 + tolerate_missing
		CasePluginPackagingCopiesAndPackagedLoad();   // T5③ 产物拷贝 + 发行形态加载(缺件/多余 DLL)
		CasePackagingManifestContracts();      // T5④ plugins: 块与 contributes: 的清单契约
		CasePluginHotReloadAndLedgerAudit();    // T6 两段式热重载 + 回滚 + 活实例门 + 账本审计

		std::error_code ec;
		fs::remove_all(fs::temp_directory_path() / "worldengine-plugin-tests", ec);
		std::printf("World.Plugins: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Plugins: FAILED: %s\n", error.what());
		return 1;
	}
}

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
#include "World/Core/Asset/AssetTypeRegistry.h"
#include "World/Core/Asset/BuiltinImporters.h"
#include "World/Core/Asset/CookPipeline.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Plugins/PluginManager.h"
#include "World/Schema/Schema.h"
#include "World/Schema/SchemaRegistry.h"

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
		CaseEditorExtensionRegistration();  // T3b① 编辑器命令 / 面板注册 / 触发计数 / 注销幂等
		CaseLeakedEditorExtensionSweep();   // T3b② 抛异常 / 卸载 / 析构三条回收路径

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

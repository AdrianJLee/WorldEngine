// PLUG-T1:插件系统 T1(清单解析 / 发现 / 校验 / 依赖拓扑 / 加载 / 卸载 / 回滚)headless 单测。
//
// 证据口径:
//   * 真进程 + 真 WorldContext + 真 PluginManager,加载 tests/CMakeLists.txt 构建的真 DLL
//     测试插件(不 mock 动态库加载);
//   * 插件侧证据(Register/Unregister 确实被调用)走宿主 WeHostApi::Log → World 核心日志,
//     用 World::Log::RecentLines 取回(所以 main 先 Log::Init());
//   * 每个用例独立临时目录 + 独立 PluginManager;失败路径逐条断言可读诊断。
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Plugins/PluginManager.h"

#include <cstdio>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace World::Plugins;
	using World::WorldContext;
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

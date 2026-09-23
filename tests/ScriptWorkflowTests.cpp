// P2 W8:脚本工作流的 headless 回归(Scripts 面板/编辑器的宿主能力对应的世界侧逻辑)。
//
// 覆盖:
//   1. ResolveScriptDiskPath:合法逻辑路径命中磁盘常规文件并返回绝对路径;"."/".."/绝对
//      路径/盘符/空串被拒;缺失文件返回 false + error;
//   2. 包内来源被拒(错误文本含"包内不可编辑")—— 只对已登记的内容上下文 Vfs 查询,
//      与 ScriptEngine::ReadScriptBytes 的解析顺序一致(headless 只挂包 provider 也能复现);
//   3. LSP 脚手架 create-if-missing:空目录生成两份文件且内容正确;已存在(改动过的内容)
//      绝不覆盖。
//
// 全程只写自己创建的临时目录,不动工作树内的任何资产。

#include "wldpch.h"

#include "World/Core/Vfs/PackageProvider.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/LuaStubGenerator.h"
#include "World/Script/HotReload.h"
#include "World/Script/BindEvents.h"
#include "World/Script/BindServices.h"
#include "World/Script/BindUI.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Script/LuauFormatter.h"
#include "World/Script/LuauHighlighter.h"
#include "World/Script/LuauSyntax.h"
#include "World/WUI/WuiTextBuffer.h"

#include "Generated/Game/GameSchemaRegistration.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
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

	std::string ReadText(const fs::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
			throw std::runtime_error("cannot read " + path.generic_string());
		return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
	}

	void WriteText(const fs::path& path, const std::string& text)
	{
		fs::create_directories(path.parent_path());
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file.write(text.data(), static_cast<std::streamsize>(text.size()));
		file.flush();
		if (!file.good())
			throw std::runtime_error("cannot write " + path.generic_string());
	}

	// 与其它 suite 同一模式:测试进程自持一个 WorldContext(无窗口/无 GPU)。
	WorldContext& TestContext()
	{
		static WorldContext context;
		return context;
	}

	fs::path RunDirectory()
	{
		return fs::temp_directory_path() /
			("WorldScriptWorkflowTests-" + std::to_string(static_cast<unsigned long long>(GetCurrentProcessId())));
	}

	struct TempDirectory
	{
		fs::path Path;
		~TempDirectory()
		{
			std::error_code ignored;
			fs::remove_all(Path, ignored);
		}
	};

	// ---- 1. 合法路径:命中磁盘上的常规文件,返回绝对路径 ----

	void ResolveDiskPathHitsRealFile()
	{
		const std::string logical = "scripts/tests/UiProbe.lua";
		const fs::path expected = fs::path(WLD_ASSETPATH) / fs::path(logical);
		CHECK(fs::is_regular_file(expected));   // 前提:入库的开发树脚本

		fs::path resolved;
		std::string error = "stale";
		CHECK(ResolveScriptDiskPath(logical, resolved, &error));
		CHECK(error.empty());
		CHECK(resolved.is_absolute());
		CHECK(fs::is_regular_file(resolved));
		CHECK(fs::equivalent(resolved, expected));

		// 反斜杠分隔与重复斜杠走同一条归一化:解析结果一致。
		fs::path separated;
		CHECK(ResolveScriptDiskPath("scripts\\tests//UiProbe.lua", separated, nullptr));
		CHECK(fs::equivalent(separated, resolved));
	}

	// ---- 2. 非法路径:'.'/'..'/绝对路径/盘符/空串一律拒绝 ----

	void ResolveDiskPathRejectsInvalidPaths()
	{
		const char* const invalid[] = {
			"",
			".",
			"..",
			"../outside.lua",
			"scripts/../../escape.lua",
			"/absolute/script.lua",
			"\\absolute\\script.lua",
			"C:/absolute/script.lua",
			"scripts/./probe.lua",
		};
		for (const char* candidate : invalid)
		{
			fs::path resolved = "unchanged";
			std::string error;
			CHECK(!ResolveScriptDiskPath(candidate, resolved, &error));
			CHECK(!error.empty());
			CHECK(resolved == fs::path("unchanged"));   // 失败时 out 保持调用前内容
		}
	}

	// ---- 3. 缺失文件:false + "not found" 一类的可读错误 ----

	void ResolveDiskPathRejectsMissingFile()
	{
		fs::path resolved;
		std::string error;
		CHECK(!ResolveScriptDiskPath("scripts/__w8_not_on_disk.lua", resolved, &error));
		CHECK(error.find("not found") != std::string::npos);
		CHECK(resolved.empty());
	}

	// ---- 4. 包内来源:包 provider 命中的逻辑路径不可编辑 ----

	void ResolveDiskPathRejectsPackageSource()
	{
		const fs::path directory = RunDirectory() / "package";
		TempDirectory cleanup { directory };
		const std::string logical = "scripts/w8/package_probe.luau";
		// 与 ScriptBinaryLoadTests 同一夹具:先落 cooked 目录,再打成 WPAK2 包。
		WriteText(directory / "cooked" / fs::path(logical), "return {}\n");
		const fs::path package = directory / "w8.pak";
		std::error_code error;
		CHECK(World::Vfs::PackageProvider::BuildFromDirectory(directory / "cooked", package, error));
		CHECK(!error);
		std::shared_ptr<World::Vfs::PackageProvider> provider = World::Vfs::PackageProvider::Open(package, error);
		CHECK(provider != nullptr);
		CHECK(!error);

		// 前提:磁盘(开发树)上不存在该逻辑路径 —— 命中只可能来自包。
		CHECK(!fs::exists(fs::path(WLD_ASSETPATH) / fs::path(logical)));

		WorldContext& context = TestContext();
		const Vfs::MountId mountId = context.Vfs().Mount("w8-script-workflow-package", provider, 100);
		CHECK(mountId != 0);
		// 登记内容上下文:ResolveScriptDiskPath 与 ScriptEngine::ReadScriptBytes 同一查询顺序
		// (内容上下文优先,未登记回退 Application),headless 才能复现包内来源。
		ScriptEngine::Init(context);

		fs::path resolved = "unchanged";
		std::string message;
		CHECK(!ResolveScriptDiskPath(logical, resolved, &message));
		CHECK(message.find("包内不可编辑") != std::string::npos);
		CHECK(resolved == fs::path("unchanged"));

		context.Vfs().Unmount(mountId);
	}

	// ---- 5. LSP 脚手架 create-if-missing(空目录生成;已有文件绝不覆盖)----

	void ScaffoldCreateIfMissing()
	{
		const fs::path directory = RunDirectory() / "scaffold";
		TempDirectory cleanup { directory };
		fs::create_directories(directory);

		std::string error = "stale";
		CHECK(EnsureScriptEditorScaffold(directory, &error));
		CHECK(error.empty());
		const fs::path settings = directory / ".vscode" / "settings.json";
		const fs::path lspConfig = directory / ".luau-lsp" / "config.json";
		CHECK(fs::is_regular_file(settings));
		CHECK(fs::is_regular_file(lspConfig));

		// 内容正确:与入库的 Game/ 两份脚手架文件逐字节一致(同一份常量,不会各自漂移)。
		const fs::path gameRoot = fs::path(WLD_ASSETPATH).parent_path();
		CHECK(ReadText(settings) == ReadText(gameRoot / ".vscode" / "settings.json"));
		CHECK(ReadText(lspConfig) == ReadText(gameRoot / ".luau-lsp" / "config.json"));
		CHECK(ReadText(settings).find("assets/scripts/intermediate/WorldEngineAPI.luau") != std::string::npos);
		CHECK(ReadText(lspConfig).find("assets/scripts/intermediate/WorldEngineAPI.luau") != std::string::npos);

		// 已存在(模拟用户改过的配置)→ 第二次调用不覆盖。
		const std::string userSettings = "{\n  \"user\": true\n}\n";
		const std::string userConfig = "{\"mine\": 1}\n";
		WriteText(settings, userSettings);
		WriteText(lspConfig, userConfig);
		CHECK(EnsureScriptEditorScaffold(directory, &error));
		CHECK(error.empty());
		CHECK(ReadText(settings) == userSettings);
		CHECK(ReadText(lspConfig) == userConfig);
	}

	// ---- 6. 存根漂移门禁:入库的 WorldEngineAPI.luau 必须与"编辑器同一输入"的渲染逐字节一致 ----
	//
	// 编辑器启动时把 World + Game 两个 schema 模块都注册进内容上下文,再用
	// LuaReflectionRegistry + 服务/UI/事件描述表渲染存根。这里复现同一组输入:
	// 本目标额外编译 Game 的生成式 schema 注册源(见 tests/CMakeLists.txt),
	// 于是"启动编辑器不会重写存根"在 headless 里等价为"渲染结果 == 入库文件"。
	void CommittedStubMatchesEditorRendering()
	{
		WorldContext& context = TestContext();
		// 与 GameAPI.cpp 的模块初始化同一条注册入口(Game 的 schema 模块)。
		CHECK(World::Schema::RegisterGameSchemaModule(context.Schemas()));

		const std::vector<const Schema::TypeSchema*> components =
			context.Schemas().List(Schema::TypeCategory::Component);
		CHECK(components.size() >= 14);   // 13 个 World 组件 + Game 的 SampleDataComponent

		std::size_t serviceCount = 0;
		const ScriptServiceBinding* services = GameplayServiceBindings(&serviceCount);
		std::vector<const ScriptServiceBinding*> serviceList;
		serviceList.reserve(serviceCount + 2);
		for (std::size_t index = 0; index < serviceCount; ++index)
			serviceList.push_back(&services[index]);
		// W4:events/timers 与 Input/Level/Save 共用服务块链路(ScriptEngine::GenerateLuaStubs 同序)。
		std::size_t eventCount = 0;
		const ScriptServiceBinding* eventTables = ScriptEventBindings(&eventCount);
		for (std::size_t index = 0; index < eventCount; ++index)
			serviceList.push_back(&eventTables[index]);
		std::size_t uiCount = 0;
		const ScriptServiceBinding* uiTables = ScriptUiBindings(&uiCount);
		std::vector<const ScriptServiceBinding*> uiList;
		uiList.reserve(uiCount);
		for (std::size_t index = 0; index < uiCount; ++index)
			uiList.push_back(&uiTables[index]);

		std::string rendered, error;
		CHECK(LuaStubGenerator::Render(LuaReflectionRegistry::GetTable(), components, serviceList, uiList,
			rendered, error));

		const fs::path committed = fs::path(WLD_ASSETPATH) / "scripts" / "intermediate" / "WorldEngineAPI.luau";
		CHECK(fs::is_regular_file(committed));
		// 漂移门禁:逐字节一致(比较前统一 CRLF→LF —— 仓库走 core.autocrlf 时工作树可能是 CRLF,
		// 那是行尾差异不是内容漂移;2026-09-23 实测过该误报,见 docs/dev/file-norms.md §R3)。
		const auto normalizeEol = [](std::string text)
		{
			std::string out;
			out.reserve(text.size());
			for (std::size_t index = 0; index < text.size(); ++index)
			{
				if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n')
					continue;
				out.push_back(text[index]);
			}
			return out;
		};
		CHECK(normalizeEol(ReadText(committed)) == normalizeEol(rendered));
		// W8 新增:WorldScript 注解包含运行时就存在的 OnUI(self) 回调。
		CHECK(rendered.find("---@field OnUI? fun(self: WorldScript)") != std::string::npos);
		CHECK(rendered.find("---@field OnCreate? fun(self: WorldScript)") != std::string::npos);

		// 旧文件名不得回归:入库制品只有 .luau 一份。
		CHECK(!fs::exists(fs::path(WLD_ASSETPATH) / "scripts" / "intermediate" / "WorldEngineAPI.lua"));
		World::Schema::UnregisterGameSchemaModule(context.Schemas());
	}

	// ---- W9.7:语法检查 + 轻量格式化 ----

	void LuauSyntaxFlagsErrors()
	{
		World::LuauSyntaxError error;
		CHECK(World::CheckLuauSyntax("local x = 1\nreturn x\n", "probe", &error));
		CHECK(!World::CheckLuauSyntax("local x = 1\nlocal y = \n", "probe", &error));
		// Luau 对 "got <eof>" 报的是 EOF 所在行(带尾换行 = 第 3 行);调用方按缓冲行数夹取。
		CHECK(error.Line == 3);
		CHECK(!error.Message.empty());
	}

	void LuauFormatterReindentsAndTrims()
	{
		const std::string messy =
			"   local a = 1   \n"
			" function f()\n"
			"      if a then\n"
			"    return a\n"
			"   else\n"
			" return 0\n"
			"     end\n"
			"end\n";
		const std::string formatted = World::FormatLuauSource(messy);
		const std::string expected =
			"local a = 1\n"
			"function f()\n"
			"    if a then\n"
			"        return a\n"
			"    else\n"
			"        return 0\n"
			"    end\n"
			"end\n";
		CHECK(formatted == expected);

		// 长字符串内部原样保留(缩进/行尾空白都不改)。
		const std::string raw =
			"local s = [[\n"
			"   keep   me   \n"
			"]]\n";
		CHECK(World::FormatLuauSource(raw) == raw);

		// 行内多余空格折叠(用户实测:多打几个空格格式化没效果)。
		CHECK(World::FormatLuauSource("local   a =    1\n") == "local a = 1\n");
		CHECK(World::FormatLuauSource("f( a ,  b )\n") == "f(a, b)\n");
		// 字符串内部空白不动。
		CHECK(World::FormatLuauSource("local s = \"a   b\"\n") == "local s = \"a   b\"\n");
	}

	void ReplaceAllIsSingleUndo()
	{
		World::Wui::WuiTextBuffer buffer;
		buffer.SetText("a\nb\n");
		CHECK(buffer.ReplaceAll("a\nB\n"));
		CHECK(buffer.Text() == "a\nB\n");
		CHECK(buffer.Undo());
		CHECK(buffer.Text() == "a\nb\n");
		CHECK(buffer.Redo());
		CHECK(buffer.Text() == "a\nB\n");
	}

	int RunAll()
	{
		const std::pair<const char*, void(*)()> tests[] = {
			{ "ResolveScriptDiskPath hits a real disk file", ResolveDiskPathHitsRealFile },
			{ "ResolveScriptDiskPath rejects invalid logical paths", ResolveDiskPathRejectsInvalidPaths },
			{ "ResolveScriptDiskPath rejects a missing file", ResolveDiskPathRejectsMissingFile },
			{ "ResolveScriptDiskPath rejects package-sourced scripts", ResolveDiskPathRejectsPackageSource },
			{ "LSP scaffold is created if missing and never overwritten", ScaffoldCreateIfMissing },
			{ "committed WorldEngineAPI.luau matches the editor renderer", CommittedStubMatchesEditorRendering },
			{ "CheckLuauSyntax flags the error line", LuauSyntaxFlagsErrors },
			{ "FormatLuauSource reindents/trims but keeps long strings", LuauFormatterReindentsAndTrims },
			{ "WuiTextBuffer::ReplaceAll is a single undo step", ReplaceAllIsSingleUndo },
		};
		int failures = 0;
		for (const auto& [name, test] : tests)
		{
			try
			{
				test();
				std::printf("[PASS] %s\n", name);
			}
			catch (const std::exception& error)
			{
				++failures;
				std::fprintf(stderr, "[FAIL] %s: %s\n", name, error.what());
			}
			catch (...)
			{
				++failures;
				std::fprintf(stderr, "[FAIL] %s: unknown exception\n", name);
			}
		}
		return failures;
	}
}

int main()
{
	try
	{
		World::Log::Init();
		World::ScriptEngine::Init();
		const int failures = RunAll();
		World::ScriptEngine::Shutdown();
		if (failures == 0)
		{
			std::printf("World.ScriptWorkflow: all checks passed\n");
			return 0;
		}
		std::fprintf(stderr, "World.ScriptWorkflow: %d group(s) failed\n", failures);
		return 1;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Test setup failed: %s\n", error.what());
		if (World::ScriptEngine::IsInitialized())
			World::ScriptEngine::Shutdown();
		return 1;
	}
	catch (...)
	{
		std::fprintf(stderr, "Test setup failed: unknown exception\n");
		if (World::ScriptEngine::IsInitialized())
			World::ScriptEngine::Shutdown();
		return 1;
	}
}

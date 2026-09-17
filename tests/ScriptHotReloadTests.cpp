// P2 W5:L2 脚本热重载(引擎侧)回归。
//
// 覆盖:
//   1. 源指纹:内容 FNV-1a64 优先,读不到时 mtime+size 兜底;同一内容(只动 mtime)不产生变化;
//   2. 字段迁移:v1{A,B} → 跑一帧改 A → v2 重排 + 新增 C → A/B 保留、C 取默认,行为描述刷新;
//   3. 类型变化:同名类型变化 → 回新默认值 + 诊断(含 LuaFieldId 稳定 id);
//   4. 失败回滚:语法错误 → 返回 false + 含路径/行号的诊断,旧引用与旧行为都保留,State 仍 Running;
//   5. 轮询监听:改内容恰好一次重载、不改零次、连续写 debounce 归并成一次;
//   6. 拒绝语义:State ∈ {Creating, Destroying} 与回调内(非安全点)拒绝,帧边界可重载;
//   7. generation:热重载后进入独立域(最高位置位),与 Scene 启动期分配值及上一次重载值都不同。
//
// headless:真实 Scene 调度(OnScriptStart/OnScriptUpdate),脚本写在构建产物的临时目录里,
// 逻辑路径是相对 WLD_ASSETPATH 的带 ".." 路径(与 ScriptLifecycleTests 同一模式)。
#include "wldpch.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Script/BehaviorRegistry.h"
#include "World/Script/HotReload.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptFileWatch.h"
#include "World/Script/ScriptValue.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/ScriptEngine.h"

#include <algorithm>
#include <any>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	namespace fs = std::filesystem;
	using namespace World;

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

	fs::path ScriptPath(const char* name)
	{
		return g_RunDirectory / name;
	}

	std::string LogicalPath(const fs::path& path)
	{
		return path.lexically_relative(fs::path(WLD_ASSETPATH)).generic_string();
	}

	void WriteScript(const fs::path& path, const std::string& text)
	{
		fs::create_directories(path.parent_path());
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << text;
		file.flush();
		if (!file.good())
			throw std::runtime_error("cannot write " + path.generic_string());
	}

	std::string ReadText(const fs::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
			throw std::runtime_error("cannot read " + path.generic_string());
		return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
	}

	// "<路径>:<行号>: <消息>" 里的行号段(不依赖具体措辞)。
	bool ContainsLineMarker(const std::string& text)
	{
		for (std::size_t index = 0; index + 2 < text.size(); ++index)
		{
			if (text[index] != ':')
				continue;
			std::size_t cursor = index + 1;
			while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])))
				++cursor;
			if (cursor > index + 1 && cursor < text.size() && text[cursor] == ':')
				return true;
			index = cursor;
		}
		return false;
	}

	std::string FieldIdText(const std::string& fieldName)
	{
		char buffer[32] = {};
		std::snprintf(buffer, sizeof(buffer), "0x%016llx",
			static_cast<unsigned long long>(BehaviorRegistry::LuaFieldId(fieldName)));
		return std::string(buffer);
	}

	// ---- 宿主探针:脚本通过它上报 OnUpdate 行为(注入必须发生在任何脚本编译之前)----

	std::vector<std::string> g_ProbeCalls;
	LuaScriptComponent* g_SafePointProbe = nullptr;
	bool g_SafePointRejected = false;
	std::string g_SafePointDiagnostic;

	ScriptValue ProbeImplementation(const ScriptValue* args, std::size_t argCount)
	{
		std::string tag;
		if (argCount > 0)
			args[0].AsString(&tag);
		if (tag == "safe-check" && g_SafePointProbe)
		{
			// 正在脚本回调里(RuntimeEntity 所属场景的 m_CallbackDepth > 0)→ 必须被拒绝。
			std::string diagnostic;
			g_SafePointRejected = !ScriptEngine::ReloadScript(*g_SafePointProbe, &diagnostic);
			g_SafePointDiagnostic = diagnostic;
			return ScriptValue::Nil();
		}
		g_ProbeCalls.push_back(tag);
		return ScriptValue::Nil();
	}

	bool ProbeCalled(const char* tag)
	{
		return std::find(g_ProbeCalls.begin(), g_ProbeCalls.end(), tag) != g_ProbeCalls.end();
	}

	int ProbeCount(const char* tag)
	{
		return static_cast<int>(std::count(g_ProbeCalls.begin(), g_ProbeCalls.end(), tag));
	}

	// 带行号的字段查表:缺失字段时报出具体行与字段名,而不是 STL 的通用 out_of_range 文本。
	LuaScriptField& Field(std::unordered_map<std::string, LuaScriptField>& fields,
		const char* name, int line)
	{
		const auto found = fields.find(name);
		if (found == fields.end())
			throw std::runtime_error(std::string("line ") + std::to_string(line) +
				": script field '" + name + "' is missing");
		return found->second;
	}
#define FIELD(fields, name) Field((fields), (name), __LINE__)

	// ---- 夹具脚本 ----

	const char* const kMigrateV1 = R"LUA(---@field A integer
---@field B integer
return {
    A = 1,
    B = 2,
    OnCreate = function(self) HotReloadProbe("create:v1") end,
    OnUpdate = function(self) HotReloadProbe("update:v1") end,
    OnDestroy = function(self) HotReloadProbe("destroy:v1") end,
}
)LUA";

	// v2:A/B 声明与表项都重排,新增 C;C 取默认,B 保留,A 用迁移后的实例值。
	const char* const kMigrateV2 = R"LUA(---@field C integer
---@field B integer
---@field A integer
return {
    C = 30,
    B = 2,
    A = 1,
    OnCreate = function(self) HotReloadProbe("create:v2") end,
    OnUpdate = function(self)
        HotReloadProbe("update:v2")
        HotReloadProbe("A=" .. tostring(self.A))
        HotReloadProbe("B=" .. tostring(self.B))
        HotReloadProbe("C=" .. tostring(self.C))
    end,
    OnDestroy = function(self) HotReloadProbe("destroy:v2") end,
}
)LUA";

	// 类型变化:A 由 integer 变 string → A 回新默认("fresh"),B 保留,诊断报类型变化。
	const char* const kTypeChangeV2 = R"LUA(---@field A string
---@field B integer
return {
    A = "fresh",
    B = 2,
    OnCreate = function(self) HotReloadProbe("create:type") end,
    OnUpdate = function(self)
        HotReloadProbe("update:type")
        HotReloadProbe("A=" .. tostring(self.A))
    end,
    OnDestroy = function(self) HotReloadProbe("destroy:type") end,
}
)LUA";

	// 字段删除:B 在新版本里消失 → 丢弃旧值 + 诊断。
	const char* const kTypeChangeV3 = R"LUA(---@field A string
return {
    A = "again",
    OnCreate = function(self) HotReloadProbe("create:type3") end,
    OnUpdate = function(self) HotReloadProbe("update:type3") end,
    OnDestroy = function(self) HotReloadProbe("destroy:type3") end,
}
)LUA";

	const char* const kRollbackV1 = R"LUA(---@field A integer
---@field B integer
return {
    A = 7,
    B = 2,
    OnCreate = function(self) HotReloadProbe("create:rollback:v1") end,
    OnUpdate = function(self) HotReloadProbe("update:rollback:v1") end,
    OnDestroy = function(self) HotReloadProbe("destroy:rollback:v1") end,
}
)LUA";

	// 第 4 行缺 ")" → 编译期错误,错误文本带 "<路径>:<行号>:"。
	const char* const kRollbackBrokenV2 = R"LUA(---@field A integer
return {
    A = 1,
    OnUpdate = function(self
}
)LUA";

	const char* const kRollbackFixedV2 = R"LUA(---@field A integer
---@field B integer
return {
    A = 7,
    B = 2,
    OnCreate = function(self) HotReloadProbe("create:rollback:v2") end,
    OnUpdate = function(self) HotReloadProbe("update:rollback:v2") end,
    OnDestroy = function(self) HotReloadProbe("destroy:rollback:v2") end,
}
)LUA";

	const char* const kSafetyV1 = R"LUA(---@field A integer
return {
    A = 1,
    OnCreate = function(self) HotReloadProbe("create:safety") end,
    OnUpdate = function(self) HotReloadProbe("safe-check") end,
    OnDestroy = function(self) HotReloadProbe("destroy:safety") end,
}
)LUA";

	const char* const kSafetyV2 = R"LUA(---@field A integer
---@field B integer
return {
    A = 1,
    B = 2,
    OnCreate = function(self) HotReloadProbe("create:safety") end,
    OnUpdate = function(self) HotReloadProbe("safe-check") end,
    OnDestroy = function(self) HotReloadProbe("destroy:safety") end,
}
)LUA";

	// 监听用例:每个版本的内容都不同(OnUpdate 里的版本标记不同),便于断言"以最后一份内容为准"。
	std::string WatchScriptSource(int version)
	{
		const std::string value = std::to_string(version);
		return std::string("---@field A integer\n---@field C integer\nreturn {\n    A = 1,\n    C = 1,\n"
			"    OnCreate = function(self) HotReloadProbe(\"create:watch\") end,\n"
			"    OnUpdate = function(self) HotReloadProbe(\"watch:v") + value + "\") end,\n"
			"    OnDestroy = function(self) HotReloadProbe(\"destroy:watch\") end,\n"
			"}\n";
	}

	// ---- 1. 指纹 + 字段迁移 ----

	void FingerprintAndFieldMigration()
	{
		const fs::path file = ScriptPath("hotreload_migrate.lua");
		WriteScript(file, kMigrateV1);
		const std::string logical = LogicalPath(file);

		const ScriptSourceFingerprint fingerprintV1 = FingerprintScriptSource(logical);
		CHECK(fingerprintV1.Exists);
		CHECK(fingerprintV1.FromContent);
		CHECK(fingerprintV1.Value != 0);
		CHECK(fingerprintV1.Value == FingerprintScriptText(ReadText(file)));
		CHECK(FingerprintScriptSource(logical + ".missing").Value == 0);
		CHECK(!FingerprintScriptSource(logical + ".missing").Exists);

		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "migration probe");
		LuaScriptComponent& script = entity.AddComponent<LuaScriptComponent>(logical);

		// 编辑器预览路径先登记 v1 的行为描述(A,B 两个字段)。
		CHECK(ScriptEngine::InitScriptForEditor(script));
		const BehaviorDesc* before = BehaviorRegistry::Instance().Find(BehaviorRegistry::LuaModuleId(logical));
		CHECK(before != nullptr);
		CHECK(before->Fields.size() == 2);

		scene.OnScriptStart();
		CHECK(script.State == ScriptInstanceState::Running);
		CHECK(script.CachedFields.size() == 2);
		const uint64_t sceneGeneration = script.Generation;
		CHECK(sceneGeneration != 0);
		CHECK(sceneGeneration < (uint64_t(1) << 63));   // Scene 启动期分配的是小整数

		// 跑一帧:v1 行为真实执行。
		g_ProbeCalls.clear();
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(ProbeCalled("update:v1"));
		CHECK(script.LastError.empty());

		// 改 A(实例状态由 CachedFields 持有,迁移以它为准)。
		const int tableRefBefore = script.ScriptTable.RefId();
		std::any_cast<int&>(FIELD(script.CachedFields, "A").Value) = 41;

		WriteScript(file, kMigrateV2);
		const ScriptSourceFingerprint fingerprintV2 = FingerprintScriptSource(logical);
		CHECK(fingerprintV2.FromContent);
		CHECK(fingerprintV2.Value != fingerprintV1.Value);

		std::string diagnostic;
		CHECK(ScriptEngine::ReloadScript(script, &diagnostic));
		CHECK(diagnostic.empty());                      // 新增字段取默认,不产生警告
		CHECK(script.ReloadDiagnostic.empty());         // 成功路径清空诊断
		CHECK(script.State == ScriptInstanceState::Running);
		CHECK(script.IsLoaded);
		CHECK(script.LastError.empty());
		CHECK(script.SourceFingerprint == fingerprintV2.Value);
		CHECK(script.ScriptTable.RefId() != tableRefBefore);   // 整体交换:脚本表引用换了

		// 字段迁移结果:A 保留实例值,B 保留,C 取新默认。
		CHECK(script.CachedFields.size() == 3);
		CHECK(FIELD(script.CachedFields, "A").Type == LuaFieldType::Int);
		CHECK(std::any_cast<int>(FIELD(script.CachedFields, "A").Value) == 41);
		CHECK(std::any_cast<int>(FIELD(script.CachedFields, "B").Value) == 2);
		CHECK(FIELD(script.CachedFields, "C").Type == LuaFieldType::Int);
		CHECK(std::any_cast<int>(FIELD(script.CachedFields, "C").Value) == 30);

		// 行为描述已刷新(按 LuaFieldId 稳定 id 断言):3 个字段、按 id 升序、类型映射不变。
		const BehaviorDesc* after = BehaviorRegistry::Instance().Find(BehaviorRegistry::LuaModuleId(logical));
		CHECK(after != nullptr);
		CHECK(after->Fields.size() == 3);
		CHECK(after->Language == BehaviorLanguage::Luau);
		std::vector<uint64_t> fieldIds;
		bool sawA = false, sawB = false, sawC = false;
		for (const BehaviorFieldDesc& field : after->Fields)
		{
			fieldIds.push_back(field.FieldId);
			if (field.Name == "A") { sawA = field.FieldId == BehaviorRegistry::LuaFieldId("A") && field.Type == Schema::Kind::Int32; }
			if (field.Name == "B") { sawB = field.FieldId == BehaviorRegistry::LuaFieldId("B") && field.Type == Schema::Kind::Int32; }
			if (field.Name == "C") { sawC = field.FieldId == BehaviorRegistry::LuaFieldId("C") && field.Type == Schema::Kind::Int32; }
		}
		CHECK(sawA && sawB && sawC);
		CHECK(std::is_sorted(fieldIds.begin(), fieldIds.end()));

		// 迁移后的实例字段对**新表**可见:下一帧 v2 行为读到 A=41,B=2,C=30。
		g_ProbeCalls.clear();
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(ProbeCount("update:v2") == 1);
		CHECK(ProbeCalled("A=41"));
		CHECK(ProbeCalled("B=2"));
		CHECK(ProbeCalled("C=30"));
		CHECK(script.LastError.empty());

		// generation:重载后进入独立域(最高位置位),与 Scene 分配值和上一次重载值都不同。
		const uint64_t firstReloadGeneration = script.Generation;
		CHECK(firstReloadGeneration >= (uint64_t(1) << 63));
		CHECK(firstReloadGeneration != sceneGeneration);
		CHECK(ScriptEngine::ReloadScript(script, &diagnostic));   // 同内容再重载一次也允许
		CHECK(script.Generation > firstReloadGeneration);
		CHECK(script.State == ScriptInstanceState::Running);

		scene.OnRuntimeStop();
		CHECK(script.State == ScriptInstanceState::Stopped);
	}

	// ---- 2. 类型变化 → 回新默认 + 诊断 ----

	void TypeChangeResetsFieldWithDiagnostic()
	{
		const fs::path file = ScriptPath("hotreload_typechange.lua");
		WriteScript(file, kMigrateV1);
		const std::string logical = LogicalPath(file);

		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "type change probe");
		LuaScriptComponent& script = entity.AddComponent<LuaScriptComponent>(logical);
		CHECK(ScriptEngine::InitScriptForEditor(script));   // CachedFields 由脚本注解/默认值构建
		scene.OnScriptStart();
		CHECK(script.State == ScriptInstanceState::Running);
		std::any_cast<int&>(FIELD(script.CachedFields, "A").Value) = 7;

		WriteScript(file, kTypeChangeV2);
		std::string diagnostic;
		CHECK(ScriptEngine::ReloadScript(script, &diagnostic));
		CHECK(!diagnostic.empty());
		CHECK(diagnostic.find("field 'A'") != std::string::npos);
		CHECK(diagnostic.find("type changed") != std::string::npos);
		CHECK(diagnostic.find("id=" + FieldIdText("A")) != std::string::npos);
		CHECK(script.ReloadDiagnostic.empty());   // 成功路径:ReloadDiagnostic 清空,警告走 diagnostics
		CHECK(script.State == ScriptInstanceState::Running);

		CHECK(FIELD(script.CachedFields, "A").Type == LuaFieldType::String);
		CHECK(std::any_cast<std::string>(FIELD(script.CachedFields, "A").Value) == "fresh");
		CHECK(std::any_cast<int>(FIELD(script.CachedFields, "B").Value) == 2);

		g_ProbeCalls.clear();
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(ProbeCalled("update:type"));
		CHECK(ProbeCalled("A=fresh"));
		CHECK(script.LastError.empty());

		// 字段被删除(B 从新版本消失)→ 丢弃旧值 + 诊断。
		WriteScript(file, kTypeChangeV3);
		diagnostic.clear();
		CHECK(ScriptEngine::ReloadScript(script, &diagnostic));
		CHECK(diagnostic.find("field 'B'") != std::string::npos);
		CHECK(diagnostic.find("missing in the new script") != std::string::npos);
		CHECK(diagnostic.find("id=" + FieldIdText("B")) != std::string::npos);
		CHECK(script.CachedFields.size() == 1);
		CHECK(FIELD(script.CachedFields, "A").Type == LuaFieldType::String);
		// 同名同类型 → 保留旧值("fresh"),新脚本里的 "again" 只是默认值。
		CHECK(std::any_cast<std::string>(FIELD(script.CachedFields, "A").Value) == "fresh");

		g_ProbeCalls.clear();
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(ProbeCalled("update:type3"));
		CHECK(script.LastError.empty());

		scene.OnRuntimeStop();
	}

	// ---- 3. 失败回滚:语法错误保留旧版本与旧行为 ----

	void FailedReloadKeepsOldVersion()
	{
		const fs::path file = ScriptPath("hotreload_rollback.lua");
		WriteScript(file, kRollbackV1);
		const std::string logical = LogicalPath(file);

		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "rollback probe");
		LuaScriptComponent& script = entity.AddComponent<LuaScriptComponent>(logical);
		CHECK(ScriptEngine::InitScriptForEditor(script));   // CachedFields 由脚本注解/默认值构建
		scene.OnScriptStart();
		CHECK(script.State == ScriptInstanceState::Running);
		std::any_cast<int&>(FIELD(script.CachedFields, "A").Value) = 41;

		g_ProbeCalls.clear();
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(ProbeCalled("update:rollback:v1"));

		const int environmentRef = script.LuaEnv.RefId();
		const int tableRef = script.ScriptTable.RefId();
		const int updateRef = script.OnUpdateFunc.RefId();
		const std::string lastError = script.LastError;

		WriteScript(file, kRollbackBrokenV2);
		std::string diagnostic;
		CHECK(!ScriptEngine::ReloadScript(script, &diagnostic));
		CHECK(!diagnostic.empty());
		CHECK(diagnostic.find(logical) != std::string::npos);   // 含脚本路径
		CHECK(ContainsLineMarker(diagnostic));                   // 含编译器给的行号
		CHECK(script.ReloadDiagnostic == diagnostic);

		// 失败语义:State 不被置 Faulted、旧引用与字段不动、LastError 不被污染。
		CHECK(script.State == ScriptInstanceState::Running);
		CHECK(script.IsLoaded);
		CHECK(script.LastError.empty());
		CHECK(script.LastError == lastError);
		CHECK(script.LuaEnv.RefId() == environmentRef);
		CHECK(script.ScriptTable.RefId() == tableRef);
		CHECK(script.OnUpdateFunc.RefId() == updateRef);
		CHECK(std::any_cast<int>(FIELD(script.CachedFields, "A").Value) == 41);

		// 下一帧仍然执行 v1 的行为。
		g_ProbeCalls.clear();
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(ProbeCount("update:rollback:v1") == 1);
		CHECK(ProbeCount("update:rollback:v2") == 0);
		CHECK(script.State == ScriptInstanceState::Running);

		// 修好之后同一实例可以继续重载(失败不粘滞)。
		WriteScript(file, kRollbackFixedV2);
		diagnostic.clear();
		CHECK(ScriptEngine::ReloadScript(script, &diagnostic));
		CHECK(script.ReloadDiagnostic.empty());
		g_ProbeCalls.clear();
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(ProbeCount("update:rollback:v2") == 1);
		CHECK(ProbeCount("update:rollback:v1") == 0);

		scene.OnRuntimeStop();
	}

	// ---- 4. 轮询监听:同内容不重载 + debounce 归并 ----

	void WatcherDebouncesAndIgnoresSameContent()
	{
		const fs::path file = ScriptPath("hotreload_watch.lua");
		WriteScript(file, WatchScriptSource(1));
		const std::string logical = LogicalPath(file);

		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "watch probe");
		LuaScriptComponent& script = entity.AddComponent<LuaScriptComponent>(logical);
		CHECK(ScriptEngine::InitScriptForEditor(script));   // CachedFields 由脚本注解/默认值构建
		scene.OnScriptStart();
		CHECK(script.State == ScriptInstanceState::Running);
		CHECK(std::any_cast<int>(FIELD(script.CachedFields, "C").Value) == 1);

		ScriptFileWatch watch;
		CHECK(watch.DebounceSeconds() == ScriptFileWatch::kDefaultDebounceSeconds);
		CHECK(ScriptFileWatch::kDefaultDebounceSeconds >= 0.1);
		CHECK(ScriptFileWatch::kDefaultDebounceSeconds <= 0.2);
		watch.Watch(logical);
		CHECK(watch.Size() == 1);
		CHECK(watch.IsWatched(logical));

		// 不改内容 → 零次。
		CHECK(watch.Poll(0.2).empty());
		CHECK(watch.Poll(0.2).empty());

		// 同内容重写(只动 mtime)→ 内容哈希优先,不算变化。
		const std::string unchanged = ReadText(file);
		WriteScript(file, unchanged);
		CHECK(watch.Poll(0.2).empty());

		// 改内容:v2 → debounce 内不报告,稳定后恰好一次。
		WriteScript(file, WatchScriptSource(2));
		CHECK(watch.Poll(0.05).empty());
		CHECK(watch.Poll(0.05).empty());
		CHECK(watch.Poll(0.05).empty());
		const std::vector<std::string> firstChange = watch.Poll(0.05);
		CHECK(firstChange.size() == 1);
		CHECK(firstChange.front() == logical);
		int reloads = 0;
		std::string diagnostic;
		for (const std::string& path : firstChange)
		{
			CHECK(path == logical);
			++reloads;
			CHECK(ScriptEngine::ReloadScript(script, &diagnostic));
		}
		CHECK(reloads == 1);
		// 同名同类型 → 实例值保留(这里 C 仍是初始的 1;版本变化体现在代码标记上)。
		CHECK(std::any_cast<int>(FIELD(script.CachedFields, "C").Value) == 1);
		CHECK(watch.Poll(0.5).empty());   // 恰好一次:没有重复报告
		CHECK(reloads == 1);
		g_ProbeCalls.clear();
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(ProbeCalled("watch:v2"));

		// 连续写:v3 后立刻 v4 → 归并成一次,并以最后一份内容为准。
		WriteScript(file, WatchScriptSource(3));
		CHECK(watch.Poll(0.05).empty());
		WriteScript(file, WatchScriptSource(4));
		int changeCount = 0;
		for (int step = 0; step < 4; ++step)
		{
			const std::vector<std::string> changed = watch.Poll(0.05);
			changeCount += static_cast<int>(changed.size());
			for (const std::string& path : changed)
			{
				CHECK(path == logical);
				++reloads;
				CHECK(ScriptEngine::ReloadScript(script, &diagnostic));
			}
		}
		CHECK(changeCount == 1);
		CHECK(reloads == 2);   // v2 一次 + v3/v4 归并一次
		CHECK(std::any_cast<int>(FIELD(script.CachedFields, "C").Value) == 1);   // 字段值继续保留
		CHECK(script.SourceFingerprint == FingerprintScriptText(ReadText(file)));

		g_ProbeCalls.clear();
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(ProbeCalled("watch:v4"));      // 以最后一份内容为准
		CHECK(!ProbeCalled("watch:v3"));     // 中间版本从未被加载
		CHECK(script.LastError.empty());

		watch.Unwatch(logical);
		CHECK(watch.Size() == 0);
		watch.Watch(logical);
		watch.Clear();
		CHECK(watch.Size() == 0);

		scene.OnRuntimeStop();
	}

	// ---- 5. 拒绝语义:状态 + 安全点 ----

	void RejectsUnsafeStatesAndCallbackReload()
	{
		const fs::path file = ScriptPath("hotreload_safety.lua");
		WriteScript(file, kSafetyV1);
		const std::string logical = LogicalPath(file);

		Scene scene(TestContext());
		Entity entity = Entity::CreateEntity(&scene, "safety probe");
		LuaScriptComponent& script = entity.AddComponent<LuaScriptComponent>(logical);
		CHECK(ScriptEngine::InitScriptForEditor(script));   // CachedFields 由脚本注解/默认值构建
		scene.OnScriptStart();
		CHECK(script.State == ScriptInstanceState::Running);

		const int tableRef = script.ScriptTable.RefId();
		std::string diagnostic;

		// State=Creating → 拒绝,不动旧引用。
		script.State = ScriptInstanceState::Creating;
		CHECK(!ScriptEngine::ReloadScript(script, &diagnostic));
		CHECK(diagnostic.find("Creating") != std::string::npos);
		CHECK(script.ReloadDiagnostic == diagnostic);
		CHECK(script.ScriptTable.RefId() == tableRef);

		// State=Destroying → 拒绝,不动旧引用。
		script.State = ScriptInstanceState::Destroying;
		diagnostic.clear();
		CHECK(!ScriptEngine::ReloadScript(script, &diagnostic));
		CHECK(diagnostic.find("Destroying") != std::string::npos);
		CHECK(script.ScriptTable.RefId() == tableRef);

		// 没有活动实例(Pending/Stopped/Faulted)→ 拒绝(没有可回滚的旧版本)。
		script.State = ScriptInstanceState::Stopped;
		diagnostic.clear();
		CHECK(!ScriptEngine::ReloadScript(script, &diagnostic));
		CHECK(diagnostic.find("running script instance") != std::string::npos);
		script.State = ScriptInstanceState::Running;

		// 回调内(OnUpdate 里通过探针调用)→ 非安全点,拒绝;回到帧边界后同样的重载成功。
		WriteScript(file, kSafetyV2);
		g_SafePointProbe = &script;
		g_SafePointRejected = false;
		g_SafePointDiagnostic.clear();
		g_ProbeCalls.clear();
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(g_SafePointRejected);
		CHECK(g_SafePointDiagnostic.find("safe point") != std::string::npos);
		CHECK(script.State == ScriptInstanceState::Running);
		CHECK(script.ReloadDiagnostic.find("safe point") != std::string::npos);
		CHECK(script.CachedFields.size() == 1);   // 回调内的拒绝没有换掉任何东西

		g_SafePointProbe = nullptr;
		diagnostic.clear();
		CHECK(ScriptEngine::ReloadScript(script, &diagnostic));
		CHECK(script.ReloadDiagnostic.empty());
		CHECK(script.CachedFields.size() == 2);

		g_ProbeCalls.clear();
		scene.OnScriptUpdate(Timestep(1.0f / 60.0f));
		CHECK(ProbeCount("safe-check") == 1);     // 探针不再调用重载,只上报
		CHECK(script.LastError.empty());

		scene.OnRuntimeStop();
	}
}

int main()
{
	try
	{
		World::Log::Init();
		World::ScriptEngine::Init();
		// 宿主探针必须在任何脚本编译之前注入(无 env chunk 在 load 期解析 import)。
		World::ScriptEngine::GetState().SetGlobal("HotReloadProbe",
			World::ScriptEngine::GetBindingContext().CreateFunction("HotReloadProbe", &ProbeImplementation));

		g_RunDirectory = fs::path(WORLD_HOTRELOAD_TEST_OUTPUT_DIR) /
			("run-" + std::to_string(::GetCurrentProcessId()));
		fs::create_directories(g_RunDirectory);

		const std::pair<const char*, void(*)()> tests[] = {
			{ "source fingerprint and field migration", FingerprintAndFieldMigration },
			{ "type change resets the field and reports a diagnostic", TypeChangeResetsFieldWithDiagnostic },
			{ "failed reload keeps the old version and its behaviour", FailedReloadKeepsOldVersion },
			{ "watcher ignores same content and debounces consecutive writes", WatcherDebouncesAndIgnoresSameContent },
			{ "unsafe states and in-callback reloads are rejected", RejectsUnsafeStatesAndCallbackReload },
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
			std::printf("World.ScriptHotReload: all checks passed\n");
			std::error_code cleanupError;
			fs::remove_all(g_RunDirectory, cleanupError);   // 只删本次 run-<pid> 临时目录
			return 0;
		}
		std::fprintf(stderr, "World.ScriptHotReload: %d group(s) failed\n", failures);
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

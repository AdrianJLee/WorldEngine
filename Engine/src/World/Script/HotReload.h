#pragma once

// P2 W5:L2 脚本热重载的引擎侧支撑(指纹 / 源解析 / 迁移诊断)。
//
// 本文件只做三件与 VM 无关的事:
//   1. 脚本源指纹:内容 FNV-1a64 优先,mtime+size 兜底(同一内容不重载);
//   2. 逻辑脚本路径 → 原始字节/源码文本:VFS 优先、磁盘回退(与 ScriptEngine 读取语义一致);
//   3. 实例字段迁移诊断:LuaFieldId 派生的稳定 id + 类型兼容规则的可读说明。
//
// 真正的重载编排(读取 → 编译 → 回调校验 → 字段迁移 → 整体交换 → generation)
// 在 ScriptEngine::ReloadScript;监听器在 Script/ScriptFileWatch.h。

#include "World/Core/Export.h"
#include "World/Scene/Components.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace World
{
	// 一次脚本源指纹计算的结果。
	//   Value=0        → 既读不到内容也 stat 不到(文件不存在/不可读);
	//   FromContent=true  → Value 是文件内容的 FNV-1a64;
	//   FromContent=false → Value 是 mtime+size 的 FNV-1a64(磁盘回退兜底)。
	// 相等判定只看 Exists+Value:内容一致时即使一个走了 mtime 兜底也不算变化。
	struct ScriptSourceFingerprint
	{
		uint64_t Value = 0;
		bool FromContent = false;
		bool Exists = false;

		bool operator==(const ScriptSourceFingerprint& other) const
		{
			return Exists == other.Exists && Value == other.Value;
		}
		bool operator!=(const ScriptSourceFingerprint& other) const { return !(*this == other); }
	};

	// FNV-1a64 over raw bytes / text(空输入同样有确定值:offset basis)。
	WLD_API uint64_t FingerprintScriptBytes(const void* data, std::size_t size);
	WLD_API uint64_t FingerprintScriptText(const std::string& text);

	// 逻辑脚本路径 → 指纹:VFS 优先,磁盘回退;内容读不到时用 size+mtime 兜底。
	// 两者都拿不到 → Exists=false / Value=0(可读 error 说明失败原因,可为 null)。
	WLD_API ScriptSourceFingerprint FingerprintScriptSource(const std::string& logicalPath,
		std::string* error = nullptr);

	// W7-3:逻辑脚本路径 → **原始字节**(VFS 优先,磁盘回退到 WLD_ASSETPATH/<path>)。
	// 与 ResolveScriptSource 同一解析顺序,但不做文本转换:容器字节里的 '\0' 必须原样保留
	// (指纹/装载都吃这一份字节)。失败返回 false、out 保持调用前内容,
	// error 写 "script not found: <path>" 一类的可读文本(可为 null)。
	WLD_API bool ResolveScriptSourceBytes(const std::string& logicalPath, std::vector<uint8_t>& out,
		std::string* error = nullptr);

	// 逻辑脚本路径 → 源码文本(VFS 优先,磁盘回退到 WLD_ASSETPATH/<path>)。
	// 失败返回 false,source 保持调用前内容,error 写"script not found: <path>"一类的可读文本。
	WLD_API bool ResolveScriptSource(const std::string& logicalPath, std::string& source,
		std::string* error = nullptr);

	// W8:逻辑脚本路径 → **可编辑的磁盘绝对路径**(编辑器 Scripts 面板的 Open/新建提示用)。
	// 与 ResolveScriptSourceBytes 的分工:那个管"读字节给编译器",这里管"去哪个文件写/打开"。
	// 语义:
	//   1. Vfs::Normalize 校验(拒绝绝对路径/盘符/空串/NUL/"."/".." 段);非法 → false + error;
	//   2. VFS 命中且来源是 Package → 不可编辑(error 文本含"包内不可编辑");
	//   3. 否则要求 WLD_ASSETPATH/<path> 是常规文件,成功时 out = 该文件的绝对路径。
	// 失败返回 false,out 保持调用前内容,error 写可读文本(可为 null)。
	WLD_API bool ResolveScriptDiskPath(std::string_view logicalPath, std::filesystem::path& out,
		std::string* error = nullptr);

	// W8:LSP 脚手架 create-if-missing(声明在此、实现见 HotReload.cpp)。
	// 在 contentRoot 下写 `.vscode/settings.json` 与 `.luau-lsp/config.json`(luau-lsp 指向
	// `scripts/intermediate/WorldEngineAPI.luau` 存根并忽略 intermediate/)。
	// **磁盘上已存在的文件一律保留**(无论内容):用户改过的配置不被生成物覆盖。
	// 需要时创建父目录;成功返回 true,error 写首个失败的路径(可为 null)。
	WLD_API bool EnsureScriptEditorScaffold(const std::filesystem::path& contentRoot,
		std::string* error = nullptr);

	// 字段迁移诊断规则(previous = 旧实例状态,next = 新脚本合并后的字段表):
	//   - 旧无、新有(新增字段):取新脚本默认值,不产生诊断;
	//   - 同名同类型:保留旧值(合并由 ScriptEngine::BuildFieldCache 完成);
	//   - 同名类型变化:回新默认值 + 一条诊断(含 BehaviorRegistry::LuaFieldId 稳定 id);
	//   - 旧有新无(字段被删):丢弃旧值 + 一条诊断(含稳定 id)。
	// 输出按字段名升序,保证跨平台/跨运行稳定;diagnostics 可为 null。
	WLD_API void DescribeScriptFieldMigration(
		const std::unordered_map<std::string, LuaScriptField>& previous,
		const std::unordered_map<std::string, LuaScriptField>& next,
		const std::string& scriptPath,
		std::vector<std::string>* diagnostics);

	// 失败诊断统一格式:`[hot-reload] <脚本路径>: <phase> failed: <error>`。
	// 编译器给的错误文本本身带 "<路径>:<行号>:" 前缀时,行号原样保留。
	WLD_API std::string FormatScriptReloadFailure(const std::string& scriptPath,
		const char* phase, const std::string& error);
}

#pragma once

#include "World/Core/Export.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace World::Ai
{
	// AI 控制命令副作用
	enum class CommandEffect : uint8_t
	{
		Read,   // 只读查询，不产生任何环境或资产修改
		Write,  // 修改场景/资产/脚本/文件等持久化状态
		Exec    // 执行模拟、回放、运行、构建或外部命令
	};

	// AI 控制命令作用域
	enum class CommandScope : uint8_t
	{
		EditorOnly,  // 仅在编辑器宿主可用 (依赖 UI/编辑器图层)
		RuntimeOnly, // 仅在无头/独立 Runtime 可用
		Both         // 两者皆可用
	};

	WLD_API const char* CommandEffectToString(CommandEffect effect);
	WLD_API CommandEffect StringToCommandEffect(const std::string& str);

	WLD_API const char* CommandScopeToString(CommandScope scope);
	WLD_API CommandScope StringToCommandScope(const std::string& str);

	// 单条 AI 控制命令的完整元数据描述与执行句柄
	struct WLD_API CommandDescriptor
	{
		using Handler = std::function<bool(const std::map<std::string, std::string>& args,
			std::string& result, std::string& error)>;

		std::string name;
		std::string description;
		CommandEffect effect = CommandEffect::Read;
		CommandScope scope = CommandScope::Both;
		std::string owner = "engine"; // "engine" 或 "plugin:<id>"
		std::string paramsSchema = "{}"; // JSON Schema 字符串
		std::string resultSchema = "{}"; // JSON Schema 字符串
		Handler handler;
	};

	// 引擎级统一 AI 控制命令注册表与能力派发中心
	class WLD_API CommandRegistry
	{
	public:
		static CommandRegistry& Get();

		CommandRegistry();
		~CommandRegistry() = default;

		// 禁用拷贝与移动
		CommandRegistry(const CommandRegistry&) = delete;
		CommandRegistry& operator=(const CommandRegistry&) = delete;

		// 注册与注销命令
		bool RegisterCommand(CommandDescriptor desc);
		bool UnregisterCommand(const std::string& name);
		bool HasCommand(const std::string& name) const;
		const CommandDescriptor* GetDescriptor(const std::string& name) const;
		std::vector<CommandDescriptor> GetAllDescriptors() const;

		// 统一派发命令
		bool Dispatch(const std::string& name,
			const std::map<std::string, std::string>& args,
			std::string& result,
			std::string& error);

		// 导出机器可读的 capabilities JSON Schema 清单
		// 支持按 scope ("editor" | "runtime" | "both") 与 effect ("read" | "write" | "exec") 过滤
		std::string ExportCapabilities(const std::string& filterScope = "",
			const std::string& filterEffect = "") const;

		// 清理非引擎核心注册命令（主要用于测试复位）
		void ClearUserCommands();

		// 清空全部命令（含内置命令，用于隔离单测）
		void ResetAll();

	private:
		void RegisterBuiltinCommands();

		mutable std::mutex m_Mutex;
		std::map<std::string, CommandDescriptor> m_Commands;
	};
}

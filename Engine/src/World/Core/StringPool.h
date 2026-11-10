#pragma once

#include "World/Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace World
{
	// ---------------------------------------------------------------------------
	// 驻留标识(2026-10-04)。
	//
	// 问题:组件里的 `std::string`(MSVC x64 = 32B、堆持有、非平凡拷贝)被当成**标识符**用。
	// 标识符应当是**值**:4B、平凡、可比较、无堆。
	//
	// 机制:进程内字符串驻留表(StringPool)。字符串只在边界层(IO / 目录 / 导入器 /
	// 诊断)出现一次;热数据里只流 4B id。比对/哈希退化为整数操作,复制退化为 memcpy。
	//
	// 两类 id 语义不同、空间独立:
	//   * `PathId` —— 资产**逻辑路径**(相对内容根)。InternPath 会做归一化。
	//   * `NameId` —— 名字标识(实体 Tag、动画 clip 名等)。InternName 原样保存。
	// 0 恒表示"无效/空",永远不等于任何驻留结果。
	//
	// 稳定性:同一进程内同一字符串恒得同一 id;id 不跨进程持久化(需要跨进程稳定身份时
	// 用 `AssetId`,那是资产层的持久身份,不是本层的驻留 id)。
	// ---------------------------------------------------------------------------

	struct PathId
	{
		uint32_t Value = 0;
		constexpr bool IsValid() const { return Value != 0; }
		constexpr bool operator==(const PathId& other) const { return Value == other.Value; }
		constexpr bool operator!=(const PathId& other) const { return Value != other.Value; }
	};

	struct NameId
	{
		uint32_t Value = 0;
		constexpr bool IsValid() const { return Value != 0; }
		constexpr bool operator==(const NameId& other) const { return Value == other.Value; }
		constexpr bool operator!=(const NameId& other) const { return Value != other.Value; }
	};

	// 进程内唯一。符号随 WorldRuntime.dll 导出,保证 Editor / Runtime / Game 读到同一实例
	// (先例:`AssetTypeRegistry::Get()`;头文件里放 inline 单例会各自生成副本)。
	//
	// 线程模型:内部 shared_mutex。读(PathOf/NameOf)可并发;Intern 取独占锁。
	// 约定:Intern 只应发生在加载/反序列化/导入等**串行**阶段;禁止在并行系统
	// (JobSystem 工作项)里大批 Intern —— 那是共享容器写,会与既有 job-system 契约冲突。
	class WLD_API StringPool
	{
	public:
		static StringPool& Get();

		// 路径归一化:统一 '/'、去掉前导 "./"、折叠冗余分隔,再做 lexically_normal。
		// 空输入 ⇒ 空串。归一化结果就是驻留键(同一资产的不同写法得到同一 PathId)。
		static std::string NormalizePath(std::string_view path);

		// 驻留。空输入 ⇒ {0}。同一(归一化后的)字符串恒返回同一 id。
		PathId InternPath(std::string_view logicalPath);
		NameId InternName(std::string_view name);

		// 反查。未驻留 ⇒ 空串。返回引用与池同寿(元素存储用 deque,插入不失效)。
		const std::string& PathOf(PathId id) const;
		const std::string& NameOf(NameId id) const;

		// 诊断(内存面板/测试):两类合计条数与近似字节占用。
		std::size_t Count() const;
		std::size_t BytesInUse() const;

	private:
		StringPool() = default;
		StringPool(const StringPool&) = delete;
		StringPool& operator=(const StringPool&) = delete;

		struct Table
		{
			std::deque<std::string> Storage;                     // id = 下标 + 1
			std::unordered_map<std::string_view, uint32_t> Index; // 视图指向 Storage 元素(稳定)
			std::size_t Bytes = 0;
		};

		uint32_t Intern(Table& table, const std::string& value);
		static const std::string& Lookup(const Table& table, uint32_t id);

		mutable std::shared_mutex m_Mutex;
		Table m_Paths;
		Table m_Names;
	};
}

namespace std
{
	template <> struct hash<World::PathId>
	{
		std::size_t operator()(const World::PathId& id) const noexcept { return hash<uint32_t>()(id.Value); }
	};
	template <> struct hash<World::NameId>
	{
		std::size_t operator()(const World::NameId& id) const noexcept { return hash<uint32_t>()(id.Value); }
	};
}

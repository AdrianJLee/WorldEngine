#pragma once

// P2 W5:L2 脚本文件监听(轮询式,不引 OS watcher)。
//
// 语义(对齐 W5 方案的"同内容不重载 + debounce 100-200ms"):
//   - Watch() 只建立基线指纹,首次登记不产生变化;
//   - Poll(dtSeconds) 重新计算指纹:与已确认指纹一致 → 无变化;内容变化后要连续
//     稳定 debounce 秒(期间没有任何新内容)才报告一次;
//   - 同内容重写(只动 mtime)不算变化(内容哈希优先,见 HotReload.h);
//   - debounce 窗口内连续写入只保留最后一份内容 → 归并成一次;
//   - 内容改回已确认值 → 撤销未决变化,不报告。
// 返回的路径按字典序排序,宿主可按稳定顺序处理(重载脚本、刷新面板等)。

#include "World/Core/Export.h"
#include "World/Script/HotReload.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace World
{
	class WLD_API ScriptFileWatch
	{
	public:
		// 150ms:落在方案要求的 100-200ms 区间。
		static constexpr double kDefaultDebounceSeconds = 0.15;

		ScriptFileWatch() = default;
		explicit ScriptFileWatch(double debounceSeconds);

		// 登记逻辑脚本路径并立即取一次基线指纹;重复登记 = 用当前内容重置基线。
		void Watch(const std::string& logicalPath);
		void Unwatch(const std::string& logicalPath);
		void Clear();

		std::size_t Size() const { return m_Entries.size(); }
		bool IsWatched(const std::string& logicalPath) const;
		double DebounceSeconds() const { return m_DebounceSeconds; }

		// 推进 deltaSeconds 并返回"已过 debounce 的稳定变化"路径集合(升序)。
		std::vector<std::string> Poll(double deltaSeconds);
		// 用 steady_clock 自行计算 delta 的便捷入口(宿主每帧调用;首次调用 delta=0)。
		std::vector<std::string> PollNow();

	private:
		struct Entry
		{
			ScriptSourceFingerprint Stable;    // 已确认(不报告)的指纹
			ScriptSourceFingerprint Pending;   // 未决内容
			double Elapsed = 0.0;              // Pending 已稳定持续的秒数
			bool HasPending = false;
		};

		std::unordered_map<std::string, Entry> m_Entries;
		double m_DebounceSeconds = kDefaultDebounceSeconds;
		std::chrono::steady_clock::time_point m_LastPoll;
		bool m_HasLastPoll = false;
	};
}

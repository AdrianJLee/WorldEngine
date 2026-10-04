#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace World::Vfs
{
	using Path = std::string;   // POSIX 分隔、相对、无 ".."、无盘符

	// 归一化：'\\'->'/'，折叠重复 '/'，去掉尾部 '/'。
	// 拒绝：空串、".." 段、"." 段、绝对路径（含 '/' 开头与盘符）、NUL。拒绝时 out 保持空。
	bool Normalize(std::string_view raw, Path& out, std::error_code& ec);

	enum class Source : uint8_t { None = 0, Directory, Package };

	struct StatInfo
	{
		Source source = Source::None;
		std::string mountId;
		uint64_t size = 0;
		bool isDirectory = false;
	};

	class IVfsProvider
	{
	public:
		virtual ~IVfsProvider() = default;
		virtual Source SourceType() const = 0;
		virtual bool Open(const Path& path, std::vector<uint8_t>& out, std::error_code& ec) const = 0;
		virtual bool Stat(const Path& path, StatInfo& out, std::error_code& ec) const = 0;
	};

	using ProviderPtr = std::shared_ptr<IVfsProvider>;
	using MountId = uint64_t;   // 0 = 无效

	class Vfs
	{
	public:
		MountId Mount(std::string id, ProviderPtr provider, int priority);
		// id 非空且唯一（重复 id 返回 0 并记错误）；priority 数值大者优先命中；
		// 同 priority 后挂载者先命中（LIFO）。
		bool Unmount(MountId id);
		size_t MountCount() const;
		void Clear();
		bool Resolve(const Path& path, StatInfo& out, std::error_code& ec) const;   // 找第一个命中
		bool Read(const Path& path, std::vector<uint8_t>& out, std::error_code& ec) const;
		bool Stat(const Path& path, StatInfo& out, std::error_code& ec) const { return Resolve(path, out, ec); }
		bool Exists(const Path& path) const;

	private:
		struct MountEntry
		{
			MountId id = 0;
			std::string name;
			ProviderPtr provider;
			int priority = 0;
			uint64_t sequence = 0;
		};

		// 只在**已持锁**时调用(public 入口负责加锁;内部互相调用不得再取锁 ——
		// std::shared_mutex 的递归共享锁是未定义行为)。
		const MountEntry* ResolveMount(const Path& normalized, StatInfo& out) const;
		bool ResolveUnlocked(const Path& path, StatInfo& out, std::error_code& ec) const;

		// T5c:挂载表可被**工作线程**并发读(异步加载器读 VFS 取字节)。
		// 读(Resolve/Read/Stat/Exists/MountCount)= 共享锁;挂载表变更(Mount/Unmount/Clear)= 独占锁。
		// 契约:运行期不重挂载(只有启动期 Mount、换项目 Clear);这里的锁让"并发读"从"约定安全"
		// 变成"结构安全",代价只有一次无竞争的 shared_lock。
		mutable std::shared_mutex m_Mutex;
		std::vector<MountEntry> m_Mounts;
		MountId m_NextId = 1;
		uint64_t m_Sequence = 0;
	};

}

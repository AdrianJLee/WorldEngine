#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
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
		size_t MountCount() const { return m_Mounts.size(); }
		void Clear() { m_Mounts.clear(); }
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

		const MountEntry* ResolveMount(const Path& normalized, StatInfo& out) const;

		std::vector<MountEntry> m_Mounts;
		MountId m_NextId = 1;
		uint64_t m_Sequence = 0;
	};

}

#pragma once

namespace World
{
	// 引擎运行时的虚拟文件系统 (Virtual File System)
	class VFS
	{
	public:
		// Pak 文件头部
		struct PakHeader
		{
			uint32_t Magic = 0x4B415057; // 'WPAK' 的 ASCII 码
			uint32_t Version = 1;
			uint32_t IndexCount = 0;     // 文件数量
		};

		// 索引表项：记录每个文件在大包里的位置
		struct FileEntry
		{
			uint64_t Offset;
			uint64_t Size;
			std::string PakPath; // 新增：记录该文件属于哪个 pak 包
		};

		// 初始化 VFS，加载 .wpak 大文件
		static void Mount(const std::string& pakFilePath, bool clearPrevious = false);

		// 核心接口：通过相对路径（如 "textures/icon.png"）获取二进制数据
		static std::vector<uint8_t> ReadFile(const std::string& virtualPath);

		static void BuildPakFromDirectory(const std::filesystem::path& sourceDir, const std::filesystem::path& outPakPath);
	private:
		static std::unordered_map<std::string, FileEntry> s_IndexTable;
		static std::string s_MountedPakPath;

	};

}
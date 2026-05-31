#include "wldpch.h"
#include "VFS.h"
#include "World/Core/Thread/JobSystem.h"
namespace World
{
	std::unordered_map<std::string, VFS::FileEntry> VFS::s_IndexTable;
	std::string VFS::s_MountedPakPath;

	void VFS::Mount(const std::string& pakFilePath, bool clearPrevious)
	{
		s_MountedPakPath = pakFilePath;
		std::ifstream file(pakFilePath, std::ios::binary);
		if (!file.is_open())
		{
			WLD_CORE_ERROR("Failed to mount pak file: {0}", pakFilePath);
			return;
		}

		PakHeader header;
		if (!file.read((char*)&header, sizeof(PakHeader)))
		{
			WLD_CORE_ERROR("Failed to read header from pak file!");
			return;
		}

		if (header.Magic != 0x4B415057)
		{
			WLD_CORE_ERROR("Invalid pak file magic number!");
			return;
		}

		if (clearPrevious)
		{
			s_IndexTable.clear();
		}

		// 读取索引表
		for (uint32_t i = 0; i < header.IndexCount; i++)
		{
			uint32_t pathLen = 0;
			if (!file.read((char*)&pathLen, sizeof(uint32_t))) break;

			// 防御性校验，防止超大数值撑爆内存 (最大限制设为 1024 字节)
			if (pathLen == 0 || pathLen > 1024)
			{
				WLD_CORE_ERROR("Corrupted pak entry: invalid path length ({0})", pathLen);
				break;
			}

			std::string path;
			path.resize(pathLen);
			if (!file.read(path.data(), pathLen)) break;

			FileEntry entry;
			if (!file.read((char*)&entry.Offset, sizeof(uint64_t))) break;
			if (!file.read((char*)&entry.Size, sizeof(uint64_t))) break;
			entry.PakPath = pakFilePath;

			s_IndexTable[path] = entry;
		}

		WLD_CORE_INFO("Successfully mounted {0} (Indexed {1} files)", pakFilePath, header.IndexCount);
	}

	std::vector<uint8_t> VFS::ReadFile(const std::string& virtualPath)
	{
		if (s_IndexTable.find(virtualPath) == s_IndexTable.end())
		{
			return {}; // 找不到文件
		}

		const auto& entry = s_IndexTable[virtualPath];
		std::vector<uint8_t> buffer(entry.Size);

		std::ifstream file(entry.PakPath, std::ios::binary);
		if (file.is_open())
		{
			file.seekg(entry.Offset);
			file.read((char*)buffer.data(), entry.Size);
		}

		return buffer;
	}

	void VFS::BuildPakFromDirectory(const std::filesystem::path& sourceDir, const std::filesystem::path& outPakPath)
	{
		std::ofstream pakFile(outPakPath, std::ios::binary);
		if (!pakFile.is_open())
		{
			// 可以增加错误日志：无法创建打包文件
			WLD_CORE_ERROR("Failed to create pak file: {0}", outPakPath.string());
			return;
		}

		// 占位 Header
		VFS::PakHeader header = {};
		pakFile.write((char*)&header, sizeof(VFS::PakHeader));

		// 收集文件列表
		std::vector<std::pair<std::string, std::filesystem::path>> files;

		// 遍历目录，收集所有文件的相对路径和绝对路径
		for (const auto& entry : std::filesystem::recursive_directory_iterator(sourceDir))
		{
			// 我们只打包文件，目录结构通过相对路径保存在索引表里
			if (entry.is_regular_file())
			{
				std::string vPath = std::filesystem::relative(entry.path(), sourceDir).generic_string();
				files.push_back({ vPath, entry.path() });
			}
		}

		// 记录索引表在文件中的起始位置（我们先把数据写进去，最后再写索引表，这是一种常见优化）
		header.IndexCount = files.size();

		// 记录索引表起始位置，后续回填正确的 Offset 和 Size
		auto indexStartPos = pakFile.tellp();

		// 写入索引表 (带占位 Offset)
		std::vector<VFS::FileEntry> entries(files.size());


		for (size_t i = 0; i < files.size(); i++)
		{
			uint32_t len = static_cast<uint32_t>(files[i].first.size());
			pakFile.write((const char*)&len, sizeof(uint32_t)); // 写入路径长度
			pakFile.write(files[i].first.c_str(), len); // 写入路径字符串

			// 写入占位的 Offset 和 Size
			pakFile.write((const char*)&entries[i].Offset, sizeof(uint64_t)); // 写入文件偏移
			pakFile.write((const char*)&entries[i].Size, sizeof(uint64_t)); // 写入文件大小
		}

		// 申请 4MB 的复用缓冲区，分块读写以适应大文件且不引起瞬时大内存和避免堆碎片
		constexpr size_t CHUNK_SIZE = 1024 * 1024 * 4;
		// 这个缓冲区在循环中复用，避免每次读写都申请新的内存
		std::vector<char> buffer(CHUNK_SIZE);

		// 写入真实文件数据，并记录 Offset 和 Size
		for (size_t i = 0; i < files.size(); i++)
		{
			// 记录当前文件数据在 pak 中的偏移位置
			entries[i].Offset = pakFile.tellp();

			// 打开源文件
			std::ifstream inFile(files[i].second, std::ios::binary);
			if (!inFile.is_open())
			{
				// 防御性：文件打不开直接跳过（Size将保持为0）
				continue;
			}

			// 获取文件大小
			inFile.seekg(0, std::ios::end); // 移动到文件末尾以获取大小
			entries[i].Size = inFile.tellg();// 记录文件大小
			inFile.seekg(0, std::ios::beg); // 重置到文件开头准备读取

			// 分块倒腾数据
			uint64_t bytesLeft = entries[i].Size;
			while (bytesLeft > 0)
			{
				// 每次读取 CHUNK_SIZE 大小的数据，最后一次可能不足 CHUNK_SIZE
				std::streamsize toRead = static_cast<std::streamsize>(std::min<uint64_t>(bytesLeft, CHUNK_SIZE));
				inFile.read(buffer.data(), toRead); // 从源文件读取数据到缓冲区
				pakFile.write(buffer.data(), toRead); // 写入到 pak 文件
				bytesLeft -= toRead;
			}
		}

		// 回填正确的 Header 和 Index 偏移数据
		pakFile.seekp(0); // 回到文件开头写入 Header
		pakFile.write((const char*)&header, sizeof(VFS::PakHeader)); // 写入更新后的 Header	

		pakFile.seekp(indexStartPos);
		for (size_t i = 0; i < files.size(); i++)
		{
			uint32_t len = static_cast<uint32_t>(files[i].first.size());
			pakFile.seekp(sizeof(uint32_t) + len, std::ios::cur); // 跳过字符串
			pakFile.write((const char*)&entries[i].Offset, sizeof(uint64_t));
			pakFile.write((const char*)&entries[i].Size, sizeof(uint64_t));
		}
	}
}
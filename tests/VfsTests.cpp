#include "World/Core/Vfs/Vfs.h"
#include "World/Core/Vfs/DirectoryProvider.h"
#include "World/Core/Vfs/PackageProvider.h"
#include "World/Core/Vfs/Crc32.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using namespace World::Vfs;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	constexpr uint32_t kTestMagic = 0x4B415057u;        // "WPAK"

	struct TempDir
	{
		std::filesystem::path path;

		TempDir()
		{
			std::error_code ec;
			const uint64_t unique =
				static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
			path = std::filesystem::temp_directory_path(ec) / ("we-vfs-" + std::to_string(unique));
			std::filesystem::remove_all(path, ec);
			std::filesystem::create_directories(path, ec);
			if (ec)
				throw std::runtime_error("failed to create temp dir " + path.string() + ": " + ec.message());
		}

		~TempDir()
		{
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
		}
	};

	void WriteBytes(const std::filesystem::path& path, const std::string& content)
	{
		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);
		if (ec)
			throw std::runtime_error("create_directories failed: " + path.string());
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out.is_open())
			throw std::runtime_error("failed to open " + path.string());
		out.write(content.data(), static_cast<std::streamsize>(content.size()));
		if (!out)
			throw std::runtime_error("failed to write " + path.string());
	}

	void WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& content)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out.is_open())
			throw std::runtime_error("failed to open " + path.string());
		out.write(reinterpret_cast<const char*>(content.data()),
			static_cast<std::streamsize>(content.size()));
		if (!out)
			throw std::runtime_error("failed to write " + path.string());
	}

	std::vector<uint8_t> ReadBytes(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in.is_open())
			throw std::runtime_error("failed to open " + path.string());
		in.seekg(0, std::ios::end);
		const std::streamoff size = in.tellg();
		in.seekg(0, std::ios::beg);
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		in.read(reinterpret_cast<char*>(bytes.data()), size);
		if (!in)
			throw std::runtime_error("failed to read " + path.string());
		return bytes;
	}

	void PutU32(std::vector<uint8_t>& out, size_t pos, uint32_t value)
	{
		out[pos + 0] = static_cast<uint8_t>(value & 0xFFu);
		out[pos + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
		out[pos + 2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
		out[pos + 3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
	}

	using SyntheticEntry = std::pair<std::string, std::pair<uint64_t, uint64_t>>;

	// 写一个只含 header+JSON 索引的最小包(不含 data,offset/size 由调用方指定)。
	void WriteSyntheticPak(const std::filesystem::path& pak,
		uint32_t version,
		const std::vector<SyntheticEntry>& entries)
	{
		std::ostringstream json;
		json << "{\"version\":2,\"entries\":[";
		for (size_t i = 0; i < entries.size(); ++i)
		{
			if (i)
				json << ",";
			json << "{\"path\":\"" << entries[i].first << "\",\"hash\":\"0000000000000000\","
				<< "\"offset\":\"" << entries[i].second.first
				<< "\",\"size\":\"" << entries[i].second.second
				<< "\",\"crc32\":\"0\"}";
		}
		json << "]}";
		const std::string text = json.str();

		std::vector<uint8_t> bytes(16, 0);
		PutU32(bytes, 0, kTestMagic);
		PutU32(bytes, 4, version);
		PutU32(bytes, 8, static_cast<uint32_t>(text.size()));
		PutU32(bytes, 12, Crc32::Compute(text.data(), text.size()));
		bytes.insert(bytes.end(), text.begin(), text.end());
		WriteBytes(pak, bytes);
	}

	uint32_t ReadJsonLen(const std::vector<uint8_t>& pak)
	{
		return static_cast<uint32_t>(pak[8]) |
		       (static_cast<uint32_t>(pak[9]) << 8) |
		       (static_cast<uint32_t>(pak[10]) << 16) |
		       (static_cast<uint32_t>(pak[11]) << 24);
	}

	std::string AsString(const std::vector<uint8_t>& bytes)
	{
		return std::string(bytes.begin(), bytes.end());
	}
}

int main()
{
	try
	{
		TempDir temp;

		// 1. Normalize 合法/非法
		{
			Path out;
			std::error_code ec;
			CHECK(Normalize("textures/icon.png", out, ec) && out == "textures/icon.png");
			CHECK(Normalize("a\\b\\c", out, ec) && out == "a/b/c");
			CHECK(Normalize("dir//file", out, ec) && out == "dir/file");
			CHECK(Normalize("dir/file/", out, ec) && out == "dir/file");

			const char* invalid[] = {
				"", ".", "..", "../a", "a/../b", "/abs", "\\abs",
				"C:/x", "C:\\x", "a/./b", "//server/share"
			};
			for (const char* raw : invalid)
			{
				out = "sentinel";
				CHECK(!Normalize(raw, out, ec));
				CHECK(out.empty());
				CHECK(static_cast<bool>(ec));
			}

			std::string withNul("a");
			withNul.push_back('\0');
			withNul += "b";
			out = "sentinel";
			CHECK(!Normalize(withNul, out, ec));
			CHECK(out.empty());
		}

		// 2. 目录 provider:Open/Stat/来源
		{
			std::error_code ec;
			const std::filesystem::path root = temp.path / "dirroot";
			WriteBytes(root / "a.txt", "hello");
			WriteBytes(root / "sub" / "b.txt", "world");

			DirectoryProvider provider(root);
			CHECK(provider.SourceType() == Source::Directory);

			std::vector<uint8_t> bytes;
			CHECK(provider.Open("a.txt", bytes, ec));
			CHECK(AsString(bytes) == "hello");

			StatInfo stat;
			CHECK(provider.Stat("a.txt", stat, ec));
			CHECK(stat.size == 5 && !stat.isDirectory);
			CHECK(provider.Stat("sub", stat, ec));
			CHECK(stat.isDirectory);
			CHECK(provider.Open("sub/b.txt", bytes, ec));
			CHECK(AsString(bytes) == "world");

			CHECK(!provider.Open("missing.txt", bytes, ec));
			CHECK(static_cast<bool>(ec));
			CHECK(!provider.Open("sub", bytes, ec));          // 目录不能当文件打开
			CHECK(!provider.Open("../a.txt", bytes, ec));     // 路径逃逸拒绝
		}

		// 3. 挂载语义:优先级 / 同优先级 LIFO / Unmount 回退 / 重复 id
		{
			const std::filesystem::path dirA = temp.path / "mountA";
			const std::filesystem::path dirB = temp.path / "mountB";
			WriteBytes(dirA / "f.txt", "A");
			WriteBytes(dirB / "f.txt", "B");

			{
				Vfs vfs;
				std::error_code ec;
				CHECK(vfs.Mount("low", std::make_shared<DirectoryProvider>(dirA), 10) != 0);
				CHECK(vfs.Mount("high", std::make_shared<DirectoryProvider>(dirB), 20) != 0);
				std::vector<uint8_t> bytes;
				CHECK(vfs.Read("f.txt", bytes, ec));
				CHECK(AsString(bytes) == "B");
			}

			{
				Vfs vfs;
				std::error_code ec;
				const MountId idA = vfs.Mount("a", std::make_shared<DirectoryProvider>(dirA), 5);
				const MountId idB = vfs.Mount("b", std::make_shared<DirectoryProvider>(dirB), 5);
				CHECK(idA != 0 && idB != 0 && idA != idB);

				std::vector<uint8_t> bytes;
				CHECK(vfs.Read("f.txt", bytes, ec));
				CHECK(AsString(bytes) == "B");   // 同优先级 LIFO:后挂载者先命中

				CHECK(vfs.Unmount(idB));
				CHECK(vfs.Read("f.txt", bytes, ec));
				CHECK(AsString(bytes) == "A");   // Unmount 后回退到下一层
				CHECK(!vfs.Unmount(idB));       // 已卸载再卸载失败
				CHECK(vfs.MountCount() == 1);

				CHECK(vfs.Mount("a", std::make_shared<DirectoryProvider>(dirB), 5) == 0);  // 重复 id
				CHECK(vfs.MountCount() == 1);

				StatInfo stat;
				CHECK(vfs.Resolve("f.txt", stat, ec));
				CHECK(stat.source == Source::Directory && stat.mountId == "a" && stat.size == 1);
				CHECK(vfs.Exists("f.txt"));
				CHECK(!vfs.Exists("nope.txt"));

				vfs.Clear();
				CHECK(vfs.MountCount() == 0);
				CHECK(!vfs.Read("f.txt", bytes, ec));
				CHECK(static_cast<bool>(ec));
			}
		}

		// 4. 包 BuildFromDirectory -> Open -> Read 往返与双形态一致
		{
			std::error_code ec;
			const std::filesystem::path srcDir = temp.path / "pkgsrc";
			WriteBytes(srcDir / "a.txt", "hello-a");
			WriteBytes(srcDir / "sub" / "b.txt", "hello-b");
			WriteBytes(srcDir / "empty.txt", "");

			const std::filesystem::path pakPath = temp.path / "roundtrip.wpak";
			CHECK(PackageProvider::BuildFromDirectory(srcDir, pakPath, ec));
			CHECK(!static_cast<bool>(ec));

			std::shared_ptr<PackageProvider> provider = PackageProvider::Open(pakPath, ec);
			CHECK(provider != nullptr);
			CHECK(!static_cast<bool>(ec));
			CHECK(provider->SourceType() == Source::Package);

			std::vector<uint8_t> bytes;
			CHECK(provider->Open("a.txt", bytes, ec));
			CHECK(AsString(bytes) == "hello-a");
			StatInfo stat;
			CHECK(provider->Stat("a.txt", stat, ec));
			CHECK(stat.size == 7 && !stat.isDirectory);
			CHECK(provider->Open("sub/b.txt", bytes, ec));
			CHECK(AsString(bytes) == "hello-b");
			CHECK(provider->Open("empty.txt", bytes, ec));
			CHECK(bytes.empty());
			CHECK(!provider->Open("missing.txt", bytes, ec));
			CHECK(static_cast<bool>(ec));

			Vfs dirVfs;
			Vfs pakVfs;
			CHECK(dirVfs.Mount("dir", std::make_shared<DirectoryProvider>(srcDir), 1) != 0);
			CHECK(pakVfs.Mount("pak", std::move(provider), 1) != 0);
			for (const char* name : { "a.txt", "sub/b.txt", "empty.txt" })
			{
				std::vector<uint8_t> fromDir;
				std::vector<uint8_t> fromPak;
				std::error_code ecDir;
				std::error_code ecPak;
				CHECK(dirVfs.Read(name, fromDir, ecDir));
				CHECK(pakVfs.Read(name, fromPak, ecPak));
				CHECK(fromDir == fromPak);

				StatInfo statDir;
				StatInfo statPak;
				CHECK(dirVfs.Stat(name, statDir, ecDir));
				CHECK(pakVfs.Stat(name, statPak, ecPak));
				CHECK(statDir.source == Source::Directory && statPak.source == Source::Package);
				CHECK(statDir.size == statPak.size);
			}
		}

		// 5. 篡改 blob 一个字节 → 挂载成功、该条目读取 CRC 失败;索引被篡改 → 挂载失败
		{
			std::error_code ec;
			const std::filesystem::path srcDir = temp.path / "tampersrc";
			WriteBytes(srcDir / "x.bin", std::string(64, 'x'));
			const std::filesystem::path pakPath = temp.path / "tamper.wpak";
			CHECK(PackageProvider::BuildFromDirectory(srcDir, pakPath, ec));

			std::vector<uint8_t> bytes = ReadBytes(pakPath);
			bytes.back() ^= 0xFFu;   // 最后一个 blob 的最后一个字节
			WriteBytes(pakPath, bytes);

			std::shared_ptr<PackageProvider> provider = PackageProvider::Open(pakPath, ec);
			CHECK(provider != nullptr);   // 索引完好,挂载成功
			std::vector<uint8_t> data;
			CHECK(!provider->Open("x.bin", data, ec));
			CHECK(static_cast<bool>(ec));

			const std::filesystem::path pakPath2 = temp.path / "tamper-index.wpak";
			CHECK(PackageProvider::BuildFromDirectory(srcDir, pakPath2, ec));
			bytes = ReadBytes(pakPath2);
			const uint32_t jsonLen = ReadJsonLen(bytes);
			bytes[16 + jsonLen / 2] ^= 0xFFu;   // 索引 JSON 内部
			WriteBytes(pakPath2, bytes);
			CHECK(PackageProvider::Open(pakPath2, ec) == nullptr);
			CHECK(static_cast<bool>(ec));
		}

		// 6. 越界 offset/size、含 ".." 的索引条目、旧魔数、不存在文件
		{
			std::error_code ec;
			const std::filesystem::path badBounds = temp.path / "bad-bounds.wpak";
			WriteSyntheticPak(badBounds, 2, { { "a.txt", { 0, 1 } } });  // offset < dataStart
			CHECK(PackageProvider::Open(badBounds, ec) == nullptr);
			CHECK(static_cast<bool>(ec));

			const std::filesystem::path badSize = temp.path / "bad-size.wpak";
			// offset 远超文件大小,size 亦越界。
			WriteSyntheticPak(badSize, 2, { { "a.txt", { 0xFFFFFFFFFFFFFFFFull, 1 } } });
			CHECK(PackageProvider::Open(badSize, ec) == nullptr);
			CHECK(static_cast<bool>(ec));

			const std::filesystem::path badDotdot = temp.path / "bad-dotdot.wpak";
			WriteSyntheticPak(badDotdot, 2, { { "../evil", { 100, 1 } } });
			CHECK(PackageProvider::Open(badDotdot, ec) == nullptr);
			CHECK(static_cast<bool>(ec));

			const std::filesystem::path legacy = temp.path / "legacy.wpak";
			WriteSyntheticPak(legacy, 1, {});   // v1 旧格式
			CHECK(PackageProvider::Open(legacy, ec) == nullptr);
			CHECK(ec == std::errc::not_supported);

			CHECK(PackageProvider::Open(temp.path / "no-such.wpak", ec) == nullptr);
			CHECK(static_cast<bool>(ec));
		}

		std::printf("World.Vfs: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Vfs: FAILED: %s\n", error.what());
		return 1;
	}
}

#include "wldpch.h"
#include "World/Core/Asset/WModelIO.h"

#include "World/Core/Application.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

namespace World::Asset::WModelIO
{
	namespace
	{
		constexpr char kMagic[4] = { 'W', 'M', 'D', 'L' };
		// magic(4) + version/flags/vertexLayoutId/reserved + 6 个计数。
		constexpr uint64_t kHeaderSize = 4u + 10u * 4u;

		void AppendU32(std::vector<uint8_t>& out, uint32_t value)
		{
			out.push_back(static_cast<uint8_t>(value & 0xFFu));
			out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
			out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
			out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
		}

		void AppendI32(std::vector<uint8_t>& out, int32_t value)
		{
			AppendU32(out, static_cast<uint32_t>(value));
		}

		void AppendF32(std::vector<uint8_t>& out, float value)
		{
			uint32_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			AppendU32(out, bits);
		}

		void AppendVec3(std::vector<uint8_t>& out, const glm::vec3& value)
		{
			AppendF32(out, value.x);
			AppendF32(out, value.y);
			AppendF32(out, value.z);
		}

		void AppendString(std::vector<uint8_t>& out, const std::string& value)
		{
			AppendU32(out, static_cast<uint32_t>(value.size()));
			out.insert(out.end(), value.begin(), value.end());
		}

		// 顺序读取器:所有越界都变成 "truncated" 可读错误(绝不猜测/回退)。
		struct Reader
		{
			const uint8_t* Data = nullptr;
			uint64_t Size = 0;
			uint64_t Offset = 0;
			std::string Error;

			bool Failed() const { return !Error.empty(); }

			bool Ensure(uint64_t count, const char* what)
			{
				if (Failed())
					return false;
				if (count > Size - Offset)
				{
					Error = std::string("truncated while reading ") + what;
					return false;
				}
				return true;
			}

			bool ReadRaw(void* destination, uint64_t count, const char* what)
			{
				if (!Ensure(count, what))
					return false;
				std::memcpy(destination, Data + Offset, static_cast<size_t>(count));
				Offset += count;
				return true;
			}

			bool ReadU32(uint32_t& out, const char* what)
			{
				uint8_t bytes[4] = { 0, 0, 0, 0 };
				if (!ReadRaw(bytes, sizeof(bytes), what))
					return false;
				out = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8)
					| (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
				return true;
			}

			bool ReadI32(int32_t& out, const char* what)
			{
				uint32_t raw = 0;
				if (!ReadU32(raw, what))
					return false;
				out = static_cast<int32_t>(raw);
				return true;
			}

			bool ReadF32(float& out, const char* what)
			{
				uint32_t raw = 0;
				if (!ReadU32(raw, what))
					return false;
				std::memcpy(&out, &raw, sizeof(out));
				return true;
			}

			bool ReadVec3(glm::vec3& out, const char* what)
			{
				return ReadF32(out.x, what) && ReadF32(out.y, what) && ReadF32(out.z, what);
			}

			bool ReadString(std::string& out, uint32_t length, const char* what)
			{
				if (!Ensure(length, what))
					return false;
				out.assign(reinterpret_cast<const char*>(Data + Offset), length);
				Offset += length;
				return true;
			}
		};

		bool ParseImpl(const uint8_t* bytes, size_t size, WModelData& out, std::string& error)
		{
			out = WModelData {};
			if (bytes == nullptr || size < kHeaderSize)
			{
				error = "truncated: file is smaller than the .wmodel v1 header";
				return false;
			}

			Reader reader { bytes, static_cast<uint64_t>(size), 0, {} };

			char magic[4] = { 0, 0, 0, 0 };
			reader.ReadRaw(magic, sizeof(magic), "magic");
			if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
			{
				error = "bad magic: expected 'WMDL'";
				return false;
			}

			uint32_t version = 0;
			reader.ReadU32(version, "version");
			if (version != kFormatVersion)
			{
				error = "unsupported .wmodel version " + std::to_string(version)
					+ " (this build reads version " + std::to_string(kFormatVersion) + ")";
				return false;
			}

			uint32_t flags = 0;
			uint32_t vertexLayoutId = 0;
			uint32_t reserved = 0;
			reader.ReadU32(flags, "flags");
			reader.ReadU32(vertexLayoutId, "vertexLayoutId");
			reader.ReadU32(reserved, "reserved");
			(void)reserved;
			if (vertexLayoutId != kVertexLayoutStandard)
			{
				error = "unsupported vertex layout id " + std::to_string(vertexLayoutId)
					+ " (expected " + std::to_string(kVertexLayoutStandard) + ": position/normal/uv)";
				return false;
			}

			uint32_t vertexCount = 0;
			uint32_t indexCount = 0;
			uint32_t meshCount = 0;
			uint32_t submeshCount = 0;
			uint32_t nodeCount = 0;
			uint32_t materialSlotCount = 0;
			reader.ReadU32(vertexCount, "vertexCount");
			reader.ReadU32(indexCount, "indexCount");
			reader.ReadU32(meshCount, "meshCount");
			reader.ReadU32(submeshCount, "submeshCount");
			reader.ReadU32(nodeCount, "nodeCount");
			reader.ReadU32(materialSlotCount, "materialSlotCount");
			if (reader.Failed())
			{
				error = reader.Error;
				return false;
			}

			// 计数保护:声明的顶点/索引数据必须能在文件总大小内放下(先校验再分配)。
			if (static_cast<uint64_t>(vertexCount) * sizeof(WModelVertex) > size)
			{
				error = "invalid vertexCount " + std::to_string(vertexCount) + ": larger than the file";
				return false;
			}
			if (static_cast<uint64_t>(indexCount) * sizeof(uint32_t) > size)
			{
				error = "invalid indexCount " + std::to_string(indexCount) + ": larger than the file";
				return false;
			}
			if (indexCount % 3u != 0u)
			{
				error = "invalid indexCount " + std::to_string(indexCount) + ": not a multiple of 3 (triangles only)";
				return false;
			}

			out.Flags = flags;

			// Bounds
			if (!reader.ReadVec3(out.Bounds.Min, "bounds.min") || !reader.ReadVec3(out.Bounds.Max, "bounds.max"))
			{
				error = reader.Error;
				return false;
			}

			// Submeshes[]
			out.Submeshes.resize(submeshCount);
			for (uint32_t index = 0; index < submeshCount; ++index)
			{
				WModelSubmesh& submesh = out.Submeshes[index];
				const std::string label = "submesh " + std::to_string(index);
				if (!reader.ReadU32(submesh.IndexOffset, (label + ".indexOffset").c_str())
					|| !reader.ReadU32(submesh.IndexCount, (label + ".indexCount").c_str())
					|| !reader.ReadI32(submesh.MaterialSlot, (label + ".materialSlot").c_str())
					|| !reader.ReadVec3(submesh.Bounds.Min, (label + ".bounds.min").c_str())
					|| !reader.ReadVec3(submesh.Bounds.Max, (label + ".bounds.max").c_str()))
				{
					error = reader.Error;
					return false;
				}
				if (static_cast<uint64_t>(submesh.IndexOffset) + submesh.IndexCount > indexCount)
				{
					error = "submesh " + std::to_string(index) + " index range ["
						+ std::to_string(submesh.IndexOffset) + ", "
						+ std::to_string(static_cast<uint64_t>(submesh.IndexOffset) + submesh.IndexCount)
						+ ") is out of bounds (indexCount = " + std::to_string(indexCount) + ")";
					return false;
				}
				const int64_t slot = submesh.MaterialSlot;
				if (slot < -1 || slot >= static_cast<int64_t>(materialSlotCount))
				{
					error = "submesh " + std::to_string(index) + " references unknown material slot "
						+ std::to_string(slot) + " (materialSlotCount = " + std::to_string(materialSlotCount) + ")";
					return false;
				}
			}

			// Meshes[]
			out.Meshes.resize(meshCount);
			for (uint32_t index = 0; index < meshCount; ++index)
			{
				WModelMeshRange& range = out.Meshes[index];
				const std::string label = "mesh " + std::to_string(index);
				if (!reader.ReadU32(range.FirstSubmesh, (label + ".firstSubmesh").c_str())
					|| !reader.ReadU32(range.SubmeshCount, (label + ".submeshCount").c_str()))
				{
					error = reader.Error;
					return false;
				}
				if (static_cast<uint64_t>(range.FirstSubmesh) + range.SubmeshCount > submeshCount)
				{
					error = "mesh " + std::to_string(index) + " submesh range is out of bounds (submeshCount = "
						+ std::to_string(submeshCount) + ")";
					return false;
				}
			}

			// Nodes[]
			out.Nodes.resize(nodeCount);
			for (uint32_t index = 0; index < nodeCount; ++index)
			{
				WModelNode& node = out.Nodes[index];
				const std::string label = "node " + std::to_string(index);
				uint32_t nameLength = 0;
				if (!reader.ReadI32(node.Parent, (label + ".parent").c_str())
					|| !reader.ReadI32(node.MeshIndex, (label + ".meshIndex").c_str())
					|| !reader.ReadVec3(node.Translation, (label + ".translation").c_str()))
				{
					error = reader.Error;
					return false;
				}
				float rotation[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
				if (!reader.ReadF32(rotation[0], (label + ".rotation.x").c_str())
					|| !reader.ReadF32(rotation[1], (label + ".rotation.y").c_str())
					|| !reader.ReadF32(rotation[2], (label + ".rotation.z").c_str())
					|| !reader.ReadF32(rotation[3], (label + ".rotation.w").c_str())
					|| !reader.ReadVec3(node.Scale, (label + ".scale").c_str())
					|| !reader.ReadU32(nameLength, (label + ".nameLength").c_str())
					|| !reader.ReadString(node.Name, nameLength, (label + ".name").c_str()))
				{
					error = reader.Error;
					return false;
				}
				node.Rotation = glm::quat(rotation[3], rotation[0], rotation[1], rotation[2]);
				if (node.Parent < -1 || node.Parent >= static_cast<int32_t>(nodeCount))
				{
					error = "node " + std::to_string(index) + " has out-of-range parent " + std::to_string(node.Parent);
					return false;
				}
				if (node.MeshIndex < -1 || node.MeshIndex >= static_cast<int32_t>(meshCount))
				{
					error = "node " + std::to_string(index) + " has out-of-range meshIndex "
						+ std::to_string(node.MeshIndex);
					return false;
				}
			}

			// 节点环检测:父子链必须能在 nodeCount 步内走到根(-1),否则实例化会成环。
			for (uint32_t index = 0; index < nodeCount; ++index)
			{
				int32_t current = static_cast<int32_t>(index);
				uint32_t steps = 0;
				while (current != -1)
				{
					if (++steps > nodeCount)
					{
						error = "node hierarchy contains a cycle (starting at node " + std::to_string(index) + ")";
						return false;
					}
					current = out.Nodes[static_cast<size_t>(current)].Parent;
				}
			}

			// MaterialSlots[]
			out.MaterialSlots.resize(materialSlotCount);
			for (uint32_t index = 0; index < materialSlotCount; ++index)
			{
				uint32_t pathLength = 0;
				const std::string label = "material slot " + std::to_string(index);
				if (!reader.ReadU32(pathLength, (label + " pathLength").c_str())
					|| !reader.ReadString(out.MaterialSlots[index], pathLength, (label + " path").c_str()))
				{
					error = reader.Error;
					return false;
				}
			}

			// VertexData / Indices
			out.Vertices.resize(vertexCount);
			if (vertexCount > 0
				&& !reader.ReadRaw(out.Vertices.data(), static_cast<uint64_t>(vertexCount) * sizeof(WModelVertex),
					"vertex data"))
			{
				error = reader.Error;
				return false;
			}
			out.Indices.resize(indexCount);
			if (indexCount > 0
				&& !reader.ReadRaw(out.Indices.data(), static_cast<uint64_t>(indexCount) * sizeof(uint32_t),
					"index data"))
			{
				error = reader.Error;
				return false;
			}

			if (reader.Offset != reader.Size)
			{
				error = "trailing bytes after .wmodel payload (" + std::to_string(reader.Size - reader.Offset)
					+ " unexpected byte(s))";
				return false;
			}
			return true;
		}
	}

	std::vector<uint8_t> Serialize(const WModelData& data)
	{
		std::vector<uint8_t> out;
		out.reserve(static_cast<size_t>(kHeaderSize)
			+ data.Vertices.size() * sizeof(WModelVertex)
			+ data.Indices.size() * sizeof(uint32_t)
			+ data.Submeshes.size() * (3u * 4u + 6u * 4u)
			+ data.Meshes.size() * 8u
			+ data.Nodes.size() * (2u * 4u + 10u * 4u + 4u)
			+ data.MaterialSlots.size() * 4u);

		out.insert(out.end(), kMagic, kMagic + sizeof(kMagic));
		AppendU32(out, kFormatVersion);
		AppendU32(out, data.Flags);
		AppendU32(out, kVertexLayoutStandard);
		AppendU32(out, 0u);   // reserved
		AppendU32(out, static_cast<uint32_t>(data.Vertices.size()));
		AppendU32(out, static_cast<uint32_t>(data.Indices.size()));
		AppendU32(out, static_cast<uint32_t>(data.Meshes.size()));
		AppendU32(out, static_cast<uint32_t>(data.Submeshes.size()));
		AppendU32(out, static_cast<uint32_t>(data.Nodes.size()));
		AppendU32(out, static_cast<uint32_t>(data.MaterialSlots.size()));

		AppendVec3(out, data.Bounds.Min);
		AppendVec3(out, data.Bounds.Max);

		for (const WModelSubmesh& submesh : data.Submeshes)
		{
			AppendU32(out, submesh.IndexOffset);
			AppendU32(out, submesh.IndexCount);
			AppendI32(out, submesh.MaterialSlot);
			AppendVec3(out, submesh.Bounds.Min);
			AppendVec3(out, submesh.Bounds.Max);
		}
		for (const WModelMeshRange& range : data.Meshes)
		{
			AppendU32(out, range.FirstSubmesh);
			AppendU32(out, range.SubmeshCount);
		}
		for (const WModelNode& node : data.Nodes)
		{
			AppendI32(out, node.Parent);
			AppendI32(out, node.MeshIndex);
			AppendVec3(out, node.Translation);
			AppendF32(out, node.Rotation.x);
			AppendF32(out, node.Rotation.y);
			AppendF32(out, node.Rotation.z);
			AppendF32(out, node.Rotation.w);
			AppendVec3(out, node.Scale);
			AppendString(out, node.Name);
		}
		for (const std::string& slot : data.MaterialSlots)
			AppendString(out, slot);

		if (!data.Vertices.empty())
		{
			const auto* raw = reinterpret_cast<const uint8_t*>(data.Vertices.data());
			out.insert(out.end(), raw, raw + data.Vertices.size() * sizeof(WModelVertex));
		}
		if (!data.Indices.empty())
		{
			const auto* raw = reinterpret_cast<const uint8_t*>(data.Indices.data());
			out.insert(out.end(), raw, raw + data.Indices.size() * sizeof(uint32_t));
		}
		return out;
	}

	bool Parse(const uint8_t* bytes, size_t size, WModelData& out, std::string* error)
	{
		std::string localError;
		if (ParseImpl(bytes, size, out, localError))
		{
			if (error) error->clear();
			return true;
		}
		if (error) *error = localError;
		return false;
	}

	bool WriteFile(const std::string& path, const WModelData& data, std::string* error)
	{
		if (path.empty())
		{
			if (error) *error = "path is empty";
			return false;
		}
		const std::vector<uint8_t> bytes = Serialize(data);
		if (error) error->clear();
		// 自校验:能写出的文件必须能被读回(越界引用等在导入期失败,而不是等到运行期)。
		WModelData verified;
		std::string parseError;
		if (!Parse(bytes.data(), bytes.size(), verified, &parseError))
		{
			if (error) *error = "refusing to write invalid .wmodel: " + parseError;
			return false;
		}

		std::error_code ec;
		const std::filesystem::path target(path);
		if (!target.parent_path().empty())
			std::filesystem::create_directories(target.parent_path(), ec);
		const std::filesystem::path temporary = target.string() + ".tmp";
		{
			std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
			if (!file)
			{
				if (error) *error = "cannot write " + temporary.string();
				return false;
			}
			file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if (!file)
			{
				if (error) *error = "write failed: " + temporary.string();
				return false;
			}
		}
		std::filesystem::rename(temporary, target, ec);
		if (ec)
		{
			// Windows 上目标已存在时 rename 会失败:先删再换。
			std::filesystem::remove(target, ec);
			ec.clear();
			std::filesystem::rename(temporary, target, ec);
		}
		if (ec)
		{
			if (error) *error = "cannot replace " + target.string() + ": " + ec.message();
			return false;
		}
		return true;
	}

	bool ReadFile(const std::string& path, WModelData& out, std::string* error)
	{
		// 路径解析必须与材质/脚本同一条口径:**VFS 优先、磁盘回退**。
		// 网格引用是"相对内容根"的逻辑路径(如 models/x.wmodel),而编辑器/Runtime 的 CWD 都不是内容根;
		// 直接 ifstream(逻辑路径) 只会在 CWD 恰好是内容根时成功(实测:编辑器里"网格加载失败,
		// 回退到 Primitive";打包后 Runtime 更是必须走 VFS)。
		std::vector<uint8_t> bytes;
		if (path.empty())
		{
			if (error) *error = "path is empty";
			return false;
		}
		if (Application::HasInstance())
		{
			std::error_code vfsError;
			if (Application::Get().GetContext().Vfs().Read(path, bytes, vfsError) && !bytes.empty())
			{
				if (error) error->clear();
				return Parse(bytes.data(), bytes.size(), out, error);
			}
		}
		// 磁盘回退:内容根 Game/assets → Game/ → Editor/(与 MaterialIO::ReadFileText 同序)。
		const std::filesystem::path candidates[] = {
			std::filesystem::path(std::string(WLD_GAME_DIR)) / "assets" / path,
			std::filesystem::path(std::string(WLD_GAME_DIR)) / path,
			std::filesystem::path(std::string(WLD_EDITOR_DIR)) / path,
		};
		for (const std::filesystem::path& candidate : candidates)
		{
			std::ifstream file(candidate, std::ios::binary);
			if (!file)
				continue;
			bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			if (bytes.empty())
				continue;
			if (error) error->clear();
			return Parse(bytes.data(), bytes.size(), out, error);
		}
		if (error) *error = "cannot open '" + path + "'";
		return false;
	}
}

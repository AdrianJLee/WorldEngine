#include "wldpch.h"
#include "World/Core/Asset/WModelIO.h"

#include "World/Core/Application.h"

#include <cmath>
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
		// magic(4) + version/flags/vertexLayoutId/reserved + 8 个计数(顶点/索引/mesh/submesh/node/
		// materialSlot/jointCount(= Skins[] 数组长度)/animation)。
		constexpr uint64_t kHeaderSize = 4u + 12u * 4u;
		// meta: sourceFingerprint(u64) + importerVersion(u32) + settingsHash(u64) +
		//       upAxis(u8) + scale(f32) + reserved(u32);全部显式小端字节,无 C++ 结构体填充。
		constexpr uint64_t kMetaSize = 8u + 4u + 8u + 1u + 4u + 4u;
		// 各区块的"最小字节数":计数保护用(声明计数大于文件本身 → 先拒绝再分配)。
		constexpr uint64_t kMinSkinBytes = 4u + 4u;            // nameLength + jointCount(关节可为空)
		constexpr uint64_t kMinAnimationBytes = 4u + 4u + 4u;  // nameLength + duration + channelCount
		constexpr uint64_t kMinChannelBytes = 4u + 1u + 4u;    // targetNode + path + keyCount
		constexpr uint64_t kMinKeyBytes = 4u + 16u;            // time + value(vec4)

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

		void AppendU8(std::vector<uint8_t>& out, uint8_t value)
		{
			out.push_back(value);
		}

		void AppendU64(std::vector<uint8_t>& out, uint64_t value)
		{
			AppendU32(out, static_cast<uint32_t>(value & 0xFFFFFFFFu));
			AppendU32(out, static_cast<uint32_t>((value >> 32) & 0xFFFFFFFFu));
		}

		void AppendVec3(std::vector<uint8_t>& out, const glm::vec3& value)
		{
			AppendF32(out, value.x);
			AppendF32(out, value.y);
			AppendF32(out, value.z);
		}

		void AppendVec4(std::vector<uint8_t>& out, const glm::vec4& value)
		{
			AppendF32(out, value.x);
			AppendF32(out, value.y);
			AppendF32(out, value.z);
			AppendF32(out, value.w);
		}

		void AppendQuat(std::vector<uint8_t>& out, const glm::quat& value)
		{
			// 与节点旋转同序(x,y,z,w)。
			AppendF32(out, value.x);
			AppendF32(out, value.y);
			AppendF32(out, value.z);
			AppendF32(out, value.w);
		}

		void AppendMat4(std::vector<uint8_t>& out, const glm::mat4& value)
		{
			for (int column = 0; column < 4; ++column)
				for (int row = 0; row < 4; ++row)
					AppendF32(out, value[column][row]);   // glm 是列主序,写盘同序
		}

		void AppendString(std::vector<uint8_t>& out, const std::string& value)
		{
			AppendU32(out, static_cast<uint32_t>(value.size()));
			out.insert(out.end(), value.begin(), value.end());
		}

		bool IsFiniteVec3(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool IsFiniteVec4(const glm::vec4& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z)
				&& std::isfinite(value.w);
		}

		bool IsFiniteQuat(const glm::quat& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z)
				&& std::isfinite(value.w);
		}

		bool IsFiniteMat4(const glm::mat4& value)
		{
			for (int column = 0; column < 4; ++column)
				for (int row = 0; row < 4; ++row)
					if (!std::isfinite(value[column][row]))
						return false;
			return true;
		}

		// 顺序读取器:所有越界都变成 "truncated" 可读错误(绝不猜测/回退)。
		struct Reader
		{
			const uint8_t* Data = nullptr;
			uint64_t Size = 0;
			uint64_t Offset = 0;
			std::string Error;

			bool Failed() const { return !Error.empty(); }
			uint64_t Remaining() const { return Size - Offset; }

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

			bool ReadU64(uint64_t& out, const char* what)
			{
				uint32_t low = 0;
				uint32_t high = 0;
				if (!ReadU32(low, what) || !ReadU32(high, what))
					return false;
				out = static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32);
				return true;
			}

			bool ReadU8(uint8_t& out, const char* what)
			{
				if (!Ensure(1, what))
					return false;
				out = Data[Offset];
				++Offset;
				return true;
			}

			bool ReadVec3(glm::vec3& out, const char* what)
			{
				return ReadF32(out.x, what) && ReadF32(out.y, what) && ReadF32(out.z, what);
			}

			bool ReadVec4(glm::vec4& out, const char* what)
			{
				return ReadF32(out.x, what) && ReadF32(out.y, what) && ReadF32(out.z, what)
					&& ReadF32(out.w, what);
			}

			bool ReadQuat(glm::quat& out, const char* what)
			{
				float x = 0.0f;
				float y = 0.0f;
				float z = 0.0f;
				float w = 1.0f;
				if (!ReadF32(x, what) || !ReadF32(y, what) || !ReadF32(z, what) || !ReadF32(w, what))
					return false;
				out = glm::quat(w, x, y, z);
				return true;
			}

			bool ReadMat4(glm::mat4& out, const char* what)
			{
				glm::mat4 value(1.0f);
				for (int column = 0; column < 4; ++column)
					for (int row = 0; row < 4; ++row)
						if (!ReadF32(value[column][row], what))
							return false;
				out = value;
				return true;
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

		// v5 meta 区块(header 之后):源身份 + 完整导入设置。ParseImpl 与 ParseMetaOnly 共用。
		// 返回 false 时 error 已填好可读原因。
		bool ReadMetaBlock(Reader& reader, WModelData::MetaData& meta, std::string& error)
		{
			meta.Valid = true;
			uint32_t metaReserved = 0;
			uint32_t sourcePathLength = 0;
			if (!reader.ReadU64(meta.SourceFingerprint, "meta.sourceFingerprint")
				|| !reader.ReadU32(meta.ImporterVersion, "meta.importerVersion")
				|| !reader.ReadU64(meta.SettingsHash, "meta.settingsHash")
				|| !reader.ReadU8(meta.UpAxis, "meta.upAxis")
				|| !reader.ReadF32(meta.Scale, "meta.scale")
				|| !reader.ReadU32(metaReserved, "meta.reserved")
				|| !reader.ReadU32(sourcePathLength, "meta.sourcePathLength")
				|| !reader.ReadString(meta.SourcePath, sourcePathLength, "meta.sourcePath"))
			{
				error = reader.Error;
				return false;
			}
			(void)metaReserved;
			if (meta.UpAxis > 1u)
			{
				error = "invalid meta.upAxis " + std::to_string(meta.UpAxis) + " (expected 0 or 1)";
				return false;
			}
			if (!std::isfinite(meta.Scale) || meta.Scale <= 0.0f)
			{
				error = "invalid meta.scale " + std::to_string(meta.Scale)
					+ " (expected a positive finite number)";
				return false;
			}
			// v5:完整导入设置(P4-U11)。缺省 = 外来产物;导入器自己写的资产一定有。
			uint8_t hasSettings = 0;
			if (!reader.ReadU8(hasSettings, "meta.hasSettings"))
			{
				error = reader.Error;
				return false;
			}
			if (hasSettings > 1u)
			{
				error = "invalid meta.hasSettings " + std::to_string(hasSettings) + " (expected 0 or 1)";
				return false;
			}
			if (hasSettings == 0u)
				return true;
			meta.HasSettings = true;
			ModelImportSettings& settings = meta.Settings;
			uint8_t flags[6] = {};
			uint8_t reuseTextures = 0;
			uint32_t sharedFolderLength = 0;
			if (!reader.ReadF32(settings.Scale, "meta.settings.scale")
				|| !reader.ReadU8(settings.UpAxis, "meta.settings.upAxis")
				|| !reader.ReadU8(flags[0], "meta.settings.exportMaterials")
				|| !reader.ReadU8(flags[1], "meta.settings.exportTextures")
				|| !reader.ReadU8(flags[2], "meta.settings.importAnimations")
				|| !reader.ReadU8(flags[3], "meta.settings.importSkins")
				|| !reader.ReadF32(settings.AnimationSampleRate, "meta.settings.animationSampleRate")
				|| !reader.ReadU8(flags[4], "meta.settings.generateNormals")
				|| !reader.ReadU8(flags[5], "meta.settings.reuseMaterials")
				|| !reader.ReadU8(reuseTextures, "meta.settings.reuseTextures")
				|| !reader.ReadU32(sharedFolderLength, "meta.settings.sharedFolderLength")
				|| !reader.ReadString(settings.SharedMaterialFolder, sharedFolderLength,
					"meta.settings.sharedMaterialFolder"))
			{
				error = reader.Error;
				return false;
			}
			settings.ExportMaterials = flags[0] != 0;
			settings.ExportTextures = flags[1] != 0;
			settings.ImportAnimations = flags[2] != 0;
			settings.ImportSkins = flags[3] != 0;
			settings.GenerateNormals = flags[4] != 0;
			settings.ReuseMaterials = flags[5] != 0;
			settings.ReuseTextures = reuseTextures != 0;
			if (settings.UpAxis > 1u)
			{
				error = "invalid meta.settings.upAxis " + std::to_string(settings.UpAxis) + " (expected 0 or 1)";
				return false;
			}
			if (!std::isfinite(settings.Scale) || settings.Scale <= 0.0f)
			{
				error = "invalid meta.settings.scale (expected a positive finite number)";
				return false;
			}
			if (!std::isfinite(settings.AnimationSampleRate) || settings.AnimationSampleRate <= 0.0f)
			{
				error = "invalid meta.settings.animationSampleRate (expected a positive finite number)";
				return false;
			}
			return true;
		}

		bool ParseImpl(const uint8_t* bytes, size_t size, WModelData& out, std::string& error)
		{
			out = WModelData {};
			if (bytes == nullptr || size < kHeaderSize)
			{
				error = "truncated: file is smaller than the .wmodel v5 header";
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
			if (version >= 1u && version <= 3u)
			{
				// v1–v3 没有 v4 的 skin/animations 区块 —— 一律拒绝,不做"尽力解析"。
				error = ".wmodel version " + std::to_string(version)
					+ " is no longer supported (v4 adds skin/animation blocks); "
					"please re-import the source asset (请重新导入)";
				return false;
			}
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
			if (GetVertexStride(vertexLayoutId) == 0u)
			{
				error = "unsupported vertex layout id " + std::to_string(vertexLayoutId)
					+ " (expected " + std::to_string(kVertexLayoutStandard) + " = position/normal/uv or "
					+ std::to_string(kVertexLayoutSkinned) + " = standard + joints/weights)";
				return false;
			}

			uint32_t vertexCount = 0;
			uint32_t indexCount = 0;
			uint32_t meshCount = 0;
			uint32_t submeshCount = 0;
			uint32_t nodeCount = 0;
			uint32_t materialSlotCount = 0;
			// 注意 wire 名:header 的 jointCount 字段 = **Skins[] 数组长度**(每个 skin 自己的关节数
			// 在 skin 区块内,上限 kMaxJointsPerSkin)。
			uint32_t skinCount = 0;
			uint32_t animationCount = 0;
			reader.ReadU32(vertexCount, "vertexCount");
			reader.ReadU32(indexCount, "indexCount");
			reader.ReadU32(meshCount, "meshCount");
			reader.ReadU32(submeshCount, "submeshCount");
			reader.ReadU32(nodeCount, "nodeCount");
			reader.ReadU32(materialSlotCount, "materialSlotCount");
			reader.ReadU32(skinCount, "jointCount(skin count)");
			reader.ReadU32(animationCount, "animationCount");
			if (reader.Failed())
			{
				error = reader.Error;
				return false;
			}
			if (size < kHeaderSize + kMetaSize)
			{
				error = "truncated: file is smaller than the .wmodel v5 meta block";
				return false;
			}

			// 计数保护:声明的顶点/索引数据必须能在文件总大小内放下(先校验再分配)。
			if (static_cast<uint64_t>(vertexCount) * GetVertexStride(vertexLayoutId) > size)
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
			out.VertexLayoutId = vertexLayoutId;

			// Meta(v5:含完整导入设置)
			if (!ReadMetaBlock(reader, out.Meta, error))
				return false;

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
					|| !reader.ReadU32(range.SubmeshCount, (label + ".submeshCount").c_str())
					|| !reader.ReadI32(range.SkinIndex, (label + ".skinIndex").c_str()))
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
				const int64_t skinIndex = range.SkinIndex;
				if (skinIndex < -1 || skinIndex >= static_cast<int64_t>(skinCount))
				{
					error = "mesh " + std::to_string(index) + " references unknown skin "
						+ std::to_string(skinIndex) + " (-1 = static, Skins[] length = "
						+ std::to_string(skinCount) + ")";
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

			// Skins[](header 的 jointCount 字段 = 本数组长度)
			if (static_cast<uint64_t>(skinCount) * kMinSkinBytes > reader.Remaining())
			{
				error = "invalid jointCount " + std::to_string(skinCount)
					+ ": the Skins[] array is larger than the file";
				return false;
			}
			out.Skins.resize(skinCount);
			for (uint32_t index = 0; index < skinCount; ++index)
			{
				WModelSkin& skin = out.Skins[index];
				const std::string label = "skin " + std::to_string(index);
				uint32_t nameLength = 0;
				uint32_t jointCount = 0;
				if (!reader.ReadU32(nameLength, (label + ".nameLength").c_str())
					|| !reader.ReadString(skin.Name, nameLength, (label + ".name").c_str())
					|| !reader.ReadU32(jointCount, (label + ".jointCount").c_str()))
				{
					error = reader.Error;
					return false;
				}
				if (jointCount > kMaxJointsPerSkin)
				{
					error = label + " declares jointCount " + std::to_string(jointCount)
						+ ", above the per-skin limit of " + std::to_string(kMaxJointsPerSkin);
					return false;
				}

				skin.JointNames.resize(jointCount);
				for (uint32_t joint = 0; joint < jointCount; ++joint)
				{
					const std::string jointLabel = label + " joint " + std::to_string(joint);
					uint32_t jointNameLength = 0;
					if (!reader.ReadU32(jointNameLength, (jointLabel + ".nameLength").c_str())
						|| !reader.ReadString(skin.JointNames[joint], jointNameLength,
							(jointLabel + ".name").c_str()))
					{
						error = reader.Error;
						return false;
					}
				}

				// JointNodes[](D5c-3a):关节集合下标 → Nodes[] 下标,跨表引用在下面统一校验。
				skin.JointNodes.resize(jointCount);
				for (uint32_t joint = 0; joint < jointCount; ++joint)
				{
					if (!reader.ReadU32(skin.JointNodes[joint],
						(label + " joint " + std::to_string(joint) + ".node").c_str()))
					{
						error = reader.Error;
						return false;
					}
				}

				skin.JointParents.resize(jointCount);
				for (uint32_t joint = 0; joint < jointCount; ++joint)
				{
					if (!reader.ReadI32(skin.JointParents[joint],
						(label + " joint " + std::to_string(joint) + ".parent").c_str()))
					{
						error = reader.Error;
						return false;
					}
				}

				skin.InverseBindMatrices.resize(jointCount);
				for (uint32_t joint = 0; joint < jointCount; ++joint)
				{
					if (!reader.ReadMat4(skin.InverseBindMatrices[joint],
						(label + " joint " + std::to_string(joint) + ".inverseBind").c_str()))
					{
						error = reader.Error;
						return false;
					}
				}

				skin.BindTranslations.resize(jointCount);
				for (uint32_t joint = 0; joint < jointCount; ++joint)
				{
					if (!reader.ReadVec3(skin.BindTranslations[joint],
						(label + " joint " + std::to_string(joint) + ".bindTranslation").c_str()))
					{
						error = reader.Error;
						return false;
					}
				}

				skin.BindRotations.resize(jointCount);
				for (uint32_t joint = 0; joint < jointCount; ++joint)
				{
					if (!reader.ReadQuat(skin.BindRotations[joint],
						(label + " joint " + std::to_string(joint) + ".bindRotation").c_str()))
					{
						error = reader.Error;
						return false;
					}
				}

				skin.BindScales.resize(jointCount);
				for (uint32_t joint = 0; joint < jointCount; ++joint)
				{
					if (!reader.ReadVec3(skin.BindScales[joint],
						(label + " joint " + std::to_string(joint) + ".bindScale").c_str()))
					{
						error = reader.Error;
						return false;
					}
				}

				for (uint32_t joint = 0; joint < jointCount; ++joint)
				{
					const std::string jointLabel = label + " joint " + std::to_string(joint);
					const int32_t parent = skin.JointParents[joint];
					if (parent < -1 || parent >= static_cast<int32_t>(jointCount))
					{
						error = jointLabel + " has out-of-range parent " + std::to_string(parent)
							+ " (jointCount = " + std::to_string(jointCount) + ")";
						return false;
					}
					// JointNodes 是关节 → 节点的引用:必须是有效节点下标(越界 = 坏文件,不留给运行时)。
					if (skin.JointNodes[joint] >= nodeCount)
					{
						error = jointLabel + " references node " + std::to_string(skin.JointNodes[joint])
							+ " (nodeCount = " + std::to_string(nodeCount) + ")";
						return false;
					}
					// 逆绑定/绑定姿态里的 NaN 会污染所有蒙皮矩阵,与"坏文件硬报错"同一口径拒绝。
					if (!IsFiniteMat4(skin.InverseBindMatrices[joint])
						|| !IsFiniteVec3(skin.BindTranslations[joint])
						|| !IsFiniteQuat(skin.BindRotations[joint])
						|| !IsFiniteVec3(skin.BindScales[joint]))
					{
						error = jointLabel + " has a non-finite bind pose or inverse bind matrix";
						return false;
					}
				}

				// 关节环检测:与节点树同一口径(成环会让姿态求值无限递归)。
				for (uint32_t joint = 0; joint < jointCount; ++joint)
				{
					int32_t current = static_cast<int32_t>(joint);
					uint32_t steps = 0;
					while (current != -1)
					{
						if (++steps > jointCount)
						{
							error = label + " joint hierarchy contains a cycle (starting at joint "
								+ std::to_string(joint) + ")";
							return false;
						}
						current = skin.JointParents[static_cast<size_t>(current)];
					}
				}
			}

			// Animations[](导入期已烘成固定采样率的关键帧,运行时只做线性插值)
			if (static_cast<uint64_t>(animationCount) * kMinAnimationBytes > reader.Remaining())
			{
				error = "invalid animationCount " + std::to_string(animationCount)
					+ ": the Animations[] array is larger than the file";
				return false;
			}
			out.Animations.resize(animationCount);
			for (uint32_t index = 0; index < animationCount; ++index)
			{
				WModelAnimation& animation = out.Animations[index];
				const std::string label = "animation " + std::to_string(index);
				uint32_t nameLength = 0;
				uint32_t channelCount = 0;
				if (!reader.ReadU32(nameLength, (label + ".nameLength").c_str())
					|| !reader.ReadString(animation.Name, nameLength, (label + ".name").c_str())
					|| !reader.ReadF32(animation.Duration, (label + ".duration").c_str())
					|| !reader.ReadU32(channelCount, (label + ".channelCount").c_str()))
				{
					error = reader.Error;
					return false;
				}
				if (!std::isfinite(animation.Duration) || animation.Duration < 0.0f)
				{
					error = label + " has invalid duration " + std::to_string(animation.Duration)
						+ " (expected a non-negative finite number)";
					return false;
				}
				if (static_cast<uint64_t>(channelCount) * kMinChannelBytes > reader.Remaining())
				{
					error = label + " declares channelCount " + std::to_string(channelCount)
						+ ": larger than the file";
					return false;
				}
				animation.Channels.resize(channelCount);
				for (uint32_t channelIndex = 0; channelIndex < channelCount; ++channelIndex)
				{
					WModelAnimationChannel& channel = animation.Channels[channelIndex];
					const std::string channelLabel = label + " channel " + std::to_string(channelIndex);
					uint8_t path = 0;
					uint32_t keyCount = 0;
					if (!reader.ReadU32(channel.TargetNode, (channelLabel + ".targetNode").c_str())
						|| !reader.ReadU8(path, (channelLabel + ".path").c_str())
						|| !reader.ReadU32(keyCount, (channelLabel + ".keyCount").c_str()))
					{
						error = reader.Error;
						return false;
					}
					if (path > static_cast<uint8_t>(WModelAnimationPath::Scale))
					{
						error = channelLabel + " has invalid path " + std::to_string(path)
							+ " (expected 0 = T, 1 = R, 2 = S)";
						return false;
					}
					channel.Path = static_cast<WModelAnimationPath>(path);
					if (channel.TargetNode >= nodeCount)
					{
						error = channelLabel + " targets node " + std::to_string(channel.TargetNode)
							+ " but the model has " + std::to_string(nodeCount) + " node(s)";
						return false;
					}
					if (static_cast<uint64_t>(keyCount) * kMinKeyBytes > reader.Remaining())
					{
						error = channelLabel + " declares keyCount " + std::to_string(keyCount)
							+ ": larger than the file";
						return false;
					}
					channel.Keys.resize(keyCount);
					for (uint32_t keyIndex = 0; keyIndex < keyCount; ++keyIndex)
					{
						WModelAnimationKey& key = channel.Keys[keyIndex];
						const std::string keyLabel = channelLabel + " key " + std::to_string(keyIndex);
						if (!reader.ReadF32(key.Time, (keyLabel + ".time").c_str())
							|| !reader.ReadVec4(key.Value, (keyLabel + ".value").c_str()))
						{
							error = reader.Error;
							return false;
						}
						if (!std::isfinite(key.Time) || !IsFiniteVec4(key.Value))
						{
							error = keyLabel + " is not finite";
							return false;
						}
					}
				}
			}

			// VertexData / Indices
			const bool skinnedVertices = vertexLayoutId == kVertexLayoutSkinned;
			out.Vertices.resize(vertexCount);
			if (skinnedVertices)
				out.SkinVertices.resize(vertexCount);
			for (uint32_t index = 0; index < vertexCount; ++index)
			{
				if (!reader.ReadRaw(&out.Vertices[index], sizeof(WModelVertex), "vertex data"))
				{
					error = reader.Error;
					return false;
				}
				if (!skinnedVertices)
					continue;
				if (!reader.ReadRaw(&out.SkinVertices[index], sizeof(WModelSkinVertex), "vertex skin data"))
				{
					error = reader.Error;
					return false;
				}
				// joints 是越界引用(0..127 精确)—— 一律拒绝;weights 不做归一化校验,
				// 但 NaN/Inf 会污染调色板混合,仍然拒绝。
				const WModelSkinVertex& skinVertex = out.SkinVertices[index];
				for (int component = 0; component < 4; ++component)
				{
					const float joint = skinVertex.Joints[component];
					if (!std::isfinite(joint) || joint < 0.0f
						|| joint > static_cast<float>(kMaxJointsPerSkin - 1u) || std::floor(joint) != joint)
					{
						error = "vertex " + std::to_string(index) + " has invalid joint index "
							+ std::to_string(joint) + " (expected an integer in 0.."
							+ std::to_string(kMaxJointsPerSkin - 1u) + ")";
						return false;
					}
					if (!std::isfinite(skinVertex.Weights[component]))
					{
						error = "vertex " + std::to_string(index) + " has a non-finite skin weight";
						return false;
					}
				}
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

		// P4-U11:header + meta 的轻量解析(读完 meta 就停,几何一个字节都不碰)。
		// 与 ParseImpl 共用 ReadMetaBlock/Reader,错误文案也一致。
		bool ParseMetaImpl(const uint8_t* bytes, size_t size, WModelData::MetaData& out, std::string& error)
		{
			out = WModelData::MetaData {};
			if (bytes == nullptr || size < kHeaderSize)
			{
				error = "truncated: file is smaller than the .wmodel header";
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
					+ " (this build reads version " + std::to_string(kFormatVersion)
					+ "); please re-import the source asset (请重新导入)";
				return false;
			}
			// flags / vertexLayoutId / reserved + 8 个计数(mesh/skin/animation 数量等)全部跳过:
			// meta 的位置固定,不依赖这些计数。
			for (int index = 0; index < 3 + 8; ++index)
			{
				uint32_t skipped = 0;
				reader.ReadU32(skipped, "header");
			}
			if (reader.Failed())
			{
				error = reader.Error;
				return false;
			}
			return ReadMetaBlock(reader, out, error);
		}

		// 路径解析必须与材质/脚本同一条口径:**VFS 优先、磁盘回退**(ReadFile / ReadMeta 共用)。
		// 网格引用是"相对内容根"的逻辑路径(如 models/x.wmodel),而编辑器/Runtime 的 CWD 都不是
		// 内容根;直接 ifstream(逻辑路径) 只会在 CWD 恰好是内容根时成功。
		bool ReadBytes(const std::string& path, std::vector<uint8_t>& bytes, std::string* error)
		{
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
					return true;
				}
			}
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
				return true;
			}
			if (error) *error = "cannot open '" + path + "'";
			return false;
		}
	}

	uint32_t GetVertexStride(uint32_t vertexLayoutId)
	{
		switch (vertexLayoutId)
		{
		case kVertexLayoutStandard:
			return static_cast<uint32_t>(sizeof(WModelVertex));
		case kVertexLayoutSkinned:
			return static_cast<uint32_t>(sizeof(WModelVertex) + sizeof(WModelSkinVertex));
		default:
			return 0u;
		}
	}

	std::vector<uint8_t> Serialize(const WModelData& data)
	{
		const bool skinnedVertices = data.VertexLayoutId == kVertexLayoutSkinned;
		std::vector<uint8_t> out;
		out.reserve(static_cast<size_t>(kHeaderSize + kMetaSize)
			+ data.Meta.SourcePath.size() + 4u
			+ data.Vertices.size() * (sizeof(WModelVertex) + (skinnedVertices ? sizeof(WModelSkinVertex) : 0u))
			+ data.Indices.size() * sizeof(uint32_t)
			+ data.Submeshes.size() * (3u * 4u + 6u * 4u)
			+ data.Meshes.size() * 12u
			+ data.Nodes.size() * (2u * 4u + 10u * 4u + 4u)
			+ data.MaterialSlots.size() * 4u
			+ data.Skins.size() * (kMinSkinBytes + 32u)
			+ data.Animations.size() * kMinAnimationBytes);

		out.insert(out.end(), kMagic, kMagic + sizeof(kMagic));
		AppendU32(out, kFormatVersion);
		AppendU32(out, data.Flags);
		AppendU32(out, data.VertexLayoutId);
		AppendU32(out, 0u);   // reserved
		AppendU32(out, static_cast<uint32_t>(data.Vertices.size()));
		AppendU32(out, static_cast<uint32_t>(data.Indices.size()));
		AppendU32(out, static_cast<uint32_t>(data.Meshes.size()));
		AppendU32(out, static_cast<uint32_t>(data.Submeshes.size()));
		AppendU32(out, static_cast<uint32_t>(data.Nodes.size()));
		AppendU32(out, static_cast<uint32_t>(data.MaterialSlots.size()));
		AppendU32(out, static_cast<uint32_t>(data.Skins.size()));        // wire 名:jointCount
		AppendU32(out, static_cast<uint32_t>(data.Animations.size()));

		AppendU64(out, data.Meta.SourceFingerprint);
		AppendU32(out, data.Meta.ImporterVersion);
		AppendU64(out, data.Meta.SettingsHash);
		AppendU8(out, data.Meta.UpAxis);
		AppendF32(out, data.Meta.Scale);
		AppendU32(out, 0u);   // meta reserved
		AppendString(out, data.Meta.SourcePath);   // v3:源逻辑路径
		// v5:完整导入设置(P4-U11)。
		AppendU8(out, data.Meta.HasSettings ? 1u : 0u);
		if (data.Meta.HasSettings)
		{
			const ModelImportSettings& settings = data.Meta.Settings;
			AppendF32(out, settings.Scale);
			AppendU8(out, settings.UpAxis);
			AppendU8(out, settings.ExportMaterials ? 1u : 0u);
			AppendU8(out, settings.ExportTextures ? 1u : 0u);
			AppendU8(out, settings.ImportAnimations ? 1u : 0u);
			AppendU8(out, settings.ImportSkins ? 1u : 0u);
			AppendF32(out, settings.AnimationSampleRate);
			AppendU8(out, settings.GenerateNormals ? 1u : 0u);
			AppendU8(out, settings.ReuseMaterials ? 1u : 0u);
			AppendU8(out, settings.ReuseTextures ? 1u : 0u);
			AppendString(out, settings.SharedMaterialFolder);
		}
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
			AppendI32(out, range.SkinIndex);
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

		// Skins[]:jointCount 由 JointNames.size() 推出(WriteFile 负责拒绝各数组长度不一致)。
		for (const WModelSkin& skin : data.Skins)
		{
			AppendString(out, skin.Name);
			AppendU32(out, static_cast<uint32_t>(skin.JointNames.size()));
			for (const std::string& jointName : skin.JointNames)
				AppendString(out, jointName);
			// JointNodes 紧跟在 jointNames 之后、jointParents 之前(每条 4B;D5c-3a 追加)。
			for (const uint32_t jointNode : skin.JointNodes)
				AppendU32(out, jointNode);
			for (const int32_t parent : skin.JointParents)
				AppendI32(out, parent);
			for (const glm::mat4& inverseBind : skin.InverseBindMatrices)
				AppendMat4(out, inverseBind);
			for (const glm::vec3& translation : skin.BindTranslations)
				AppendVec3(out, translation);
			for (const glm::quat& rotation : skin.BindRotations)
				AppendQuat(out, rotation);
			for (const glm::vec3& scale : skin.BindScales)
				AppendVec3(out, scale);
		}

		// Animations[]:keyCount 由 Keys.size() 推出。
		for (const WModelAnimation& animation : data.Animations)
		{
			AppendString(out, animation.Name);
			AppendF32(out, animation.Duration);
			AppendU32(out, static_cast<uint32_t>(animation.Channels.size()));
			for (const WModelAnimationChannel& channel : animation.Channels)
			{
				AppendU32(out, channel.TargetNode);
				AppendU8(out, static_cast<uint8_t>(channel.Path));
				AppendU32(out, static_cast<uint32_t>(channel.Keys.size()));
				for (const WModelAnimationKey& key : channel.Keys)
				{
					AppendF32(out, key.Time);
					AppendVec4(out, key.Value);
				}
			}
		}

		if (!data.Vertices.empty())
		{
			// 布局 2 = 标准 32B 之后追加 joints/weights(64B stride)。缺记录时写零,由 WriteFile 门禁拒绝
			// (Serialize 没有错误通道,不能在这里"尽力修复")。
			const WModelSkinVertex kZeroSkinVertex {};
			for (size_t index = 0; index < data.Vertices.size(); ++index)
			{
				const auto* raw = reinterpret_cast<const uint8_t*>(&data.Vertices[index]);
				out.insert(out.end(), raw, raw + sizeof(WModelVertex));
				if (!skinnedVertices)
					continue;
				const WModelSkinVertex& skinVertex = index < data.SkinVertices.size()
					? data.SkinVertices[index] : kZeroSkinVertex;
				const auto* skinRaw = reinterpret_cast<const uint8_t*>(&skinVertex);
				out.insert(out.end(), skinRaw, skinRaw + sizeof(WModelSkinVertex));
			}
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

	// P4-U11:只读 header + meta(几何不解析)。编辑器/cook 判断"这份 .wmodel 是不是这个源的
	// 产物、用的什么导入设置"时用它 —— 不必把整个模型读进内存。
	bool ParseMetaOnly(const uint8_t* bytes, size_t size, WModelData::MetaData& out, std::string* error)
	{
		std::string localError;
		if (ParseMetaImpl(bytes, size, out, localError))
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
		if (!data.Meta.Valid)
		{
			// 契约:meta 必须由导入器写入(源指纹/导入器版本/设置哈希);手写数据不得绕过。
			if (error)
				*error = "refusing to write .wmodel v5 without a valid meta block "
					"(importer must set sourceFingerprint/importerVersion/settingsHash)";
			return false;
		}
		// 顶点布局契约:布局 2 必须每个顶点都有一条 joints/weights 记录(缺失时 Serialize 只能写零,
		// 属于静默错数据);布局 1 不接受残留的蒙皮记录。
		if (GetVertexStride(data.VertexLayoutId) == 0u)
		{
			if (error)
				*error = "refusing to write .wmodel v5: unsupported vertex layout id "
					+ std::to_string(data.VertexLayoutId);
			return false;
		}
		if (data.VertexLayoutId == kVertexLayoutSkinned && data.SkinVertices.size() != data.Vertices.size())
		{
			if (error)
				*error = "refusing to write .wmodel v5: vertex layout "
					+ std::to_string(kVertexLayoutSkinned) + " needs one joints/weights record per vertex (vertices = "
					+ std::to_string(data.Vertices.size()) + ", skin records = "
					+ std::to_string(data.SkinVertices.size()) + ")";
			return false;
		}
		if (data.VertexLayoutId == kVertexLayoutStandard && !data.SkinVertices.empty())
		{
			if (error)
				*error = "refusing to write .wmodel v5: vertex layout " + std::to_string(kVertexLayoutStandard)
					+ " does not carry joints/weights (skin records = "
					+ std::to_string(data.SkinVertices.size()) + ")";
			return false;
		}
		// skin 的七个数组必须同长(jointCount 由 JointNames.size() 推出,长度不齐会写错位数据)。
		for (size_t index = 0; index < data.Skins.size(); ++index)
		{
			const WModelSkin& skin = data.Skins[index];
			const size_t jointCount = skin.JointNames.size();
			if (skin.JointNodes.size() != jointCount || skin.JointParents.size() != jointCount
				|| skin.InverseBindMatrices.size() != jointCount
				|| skin.BindTranslations.size() != jointCount || skin.BindRotations.size() != jointCount
				|| skin.BindScales.size() != jointCount)
			{
				if (error)
					*error = "refusing to write .wmodel v5: skin " + std::to_string(index)
						+ " joint arrays have different lengths (jointCount = " + std::to_string(jointCount)
						+ ", JointNodes = " + std::to_string(skin.JointNodes.size()) + ")";
				return false;
			}
		}
		// mesh → skin 引用必须是 -1(静态)或已存在的 Skins[] 下标。
		for (size_t index = 0; index < data.Meshes.size(); ++index)
		{
			const int64_t skinIndex = data.Meshes[index].SkinIndex;
			if (skinIndex < -1 || skinIndex >= static_cast<int64_t>(data.Skins.size()))
			{
				if (error)
					*error = "refusing to write .wmodel v5: mesh " + std::to_string(index)
						+ " references unknown skin " + std::to_string(skinIndex)
						+ " (Skins[] length = " + std::to_string(data.Skins.size()) + ")";
				return false;
			}
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
		std::vector<uint8_t> bytes;
		if (!ReadBytes(path, bytes, error))
			return false;
		return Parse(bytes.data(), bytes.size(), out, error);
	}

	// P4-U11:只读 meta(同 ReadFile 的 VFS → 磁盘解析顺序,但不解析几何)。
	bool ReadMeta(const std::string& path, WModelData::MetaData& out, std::string* error)
	{
		std::vector<uint8_t> bytes;
		if (!ReadBytes(path, bytes, error))
			return false;
		return ParseMetaOnly(bytes.data(), bytes.size(), out, error);
	}
}

#include "wldpch.h"
#include "World/Asset/WSkeletonIO.h"
#include "World/Asset/WAnimationClipIO.h"
#include "World/Asset/WModelIO.h"
#include "World/Core/Application.h"
#include "World/Utils/Paths.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

namespace World::Asset
{
	namespace
	{
		inline void WriteU32(std::vector<uint8_t>& buf, uint32_t val)
		{
			const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
			buf.insert(buf.end(), p, p + 4);
		}

		inline void WriteI32(std::vector<uint8_t>& buf, int32_t val)
		{
			const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
			buf.insert(buf.end(), p, p + 4);
		}

		inline void WriteU64(std::vector<uint8_t>& buf, uint64_t val)
		{
			const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
			buf.insert(buf.end(), p, p + 8);
		}

		inline void WriteF32(std::vector<uint8_t>& buf, float val)
		{
			const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
			buf.insert(buf.end(), p, p + 4);
		}

		inline void WriteVec3(std::vector<uint8_t>& buf, const glm::vec3& v)
		{
			WriteF32(buf, v.x);
			WriteF32(buf, v.y);
			WriteF32(buf, v.z);
		}

		inline void WriteQuat(std::vector<uint8_t>& buf, const glm::quat& q)
		{
			WriteF32(buf, q.x);
			WriteF32(buf, q.y);
			WriteF32(buf, q.z);
			WriteF32(buf, q.w);
		}

		inline void WriteMat4(std::vector<uint8_t>& buf, const glm::mat4& m)
		{
			const float* p = &m[0][0];
			for (int i = 0; i < 16; ++i)
				WriteF32(buf, p[i]);
		}

		inline void WriteStr(std::vector<uint8_t>& buf, const std::string& str)
		{
			WriteU32(buf, static_cast<uint32_t>(str.size()));
			buf.insert(buf.end(), str.begin(), str.end());
		}

		inline bool ReadU32(const uint8_t* bytes, size_t size, size_t& offset, uint32_t& out)
		{
			if (offset + 4 > size) return false;
			std::memcpy(&out, bytes + offset, 4);
			offset += 4;
			return true;
		}

		inline bool ReadI32(const uint8_t* bytes, size_t size, size_t& offset, int32_t& out)
		{
			if (offset + 4 > size) return false;
			std::memcpy(&out, bytes + offset, 4);
			offset += 4;
			return true;
		}

		inline bool ReadU64(const uint8_t* bytes, size_t size, size_t& offset, uint64_t& out)
		{
			if (offset + 8 > size) return false;
			std::memcpy(&out, bytes + offset, 8);
			offset += 8;
			return true;
		}

		inline bool ReadF32(const uint8_t* bytes, size_t size, size_t& offset, float& out)
		{
			if (offset + 4 > size) return false;
			std::memcpy(&out, bytes + offset, 4);
			offset += 4;
			return true;
		}

		inline bool ReadVec3(const uint8_t* bytes, size_t size, size_t& offset, glm::vec3& out)
		{
			return ReadF32(bytes, size, offset, out.x) &&
			       ReadF32(bytes, size, offset, out.y) &&
			       ReadF32(bytes, size, offset, out.z);
		}

		inline bool ReadQuat(const uint8_t* bytes, size_t size, size_t& offset, glm::quat& out)
		{
			return ReadF32(bytes, size, offset, out.x) &&
			       ReadF32(bytes, size, offset, out.y) &&
			       ReadF32(bytes, size, offset, out.z) &&
			       ReadF32(bytes, size, offset, out.w);
		}

		inline bool ReadMat4(const uint8_t* bytes, size_t size, size_t& offset, glm::mat4& out)
		{
			float* p = &out[0][0];
			for (int i = 0; i < 16; ++i)
			{
				if (!ReadF32(bytes, size, offset, p[i]))
					return false;
			}
			return true;
		}

		inline bool ReadStr(const uint8_t* bytes, size_t size, size_t& offset, std::string& out)
		{
			uint32_t len = 0;
			if (!ReadU32(bytes, size, offset, len)) return false;
			if (offset + len > size) return false;
			out.assign(reinterpret_cast<const char*>(bytes + offset), len);
			offset += len;
			return true;
		}

		bool ResolveAssetBytes(const std::string& path, std::vector<uint8_t>& bytes, std::string* error)
		{
			if (path.empty())
			{
				if (error) *error = "path is empty";
				return false;
			}

			// 直接绝对路径或本地文件
			std::filesystem::path direct(path);
			if (std::filesystem::exists(direct) && !std::filesystem::is_directory(direct))
			{
				std::ifstream file(direct, std::ios::binary);
				if (file)
				{
					bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
					if (!bytes.empty())
					{
						if (error) error->clear();
						return true;
					}
				}
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

			std::string stripped = path;
			if (stripped.rfind("engine/", 0) == 0 || stripped.rfind("Engine/", 0) == 0)
				stripped = stripped.substr(7);

			const std::filesystem::path candidates[] = {
				World::Paths::AssetRoot() / path,
				World::Paths::AssetRoot() / stripped,
				World::Paths::EngineContentRoot() / path,
				World::Paths::EngineContentRoot() / stripped,
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

	void WSkeletonData::BuildSubtreeIndices()
	{
		const size_t n = Bones.size();
		SubtreeIndices.clear();
		SubtreeIndices.resize(n);

		for (size_t i = 0; i < n; ++i)
		{
			for (size_t j = 0; j < n; ++j)
			{
				if (i == j)
					continue;
				int32_t p = Bones[j].ParentIndex;
				while (p >= 0 && static_cast<size_t>(p) < n)
				{
					if (static_cast<size_t>(p) == i)
					{
						SubtreeIndices[i].push_back(static_cast<uint32_t>(j));
						break;
					}
					p = Bones[static_cast<size_t>(p)].ParentIndex;
				}
			}
		}
	}

	int32_t WSkeletonData::FindBoneIndex(NameId name) const
	{
		if (!name.IsValid())
			return -1;
		for (size_t i = 0; i < Bones.size(); ++i)
		{
			if (Bones[i].Name == name)
				return static_cast<int32_t>(i);
		}
		return -1;
	}

	int32_t WSkeletonData::FindBoneIndex(const std::string& name) const
	{
		for (size_t i = 0; i < Bones.size(); ++i)
		{
			if (Bones[i].NameStr == name)
				return static_cast<int32_t>(i);
		}
		const NameId nid = StringPool::Get().InternName(name);
		return FindBoneIndex(nid);
	}

	int32_t WSkeletonData::FindSocketIndex(NameId name) const
	{
		if (!name.IsValid())
			return -1;
		for (size_t i = 0; i < Sockets.size(); ++i)
		{
			if (Sockets[i].Name == name)
				return static_cast<int32_t>(i);
		}
		return -1;
	}

	int32_t WSkeletonData::FindSocketIndex(const std::string& name) const
	{
		for (size_t i = 0; i < Sockets.size(); ++i)
		{
			if (Sockets[i].NameStr == name)
				return static_cast<int32_t>(i);
		}
		const NameId nid = StringPool::Get().InternName(name);
		return FindSocketIndex(nid);
	}

	namespace WSkeletonIO
	{
		std::vector<uint8_t> Serialize(const WSkeletonData& data)
		{
			std::vector<uint8_t> bytes;
			bytes.reserve(64 + data.Bones.size() * 128 + data.Sockets.size() * 64);

			WriteU32(bytes, kMagic);
			WriteU32(bytes, kFormatVersion);
			WriteU64(bytes, data.Identity.Value);
			WriteU32(bytes, static_cast<uint32_t>(data.Bones.size()));
			WriteU32(bytes, static_cast<uint32_t>(data.Sockets.size()));
			WriteStr(bytes, data.SourceModelPath);

			for (const SkeletonBone& bone : data.Bones)
			{
				const std::string name = bone.NameStr.empty()
					? StringPool::Get().NameOf(bone.Name) : bone.NameStr;
				WriteStr(bytes, name);
				WriteI32(bytes, bone.ParentIndex);
				WriteVec3(bytes, bone.BindPoseLocal.Translation);
				WriteQuat(bytes, bone.BindPoseLocal.Rotation);
				WriteVec3(bytes, bone.BindPoseLocal.Scale);
				WriteMat4(bytes, bone.InverseBindMatrix);
			}

			for (const SkeletonSocket& socket : data.Sockets)
			{
				const std::string name = socket.NameStr.empty()
					? StringPool::Get().NameOf(socket.Name) : socket.NameStr;
				WriteStr(bytes, name);
				WriteI32(bytes, socket.BoneIndex);
				WriteVec3(bytes, socket.RelativeTransform.Translation);
				WriteQuat(bytes, socket.RelativeTransform.Rotation);
				WriteVec3(bytes, socket.RelativeTransform.Scale);
			}

			return bytes;
		}

		bool Parse(const uint8_t* bytes, size_t size, WSkeletonData& out, std::string* error)
		{
			if (!bytes || size < 24)
			{
				if (error) *error = "buffer too small for WSkeleton";
				return false;
			}

			size_t offset = 0;
			uint32_t magic = 0;
			if (!ReadU32(bytes, size, offset, magic) || magic != kMagic)
			{
				if (error) *error = "invalid WSkeleton magic";
				return false;
			}

			uint32_t version = 0;
			if (!ReadU32(bytes, size, offset, version) || version != kFormatVersion)
			{
				if (error) *error = "unsupported WSkeleton version";
				return false;
			}

			uint64_t idVal = 0;
			if (!ReadU64(bytes, size, offset, idVal)) return false;
			out.Identity = AssetId { idVal };

			uint32_t boneCount = 0;
			uint32_t socketCount = 0;
			if (!ReadU32(bytes, size, offset, boneCount)) return false;
			if (!ReadU32(bytes, size, offset, socketCount)) return false;
			if (boneCount > kMaxBones)
			{
				if (error) *error = "bone count exceeds limit";
				return false;
			}

			if (!ReadStr(bytes, size, offset, out.SourceModelPath)) return false;

			out.Bones.clear();
			out.Bones.resize(boneCount);
			for (uint32_t i = 0; i < boneCount; ++i)
			{
				SkeletonBone& bone = out.Bones[i];
				if (!ReadStr(bytes, size, offset, bone.NameStr)) return false;
				bone.Name = StringPool::Get().InternName(bone.NameStr);
				if (!ReadI32(bytes, size, offset, bone.ParentIndex)) return false;
				if (!ReadVec3(bytes, size, offset, bone.BindPoseLocal.Translation)) return false;
				if (!ReadQuat(bytes, size, offset, bone.BindPoseLocal.Rotation)) return false;
				if (!ReadVec3(bytes, size, offset, bone.BindPoseLocal.Scale)) return false;
				if (!ReadMat4(bytes, size, offset, bone.InverseBindMatrix)) return false;
			}

			out.Sockets.clear();
			out.Sockets.resize(socketCount);
			for (uint32_t i = 0; i < socketCount; ++i)
			{
				SkeletonSocket& socket = out.Sockets[i];
				if (!ReadStr(bytes, size, offset, socket.NameStr)) return false;
				socket.Name = StringPool::Get().InternName(socket.NameStr);
				if (!ReadI32(bytes, size, offset, socket.BoneIndex)) return false;
				if (!ReadVec3(bytes, size, offset, socket.RelativeTransform.Translation)) return false;
				if (!ReadQuat(bytes, size, offset, socket.RelativeTransform.Rotation)) return false;
				if (!ReadVec3(bytes, size, offset, socket.RelativeTransform.Scale)) return false;
			}

			out.BuildSubtreeIndices();
			if (error) error->clear();
			return true;
		}

		bool WriteFile(const std::string& path, const WSkeletonData& data, std::string* error)
		{
			std::vector<uint8_t> bytes = Serialize(data);
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			if (!file)
			{
				if (error) *error = "cannot open file for writing: " + path;
				return false;
			}
			file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if (!file)
			{
				if (error) *error = "failed writing to file: " + path;
				return false;
			}
			if (error) error->clear();
			return true;
		}

		bool ReadFile(const std::string& path, WSkeletonData& out, std::string* error)
		{
			std::vector<uint8_t> bytes;
			if (!ResolveAssetBytes(path, bytes, error))
				return false;
			return Parse(bytes.data(), bytes.size(), out, error);
		}

		bool ExtractSkeletonAndClips(const WModelData& model,
			WSkeletonData& outSkeleton,
			std::vector<WAnimationClipData>& outClips,
			std::string* error)
		{
			if (model.Skins.empty())
			{
				if (error) *error = "WModelData has no skins to extract skeleton from";
				return false;
			}

			const WModelSkin& skin = model.Skins.front();
			const size_t jointCount = skin.JointNames.size();
			if (jointCount == 0)
			{
				if (error) *error = "Skin has zero joints";
				return false;
			}

			outSkeleton.Identity = GenerateAssetId();
			outSkeleton.SourceModelPath = model.Meta.SourcePath;
			outSkeleton.Bones.clear();
			outSkeleton.Bones.resize(jointCount);

			for (size_t j = 0; j < jointCount; ++j)
			{
				SkeletonBone& bone = outSkeleton.Bones[j];
				bone.NameStr = skin.JointNames[j];
				bone.Name = StringPool::Get().InternName(bone.NameStr);
				bone.ParentIndex = (j < skin.JointParents.size()) ? skin.JointParents[j] : -1;
				if (j < skin.BindTranslations.size())
					bone.BindPoseLocal.Translation = skin.BindTranslations[j];
				if (j < skin.BindRotations.size())
					bone.BindPoseLocal.Rotation = skin.BindRotations[j];
				if (j < skin.BindScales.size())
					bone.BindPoseLocal.Scale = skin.BindScales[j];
				if (j < skin.InverseBindMatrices.size())
					bone.InverseBindMatrix = skin.InverseBindMatrices[j];
			}
			outSkeleton.BuildSubtreeIndices();

			outClips.clear();
			for (const WModelAnimation& anim : model.Animations)
			{
				WAnimationClipData clip;
				clip.Identity = GenerateAssetId();
				clip.Name = anim.Name;
				clip.Duration = anim.Duration;
				clip.SampleRate = 30.0f;
				clip.Loop = true;
				clip.HasRootMotion = false;

				for (const WModelAnimationChannel& ch : anim.Channels)
				{
					int32_t jointIdx = -1;
					for (size_t k = 0; k < skin.JointNodes.size(); ++k)
					{
						if (skin.JointNodes[k] == ch.TargetNode)
						{
							jointIdx = static_cast<int32_t>(k);
							break;
						}
					}

					std::string boneName;
					if (jointIdx >= 0 && static_cast<size_t>(jointIdx) < skin.JointNames.size())
						boneName = skin.JointNames[jointIdx];
					else if (ch.TargetNode < model.Nodes.size())
						boneName = model.Nodes[ch.TargetNode].Name;

					if (boneName.empty())
						continue;

					AnimationChannel* dstCh = nullptr;
					for (AnimationChannel& c : clip.Channels)
					{
						if (c.BoneNameStr == boneName)
						{
							dstCh = &c;
							break;
						}
					}
					if (!dstCh)
					{
						clip.Channels.emplace_back();
						dstCh = &clip.Channels.back();
						dstCh->BoneNameStr = boneName;
						dstCh->BoneName = StringPool::Get().InternName(boneName);
					}

					if (ch.Path == WModelIO::WModelAnimationPath::Translation)
					{
						for (const WModelAnimationKey& key : ch.Keys)
							dstCh->PositionKeys.push_back({ key.Time, glm::vec3(key.Value) });

						if (jointIdx == 0 || boneName == "Root" || boneName == "root")
						{
							for (const WModelAnimationKey& key : ch.Keys)
								clip.RootTranslation.push_back({ key.Time, glm::vec3(key.Value) });
							clip.HasRootMotion = true;
						}
					}
					else if (ch.Path == WModelIO::WModelAnimationPath::Rotation)
					{
						for (const WModelAnimationKey& key : ch.Keys)
							dstCh->RotationKeys.push_back({ key.Time, glm::quat(key.Value.w, key.Value.x, key.Value.y, key.Value.z) });

						if (jointIdx == 0 || boneName == "Root" || boneName == "root")
						{
							for (const WModelAnimationKey& key : ch.Keys)
								clip.RootRotation.push_back({ key.Time, glm::quat(key.Value.w, key.Value.x, key.Value.y, key.Value.z) });
							clip.HasRootMotion = true;
						}
					}
					else if (ch.Path == WModelIO::WModelAnimationPath::Scale)
					{
						for (const WModelAnimationKey& key : ch.Keys)
							dstCh->ScaleKeys.push_back({ key.Time, glm::vec3(key.Value) });
					}
				}

				outClips.push_back(std::move(clip));
			}

			if (error) error->clear();
			return true;
		}
	}
}

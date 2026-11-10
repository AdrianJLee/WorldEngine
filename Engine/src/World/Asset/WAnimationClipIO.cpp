#include "wldpch.h"
#include "World/Asset/WAnimationClipIO.h"
#include "World/Core/Application.h"
#include "World/Utils/Paths.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace World::Asset
{
	namespace
	{
		inline void WriteU32(std::vector<uint8_t>& buf, uint32_t val)
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

	glm::vec3 AnimationChannel::SamplePosition(float time, const glm::vec3& fallback) const
	{
		if (PositionKeys.empty())
			return fallback;
		if (PositionKeys.size() == 1 || time <= PositionKeys.front().Time)
			return PositionKeys.front().Value;
		if (time >= PositionKeys.back().Time)
			return PositionKeys.back().Value;

		auto it = std::upper_bound(PositionKeys.begin(), PositionKeys.end(), time,
			[](float val, const AnimationKeyVec3& k) { return val < k.Time; });
		if (it == PositionKeys.end())
			return PositionKeys.back().Value;
		if (it == PositionKeys.begin())
			return PositionKeys.front().Value;

		const auto prev = it - 1;
		const float range = it->Time - prev->Time;
		const float alpha = (range > 1e-6f) ? std::clamp((time - prev->Time) / range, 0.0f, 1.0f) : 0.0f;
		return glm::mix(prev->Value, it->Value, alpha);
	}

	glm::quat AnimationChannel::SampleRotation(float time, const glm::quat& fallback) const
	{
		if (RotationKeys.empty())
			return fallback;
		if (RotationKeys.size() == 1 || time <= RotationKeys.front().Time)
			return glm::normalize(RotationKeys.front().Value);
		if (time >= RotationKeys.back().Time)
			return glm::normalize(RotationKeys.back().Value);

		auto it = std::upper_bound(RotationKeys.begin(), RotationKeys.end(), time,
			[](float val, const AnimationKeyQuat& k) { return val < k.Time; });
		if (it == RotationKeys.end())
			return glm::normalize(RotationKeys.back().Value);
		if (it == RotationKeys.begin())
			return glm::normalize(RotationKeys.front().Value);

		const auto prev = it - 1;
		const float range = it->Time - prev->Time;
		const float alpha = (range > 1e-6f) ? std::clamp((time - prev->Time) / range, 0.0f, 1.0f) : 0.0f;
		return glm::normalize(glm::slerp(glm::normalize(prev->Value), glm::normalize(it->Value), alpha));
	}

	glm::vec3 AnimationChannel::SampleScale(float time, const glm::vec3& fallback) const
	{
		if (ScaleKeys.empty())
			return fallback;
		if (ScaleKeys.size() == 1 || time <= ScaleKeys.front().Time)
			return ScaleKeys.front().Value;
		if (time >= ScaleKeys.back().Time)
			return ScaleKeys.back().Value;

		auto it = std::upper_bound(ScaleKeys.begin(), ScaleKeys.end(), time,
			[](float val, const AnimationKeyVec3& k) { return val < k.Time; });
		if (it == ScaleKeys.end())
			return ScaleKeys.back().Value;
		if (it == ScaleKeys.begin())
			return ScaleKeys.front().Value;

		const auto prev = it - 1;
		const float range = it->Time - prev->Time;
		const float alpha = (range > 1e-6f) ? std::clamp((time - prev->Time) / range, 0.0f, 1.0f) : 0.0f;
		return glm::mix(prev->Value, it->Value, alpha);
	}

	const AnimationChannel* WAnimationClipData::FindChannel(NameId boneName) const
	{
		if (!boneName.IsValid())
			return nullptr;
		for (const auto& ch : Channels)
		{
			if (ch.BoneName == boneName)
				return &ch;
		}
		return nullptr;
	}

	const AnimationChannel* WAnimationClipData::FindChannel(const std::string& boneName) const
	{
		for (const auto& ch : Channels)
		{
			if (ch.BoneNameStr == boneName)
				return &ch;
		}
		const NameId nid = StringPool::Get().InternName(boneName);
		return FindChannel(nid);
	}

	namespace WAnimationClipIO
	{
		std::vector<uint8_t> Serialize(const WAnimationClipData& data)
		{
			std::vector<uint8_t> bytes;
			bytes.reserve(128 + data.Channels.size() * 256 + data.Events.size() * 32);

			WriteU32(bytes, kMagic);
			WriteU32(bytes, kFormatVersion);
			WriteU64(bytes, data.Identity.Value);
			WriteF32(bytes, data.Duration);
			WriteF32(bytes, data.SampleRate);

			uint32_t flags = 0;
			if (data.Loop) flags |= 0x1;
			if (data.HasRootMotion) flags |= 0x2;
			WriteU32(bytes, flags);

			const std::string skelPath = data.SkeletonPath.empty()
				? StringPool::Get().PathOf(data.Skeleton.Path) : data.SkeletonPath;
			WriteStr(bytes, skelPath);
			WriteStr(bytes, data.Name);

			WriteU32(bytes, static_cast<uint32_t>(data.Channels.size()));
			WriteU32(bytes, static_cast<uint32_t>(data.Events.size()));
			WriteU32(bytes, static_cast<uint32_t>(data.RootTranslation.size()));
			WriteU32(bytes, static_cast<uint32_t>(data.RootRotation.size()));

			for (const auto& k : data.RootTranslation)
			{
				WriteF32(bytes, k.Time);
				WriteVec3(bytes, k.Value);
			}

			for (const auto& k : data.RootRotation)
			{
				WriteF32(bytes, k.Time);
				WriteQuat(bytes, k.Value);
			}

			for (const auto& ch : data.Channels)
			{
				const std::string boneName = ch.BoneNameStr.empty()
					? StringPool::Get().NameOf(ch.BoneName) : ch.BoneNameStr;
				WriteStr(bytes, boneName);

				WriteU32(bytes, static_cast<uint32_t>(ch.PositionKeys.size()));
				for (const auto& k : ch.PositionKeys)
				{
					WriteF32(bytes, k.Time);
					WriteVec3(bytes, k.Value);
				}

				WriteU32(bytes, static_cast<uint32_t>(ch.RotationKeys.size()));
				for (const auto& k : ch.RotationKeys)
				{
					WriteF32(bytes, k.Time);
					WriteQuat(bytes, k.Value);
				}

				WriteU32(bytes, static_cast<uint32_t>(ch.ScaleKeys.size()));
				for (const auto& k : ch.ScaleKeys)
				{
					WriteF32(bytes, k.Time);
					WriteVec3(bytes, k.Value);
				}
			}

			for (const auto& ev : data.Events)
			{
				WriteF32(bytes, ev.Time);
				const std::string evName = ev.NameStr.empty()
					? StringPool::Get().NameOf(ev.Name) : ev.NameStr;
				WriteStr(bytes, evName);
				WriteStr(bytes, ev.Payload);
			}

			return bytes;
		}

		bool Parse(const uint8_t* bytes, size_t size, WAnimationClipData& out, std::string* error)
		{
			if (!bytes || size < 32)
			{
				if (error) *error = "buffer too small for WAnimationClip";
				return false;
			}

			size_t offset = 0;
			uint32_t magic = 0;
			if (!ReadU32(bytes, size, offset, magic) || magic != kMagic)
			{
				if (error) *error = "invalid WAnimationClip magic";
				return false;
			}

			uint32_t version = 0;
			if (!ReadU32(bytes, size, offset, version) || version != kFormatVersion)
			{
				if (error) *error = "unsupported WAnimationClip version";
				return false;
			}

			uint64_t idVal = 0;
			if (!ReadU64(bytes, size, offset, idVal)) return false;
			out.Identity = AssetId { idVal };

			if (!ReadF32(bytes, size, offset, out.Duration)) return false;
			if (!ReadF32(bytes, size, offset, out.SampleRate)) return false;

			uint32_t flags = 0;
			if (!ReadU32(bytes, size, offset, flags)) return false;
			out.Loop = (flags & 0x1) != 0;
			out.HasRootMotion = (flags & 0x2) != 0;

			if (!ReadStr(bytes, size, offset, out.SkeletonPath)) return false;
			out.Skeleton = AssetRef::FromPath(out.SkeletonPath);
			if (!ReadStr(bytes, size, offset, out.Name)) return false;

			uint32_t channelCount = 0;
			uint32_t eventCount = 0;
			uint32_t rootPosCount = 0;
			uint32_t rootRotCount = 0;
			if (!ReadU32(bytes, size, offset, channelCount)) return false;
			if (!ReadU32(bytes, size, offset, eventCount)) return false;
			if (!ReadU32(bytes, size, offset, rootPosCount)) return false;
			if (!ReadU32(bytes, size, offset, rootRotCount)) return false;

			out.RootTranslation.resize(rootPosCount);
			for (uint32_t i = 0; i < rootPosCount; ++i)
			{
				if (!ReadF32(bytes, size, offset, out.RootTranslation[i].Time)) return false;
				if (!ReadVec3(bytes, size, offset, out.RootTranslation[i].Value)) return false;
			}

			out.RootRotation.resize(rootRotCount);
			for (uint32_t i = 0; i < rootRotCount; ++i)
			{
				if (!ReadF32(bytes, size, offset, out.RootRotation[i].Time)) return false;
				if (!ReadQuat(bytes, size, offset, out.RootRotation[i].Value)) return false;
			}

			out.Channels.resize(channelCount);
			for (uint32_t i = 0; i < channelCount; ++i)
			{
				AnimationChannel& ch = out.Channels[i];
				if (!ReadStr(bytes, size, offset, ch.BoneNameStr)) return false;
				ch.BoneName = StringPool::Get().InternName(ch.BoneNameStr);

				uint32_t posCount = 0;
				if (!ReadU32(bytes, size, offset, posCount)) return false;
				ch.PositionKeys.resize(posCount);
				for (uint32_t k = 0; k < posCount; ++k)
				{
					if (!ReadF32(bytes, size, offset, ch.PositionKeys[k].Time)) return false;
					if (!ReadVec3(bytes, size, offset, ch.PositionKeys[k].Value)) return false;
				}

				uint32_t rotCount = 0;
				if (!ReadU32(bytes, size, offset, rotCount)) return false;
				ch.RotationKeys.resize(rotCount);
				for (uint32_t k = 0; k < rotCount; ++k)
				{
					if (!ReadF32(bytes, size, offset, ch.RotationKeys[k].Time)) return false;
					if (!ReadQuat(bytes, size, offset, ch.RotationKeys[k].Value)) return false;
				}

				uint32_t scaleCount = 0;
				if (!ReadU32(bytes, size, offset, scaleCount)) return false;
				ch.ScaleKeys.resize(scaleCount);
				for (uint32_t k = 0; k < scaleCount; ++k)
				{
					if (!ReadF32(bytes, size, offset, ch.ScaleKeys[k].Time)) return false;
					if (!ReadVec3(bytes, size, offset, ch.ScaleKeys[k].Value)) return false;
				}
			}

			out.Events.resize(eventCount);
			for (uint32_t i = 0; i < eventCount; ++i)
			{
				AnimationEvent& ev = out.Events[i];
				if (!ReadF32(bytes, size, offset, ev.Time)) return false;
				if (!ReadStr(bytes, size, offset, ev.NameStr)) return false;
				ev.Name = StringPool::Get().InternName(ev.NameStr);
				if (!ReadStr(bytes, size, offset, ev.Payload)) return false;
			}

			if (error) error->clear();
			return true;
		}

		bool WriteFile(const std::string& path, const WAnimationClipData& data, std::string* error)
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

		bool ReadFile(const std::string& path, WAnimationClipData& out, std::string* error)
		{
			std::vector<uint8_t> bytes;
			if (!ResolveAssetBytes(path, bytes, error))
				return false;
			return Parse(bytes.data(), bytes.size(), out, error);
		}

		bool SampleAnimationClip(const WAnimationClipData& clip,
			float time,
			bool loop,
			const WSkeletonData& skeleton,
			std::vector<JointTransform>& outPose,
			glm::vec3* outRootPos,
			glm::quat* outRootRot)
		{
			float t = time;
			if (loop && clip.Duration > 1e-5f)
			{
				t = std::fmod(time, clip.Duration);
				if (t < 0.0f)
					t += clip.Duration;
			}
			else
			{
				t = std::clamp(time, 0.0f, std::max(0.0f, clip.Duration));
			}

			const size_t boneCount = skeleton.Bones.size();
			outPose.resize(boneCount);

			for (size_t i = 0; i < boneCount; ++i)
			{
				const SkeletonBone& bone = skeleton.Bones[i];
				outPose[i] = bone.BindPoseLocal;

				const AnimationChannel* ch = clip.FindChannel(bone.Name);
				if (!ch && !bone.NameStr.empty())
					ch = clip.FindChannel(bone.NameStr);

				if (ch)
				{
					outPose[i].Translation = ch->SamplePosition(t, bone.BindPoseLocal.Translation);
					outPose[i].Rotation = ch->SampleRotation(t, bone.BindPoseLocal.Rotation);
					outPose[i].Scale = ch->SampleScale(t, bone.BindPoseLocal.Scale);
				}
			}

			if (outRootPos)
			{
				if (!clip.RootTranslation.empty())
				{
					AnimationChannel tempCh;
					tempCh.PositionKeys = clip.RootTranslation;
					*outRootPos = tempCh.SamplePosition(t, glm::vec3(0.0f));
				}
				else if (!outPose.empty())
				{
					*outRootPos = outPose[0].Translation;
				}
				else
				{
					*outRootPos = glm::vec3(0.0f);
				}
			}

			if (outRootRot)
			{
				if (!clip.RootRotation.empty())
				{
					AnimationChannel tempCh;
					tempCh.RotationKeys = clip.RootRotation;
					*outRootRot = tempCh.SampleRotation(t, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
				}
				else if (!outPose.empty())
				{
					*outRootRot = outPose[0].Rotation;
				}
				else
				{
					*outRootRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
				}
			}

			return true;
		}
	}
}

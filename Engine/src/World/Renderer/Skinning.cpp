#include "World/Renderer/Skinning.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>

namespace World
{
	namespace
	{
		// 无关键帧的通道按该路径的单位值上报(与容器一致:T/S 的 w 写 0,旋转用 identity)。
		glm::vec4 DefaultChannelValue(WModelIO::WModelAnimationPath path)
		{
			switch (path)
			{
			case WModelIO::WModelAnimationPath::Rotation:
				return { 0.0f, 0.0f, 0.0f, 1.0f };
			case WModelIO::WModelAnimationPath::Scale:
				return { 1.0f, 1.0f, 1.0f, 0.0f };
			case WModelIO::WModelAnimationPath::Translation:
			default:
				return { 0.0f, 0.0f, 0.0f, 0.0f };
			}
		}

		glm::vec4 SampleChannel(const Asset::WModelAnimationChannel& channel, float time)
		{
			if (channel.Keys.empty())
				return DefaultChannelValue(channel.Path);
			if (time <= channel.Keys.front().Time)
				return channel.Keys.front().Value;
			if (time >= channel.Keys.back().Time)
				return channel.Keys.back().Value;

			// 关键帧时间由导入器排好序;upper_bound 找第一个 time > 采样时间的帧。
			const auto next = std::upper_bound(channel.Keys.begin(), channel.Keys.end(), time,
				[](float value, const Asset::WModelAnimationKey& key) { return value < key.Time; });
			const Asset::WModelAnimationKey& upper = *next;
			const Asset::WModelAnimationKey& lower = *(next - 1);
			const float span = upper.Time - lower.Time;
			const float alpha = span > 0.0f ? (time - lower.Time) / span : 0.0f;

			if (channel.Path == WModelIO::WModelAnimationPath::Rotation)
			{
				// 旋转按四元数 slerp(GLM 自带最短路径处理),结果重新归一化以抵抗浮点漂移。
				const glm::quat from = glm::normalize(
					glm::quat(lower.Value.w, lower.Value.x, lower.Value.y, lower.Value.z));
				const glm::quat to = glm::normalize(
					glm::quat(upper.Value.w, upper.Value.x, upper.Value.y, upper.Value.z));
				const glm::quat value = glm::normalize(glm::slerp(from, to, alpha));
				return { value.x, value.y, value.z, value.w };
			}
			// 平移/缩放:x,y,z 线性(w 在容器里恒 0,插值后仍是 0)。
			return glm::mix(lower.Value, upper.Value, alpha);
		}
	}

	std::vector<AnimationSample> SampleAnimation(const Asset::WModelAnimation& animation, float time)
	{
		std::vector<AnimationSample> samples;
		if (animation.Channels.empty())
			return samples;

		// time 先 clamp 到 [0, duration](负时长按 0 处理,不产生反向区间)。
		const float duration = animation.Duration > 0.0f ? animation.Duration : 0.0f;
		const float clamped = std::min(std::max(time, 0.0f), duration);

		samples.reserve(animation.Channels.size());
		for (const Asset::WModelAnimationChannel& channel : animation.Channels)
		{
			AnimationSample sample;
			sample.Node = channel.TargetNode;
			sample.Path = channel.Path;
			sample.Value = SampleChannel(channel, clamped);
			samples.push_back(sample);
		}
		return samples;
	}

	std::vector<glm::mat4> BuildJointMatrices(const Asset::WModelSkin& skin,
		const std::vector<glm::mat4>& jointWorld, const glm::mat4& meshWorld)
	{
		const size_t jointCount = skin.JointNodes.size();
		// 逆绑定矩阵少一条 = 调色板不完整(容器不变量之外的手工数据):返回空,不越界读。
		if (skin.InverseBindMatrices.size() < jointCount)
			return {};
		for (size_t joint = 0; joint < jointCount; ++joint)
		{
			// 调用方给的 jointWorld 引用越界 → 空 vector(调用方按"无蒙皮"处理)。
			if (static_cast<size_t>(skin.JointNodes[joint]) >= jointWorld.size())
				return {};
		}

		const glm::mat4 inverseMesh = glm::inverse(meshWorld);
		std::vector<glm::mat4> palette(jointCount, glm::mat4(1.0f));
		for (size_t joint = 0; joint < jointCount; ++joint)
			palette[joint] = inverseMesh * jointWorld[static_cast<size_t>(skin.JointNodes[joint])]
				* skin.InverseBindMatrices[joint];
		return palette;
	}
}

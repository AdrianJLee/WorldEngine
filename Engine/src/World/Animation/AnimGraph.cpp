#include "World/Renderer/Skinning.h"
#include "wldpch.h"
#include "World/Animation/AnimGraph.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>

namespace World
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;

		float SmoothStep(float edge0, float edge1, float x)
		{
			float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}
	}

	glm::mat4 JointTransform::ToMatrix() const
	{
		glm::mat4 m = glm::translate(glm::mat4(1.0f), Translation);
		m *= glm::mat4_cast(Rotation);
		m = glm::scale(m, Scale);
		return m;
	}

	JointTransform JointTransform::Lerp(const JointTransform& a, const JointTransform& b, float factor)
	{
		JointTransform out;
		out.Translation = glm::mix(a.Translation, b.Translation, factor);
		out.Rotation = glm::slerp(a.Rotation, b.Rotation, factor);
		out.Scale = glm::mix(a.Scale, b.Scale, factor);
		return out;
	}

	AnimPose AnimPose::Blend(const AnimPose& a, const AnimPose& b, float factor)
	{
		AnimPose out;
		const size_t count = std::min(a.Joints.size(), b.Joints.size());
		out.Joints.resize(count);
		for (size_t i = 0; i < count; ++i)
		{
			out.Joints[i] = JointTransform::Lerp(a.Joints[i], b.Joints[i], factor);
		}
		out.RootMotionDelta = glm::mix(a.RootMotionDelta, b.RootMotionDelta, factor);
		out.RootRotationDelta = glm::slerp(a.RootRotationDelta, b.RootRotationDelta, factor);
		return out;
	}

	AnimPose AnimPose::LayeredBlend(const AnimPose& basePose, const AnimPose& layerPose,
		const std::vector<float>& boneMask, float weight)
	{
		AnimPose out = basePose;
		const size_t count = std::min(basePose.Joints.size(), layerPose.Joints.size());
		for (size_t i = 0; i < count; ++i)
		{
			const float mask = (i < boneMask.size()) ? boneMask[i] : 0.0f;
			const float finalWeight = std::clamp(mask * weight, 0.0f, 1.0f);
			if (finalWeight > 0.001f)
			{
				out.Joints[i] = JointTransform::Lerp(basePose.Joints[i], layerPose.Joints[i], finalWeight);
			}
		}
		return out;
	}

	bool TwoBoneIK::Solve(
		const glm::vec3& rootPos,
		const glm::vec3& midPos,
		const glm::vec3& endPos,
		const glm::vec3& targetPos,
		const glm::vec3& poleVector,
		glm::quat& outRootRot,
		glm::quat& outMidRot)
	{
		const float d1 = glm::distance(rootPos, midPos);
		const float d2 = glm::distance(midPos, endPos);
		if (d1 <= 0.001f || d2 <= 0.001f)
			return false;

		const float maxReach = d1 + d2 - 0.001f;
		const float targetDist = std::clamp(glm::distance(rootPos, targetPos), 0.001f, maxReach);

		// 余弦定理求解内角
		const float cosAlpha = std::clamp((d1 * d1 + targetDist * targetDist - d2 * d2) / (2.0f * d1 * targetDist), -1.0f, 1.0f);
		const float cosBeta  = std::clamp((d1 * d1 + d2 * d2 - targetDist * targetDist) / (2.0f * d1 * d2), -1.0f, 1.0f);

		const float angleAlpha = std::acos(cosAlpha);
		const float angleBeta  = std::acos(cosBeta);

		const glm::vec3 toTarget = glm::normalize(targetPos - rootPos);
		glm::vec3 bendNormal = glm::cross(toTarget, glm::normalize(poleVector - rootPos));
		if (glm::length(bendNormal) < 0.001f)
			bendNormal = glm::vec3(0.0f, 0.0f, 1.0f);
		else
			bendNormal = glm::normalize(bendNormal);

		// 构造根关节与中间关节的四元数旋转
		outRootRot = glm::angleAxis(angleAlpha, bendNormal);
		outMidRot  = glm::angleAxis(kPi - angleBeta, bendNormal);

		return true;
	}

	void FootIK::SolveTerrainPlacement(
		const TerrainData& terrain,
		const glm::vec3& characterPos,
		const glm::vec3& characterForward,
		float footSpacing,
		float& outPelvisYOffset,
		float& outLeftFootYOffset,
		float& outRightFootYOffset,
		glm::vec3& outLeftNormal,
		glm::vec3& outRightNormal)
	{
		const glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), characterForward));

		const glm::vec3 leftFootPos  = characterPos + right * (footSpacing * 0.5f);
		const glm::vec3 rightFootPos = characterPos - right * (footSpacing * 0.5f);

		const float leftH  = terrain.SampleHeight(leftFootPos.x, leftFootPos.z);
		const float rightH = terrain.SampleHeight(rightFootPos.x, rightFootPos.z);

		outLeftNormal  = terrain.SampleNormal(leftFootPos.x, leftFootPos.z);
		outRightNormal = terrain.SampleNormal(rightFootPos.x, rightFootPos.z);

		const float minH = std::min(leftH, rightH);
		const float maxH = std::max(leftH, rightH);

		// 骨盆向下沉降以迎合较低脚
		outPelvisYOffset = std::min(0.0f, minH - characterPos.y);

		// 双足分别向上或向下补偿以踩实斜坡地表
		outLeftFootYOffset  = leftH - (characterPos.y + outPelvisYOffset);
		outRightFootYOffset = rightH - (characterPos.y + outPelvisYOffset);
	}

	void BlendSpace1D::AddSample(float value, const std::string& clipName)
	{
		BlendSample1D sample;
		sample.Value = value;
		sample.ClipName = clipName;
		m_Samples.push_back(sample);

		std::sort(m_Samples.begin(), m_Samples.end(),
			[](const BlendSample1D& a, const BlendSample1D& b) {
				return a.Value < b.Value;
			});
	}

	void BlendSpace1D::Evaluate(float param, size_t& outIdx0, size_t& outIdx1, float& outFactor) const
	{
		if (m_Samples.empty())
		{
			outIdx0 = outIdx1 = 0;
			outFactor = 0.0f;
			return;
		}
		if (m_Samples.size() == 1 || param <= m_Samples.front().Value)
		{
			outIdx0 = outIdx1 = 0;
			outFactor = 0.0f;
			return;
		}
		if (param >= m_Samples.back().Value)
		{
			outIdx0 = outIdx1 = m_Samples.size() - 1;
			outFactor = 0.0f;
			return;
		}

		for (size_t i = 0; i + 1 < m_Samples.size(); ++i)
		{
			if (param >= m_Samples[i].Value && param <= m_Samples[i + 1].Value)
			{
				outIdx0 = i;
				outIdx1 = i + 1;
				const float range = m_Samples[i + 1].Value - m_Samples[i].Value;
				outFactor = (range > 1e-4f) ? ((param - m_Samples[i].Value) / range) : 0.0f;
				return;
			}
		}

		outIdx0 = outIdx1 = 0;
		outFactor = 0.0f;
	}

	void BlendSpace2D::AddSample(const glm::vec2& point, const std::string& clipName)
	{
		BlendSample2D sample;
		sample.Point = point;
		sample.ClipName = clipName;
		m_Samples.push_back(sample);
	}

	void BlendSpace2D::Evaluate(const glm::vec2& param, std::vector<std::pair<size_t, float>>& outWeights) const
	{
		outWeights.clear();
		if (m_Samples.empty())
			return;

		if (m_Samples.size() == 1)
		{
			outWeights.push_back({ 0, 1.0f });
			return;
		}

		// 距离倒数反比加权 (Inverse Distance Weighting)
		float totalWeight = 0.0f;
		for (size_t i = 0; i < m_Samples.size(); ++i)
		{
			const float d = glm::distance(param, m_Samples[i].Point);
			if (d < 1e-4f)
			{
				outWeights.clear();
				outWeights.push_back({ i, 1.0f });
				return;
			}
			const float w = 1.0f / (d * d);
			outWeights.push_back({ i, w });
			totalWeight += w;
		}

		if (totalWeight > 1e-4f)
		{
			for (auto& pair : outWeights)
				pair.second /= totalWeight;
		}
	}

	AnimGraph::AnimGraph()
	{
	}

	void AnimGraph::AddState(const AnimState& state)
	{
		m_States[state.Name] = state;
		if (m_CurrentStateName.empty())
			m_CurrentStateName = state.Name;
	}

	void AnimGraph::SetInitialState(const std::string& stateName)
	{
		m_CurrentStateName = stateName;
		m_StateTime = 0.0f;
		m_InTransition = false;
	}

	void AnimGraph::SetFloat(const std::string& name, float value)
	{
		m_Parameters[name] = value;
	}

	float AnimGraph::GetFloat(const std::string& name, float fallback) const
	{
		auto it = m_Parameters.find(name);
		return (it != m_Parameters.end()) ? it->second : fallback;
	}

	void AnimGraph::SetBool(const std::string& name, bool value)
	{
		m_Parameters[name] = value ? 1.0f : 0.0f;
	}

	bool AnimGraph::GetBool(const std::string& name, bool fallback) const
	{
		auto it = m_Parameters.find(name);
		return (it != m_Parameters.end()) ? (it->second > 0.5f) : fallback;
	}

	void AnimGraph::Update(float deltaSeconds)
	{
		if (m_CurrentStateName.empty())
			return;

		auto itState = m_States.find(m_CurrentStateName);
		if (itState == m_States.end())
			return;

		m_StateTime += deltaSeconds * itState->second.Speed;

		if (m_InTransition)
		{
			m_TransitionElapsed += deltaSeconds;
			m_TransitionTargetTime += deltaSeconds;
			m_TransitionProgress = std::clamp(m_TransitionElapsed / std::max(m_TransitionDuration, 0.001f), 0.0f, 1.0f);

			if (m_TransitionProgress >= 1.0f)
			{
				m_CurrentStateName = m_TransitionTargetState;
				m_StateTime = m_TransitionTargetTime;
				m_InTransition = false;
				m_TransitionProgress = 0.0f;
			}
		}
		else
		{
			// 检查当前状态的触发转换条件
			for (const auto& trans : itState->second.Transitions)
			{
				if (trans.Condition && trans.Condition(m_Parameters))
				{
					m_InTransition = true;
					m_TransitionTargetState = trans.TargetState;
					m_TransitionDuration = trans.Duration;
					m_TransitionElapsed = deltaSeconds;
					m_TransitionTargetTime = deltaSeconds;
					m_TransitionProgress = std::clamp(m_TransitionElapsed / std::max(m_TransitionDuration, 0.001f), 0.0f, 1.0f);
					break;
				}
			}
		}
	}

	void AnimGraph::SampleStatePose(const AnimState& state, float time, const Asset::WModelData& model, AnimPose& outPose) const
	{
		outPose.Joints.clear();
		const size_t nodeCount = model.Nodes.size();
		outPose.Joints.resize(nodeCount);

		for (size_t i = 0; i < nodeCount; ++i)
		{
			outPose.Joints[i].Translation = model.Nodes[i].Translation;
			outPose.Joints[i].Rotation = model.Nodes[i].Rotation;
			outPose.Joints[i].Scale = model.Nodes[i].Scale;
		}

		if (model.Animations.empty())
			return;

		const Asset::WModelAnimation* clip = &model.Animations.front();
		if (!state.ClipName.empty())
		{
			for (const auto& anim : model.Animations)
			{
				if (anim.Name == state.ClipName)
				{
					clip = &anim;
					break;
				}
			}
		}

		// 采样动画通道
		const float duration = clip->Duration;
		const float sampleTime = (duration > 0.0f && state.Loop) ? std::fmod(time, duration) : std::min(time, duration);

		for (const auto& sample : SampleAnimation(*clip, sampleTime))
		{
			if (sample.Node >= outPose.Joints.size())
				continue;

			auto& jt = outPose.Joints[sample.Node];
			if (sample.Path == Asset::WModelIO::WModelAnimationPath::Translation)
			{
				jt.Translation = glm::vec3(sample.Value);
			}
			else if (sample.Path == Asset::WModelIO::WModelAnimationPath::Rotation)
			{
				jt.Rotation = glm::normalize(glm::quat(sample.Value.w, sample.Value.x, sample.Value.y, sample.Value.z));
			}
			else if (sample.Path == Asset::WModelIO::WModelAnimationPath::Scale)
			{
				jt.Scale = glm::vec3(sample.Value);
			}
		}

		// 根骨骼位移提取
		if (!outPose.Joints.empty())
		{
			outPose.RootMotionDelta = outPose.Joints[0].Translation;
			outPose.RootRotationDelta = outPose.Joints[0].Rotation;
		}
	}

	bool AnimGraph::EvaluatePalette(const Asset::WModelData& model, uint32_t skinIndex,
		std::vector<glm::mat4>& outPalette, glm::vec3& outRootMotionDelta)
	{
		outPalette.clear();
		outRootMotionDelta = glm::vec3(0.0f);

		if (m_CurrentStateName.empty() || skinIndex >= model.Skins.size())
			return false;

		auto itState = m_States.find(m_CurrentStateName);
		if (itState == m_States.end())
			return false;

		AnimPose poseA;
		SampleStatePose(itState->second, m_StateTime, model, poseA);

		AnimPose finalPose = poseA;
		if (m_InTransition)
		{
			auto itTarget = m_States.find(m_TransitionTargetState);
			if (itTarget != m_States.end())
			{
				AnimPose poseB;
				SampleStatePose(itTarget->second, m_TransitionTargetTime, model, poseB);
				const float smoothT = SmoothStep(0.0f, 1.0f, m_TransitionProgress);
				finalPose = AnimPose::Blend(poseA, poseB, smoothT);
			}
		}

		outRootMotionDelta = finalPose.RootMotionDelta - m_LastRootPos;
		m_LastRootPos = finalPose.RootMotionDelta;

		// 计算节点全局世界矩阵
		const size_t nodeCount = model.Nodes.size();
		std::vector<glm::mat4> nodeWorldMatrices(nodeCount, glm::mat4(1.0f));

		for (size_t i = 0; i < nodeCount; ++i)
		{
			glm::mat4 local = finalPose.Joints[i].ToMatrix();
			const int32_t parent = model.Nodes[i].Parent;
			nodeWorldMatrices[i] = (parent >= 0 && parent < static_cast<int32_t>(nodeCount))
				? (nodeWorldMatrices[parent] * local)
				: local;
		}

		// 求解 Skin 调色板
		const auto& skin = model.Skins[skinIndex];
		const size_t jointCount = skin.JointNodes.size();
		outPalette.resize(jointCount);

		for (size_t j = 0; j < jointCount; ++j)
		{
			const uint32_t nodeIdx = skin.JointNodes[j];
			const glm::mat4& invBind = (j < skin.InverseBindMatrices.size())
				? skin.InverseBindMatrices[j]
				: glm::mat4(1.0f);

			if (nodeIdx < nodeWorldMatrices.size())
			{
				outPalette[j] = nodeWorldMatrices[nodeIdx] * invBind;
			}
			else
			{
				outPalette[j] = glm::mat4(1.0f);
			}
		}

		return true;
	}

	bool AnimGraph::SaveToFile(const std::string& path) const
	{
		std::ofstream stream(path);
		if (!stream.is_open())
			return false;
		stream << "version: 1\n";
		stream << "initial_state: " << m_CurrentStateName << "\n";
		stream << "states_count: " << m_States.size() << "\n";
		return true;
	}

	bool AnimGraph::LoadFromFile(const std::string& path)
	{
		std::ifstream stream(path);
		return stream.is_open();
	}
}



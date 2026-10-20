#pragma once

#include "World/Asset/WModelIO.h"
#include "World/Core/Export.h"
#include "World/Terrain/Terrain.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace World
{
	struct WLD_API JointTransform
	{
		glm::vec3 Translation { 0.0f };
		glm::quat Rotation { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale { 1.0f };

		glm::mat4 ToMatrix() const;
		static JointTransform Lerp(const JointTransform& a, const JointTransform& b, float factor);
	};

	struct WLD_API AnimPose
	{
		std::vector<JointTransform> Joints;
		glm::vec3 RootMotionDelta { 0.0f };
		glm::quat RootRotationDelta { 1.0f, 0.0f, 0.0f, 0.0f };

		static AnimPose Blend(const AnimPose& a, const AnimPose& b, float factor);
		static AnimPose LayeredBlend(const AnimPose& basePose, const AnimPose& layerPose,
			const std::vector<float>& boneMask, float weight);
	};

	class WLD_API TwoBoneIK
	{
	public:
		// 解析解算器 (余弦定理): 根据目标位置与极向量求解大腿与小腿旋转
		static bool Solve(
			const glm::vec3& rootPos,
			const glm::vec3& midPos,
			const glm::vec3& endPos,
			const glm::vec3& targetPos,
			const glm::vec3& poleVector,
			glm::quat& outRootRot,
			glm::quat& outMidRot);
	};

	class WLD_API FootIK
	{
	public:
		// 联动地貌高度场采样双足自适应高度与骨盆下沉量
		static void SolveTerrainPlacement(
			const TerrainData& terrain,
			const glm::vec3& characterPos,
			const glm::vec3& characterForward,
			float footSpacing,
			float& outPelvisYOffset,
			float& outLeftFootYOffset,
			float& outRightFootYOffset,
			glm::vec3& outLeftNormal,
			glm::vec3& outRightNormal);
	};

	struct WLD_API BlendSample1D
	{
		float Value { 0.0f };
		std::string ClipName;
	};

	class WLD_API BlendSpace1D
	{
	public:
		void AddSample(float value, const std::string& clipName);
		void Clear() { m_Samples.clear(); }
		size_t GetSampleCount() const { return m_Samples.size(); }
		const std::vector<BlendSample1D>& GetSamples() const { return m_Samples; }

		// 计算指定参数下的混合对与权重
		void Evaluate(float param, size_t& outIdx0, size_t& outIdx1, float& outFactor) const;

	private:
		std::vector<BlendSample1D> m_Samples;
	};

	struct WLD_API BlendSample2D
	{
		glm::vec2 Point { 0.0f }; // x=Speed, y=Direction
		std::string ClipName;
	};

	class WLD_API BlendSpace2D
	{
	public:
		void AddSample(const glm::vec2& point, const std::string& clipName);
		void Clear() { m_Samples.clear(); }
		size_t GetSampleCount() const { return m_Samples.size(); }
		const std::vector<BlendSample2D>& GetSamples() const { return m_Samples; }

		void Evaluate(const glm::vec2& param, std::vector<std::pair<size_t, float>>& outWeights) const;

	private:
		std::vector<BlendSample2D> m_Samples;
	};

	enum class AnimStateKind : uint8_t
	{
		SingleClip = 0,
		BlendSpace1D = 1,
		BlendSpace2D = 2
	};

	struct WLD_API AnimTransition
	{
		std::string TargetState;
		float Duration { 0.2f }; // 跨状态平滑混合时间 (秒)
		std::function<bool(const std::unordered_map<std::string, float>&)> Condition;
	};

	struct WLD_API AnimState
	{
		std::string Name;
		AnimStateKind Kind { AnimStateKind::SingleClip };
		std::string ClipName;
		BlendSpace1D Blend1D;
		BlendSpace2D Blend2D;
		float Speed { 1.0f };
		bool Loop { true };

		std::vector<AnimTransition> Transitions;
	};

	class WLD_API AnimGraph
	{
	public:
		AnimGraph();
		~AnimGraph() = default;

		void AddState(const AnimState& state);
		void SetInitialState(const std::string& stateName);

		// 设置控制参数 (如 "Speed", "Direction", "IsFalling")
		void SetFloat(const std::string& name, float value);
		float GetFloat(const std::string& name, float fallback = 0.0f) const;
		void SetBool(const std::string& name, bool value);
		bool GetBool(const std::string& name, bool fallback = false) const;

		// 推进时间并进行状态切换与跨状态姿态融合
		void Update(float deltaSeconds);

		// 采样当前融合后的关节调色板与根骨骼位移
		bool EvaluatePalette(const Asset::WModelData& model, uint32_t skinIndex,
			std::vector<glm::mat4>& outPalette, glm::vec3& outRootMotionDelta);

		const std::string& GetCurrentStateName() const { return m_CurrentStateName; }
		float GetTransitionProgress() const { return m_TransitionProgress; }
		bool IsInTransition() const { return m_InTransition; }

		// 持久化
		bool SaveToFile(const std::string& path) const;
		bool LoadFromFile(const std::string& path);

	private:
		void SampleStatePose(const AnimState& state, float time, const Asset::WModelData& model, AnimPose& outPose) const;

		std::unordered_map<std::string, AnimState> m_States;
		std::string m_CurrentStateName;
		std::string m_TransitionTargetState;

		float m_StateTime { 0.0f };
		float m_TransitionTargetTime { 0.0f };
		float m_TransitionDuration { 0.2f };
		float m_TransitionElapsed { 0.0f };
		float m_TransitionProgress { 0.0f };
		bool m_InTransition { false };

		std::unordered_map<std::string, float> m_Parameters;
		glm::vec3 m_LastRootPos { 0.0f };
	};
}


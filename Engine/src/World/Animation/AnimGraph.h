#pragma once

#include "World/Asset/WModelIO.h"
#include "World/Core/Export.h"
#include "World/Terrain/Terrain.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <map>
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

		// 计算指定参数下的混合对与权重（端点夹取；单样本 → 自身权重 1）
		void Evaluate(float param, size_t& outIdx0, size_t& outIdx1, float& outFactor) const;

	private:
		std::vector<BlendSample1D> m_Samples;
	};

	struct WLD_API BlendSample2D
	{
		glm::vec2 Point { 0.0f }; // x=Speed, y=Direction
		std::string ClipName;
	};

	struct WLD_API BlendTriangle2D
	{
		uint32_t A { 0 };
		uint32_t B { 0 };
		uint32_t C { 0 };
	};

	class WLD_API BlendSpace2D
	{
	public:
		void AddSample(const glm::vec2& point, const std::string& clipName);
		void Clear() { m_Samples.clear(); m_TrianglesDirty = true; }
		size_t GetSampleCount() const { return m_Samples.size(); }
		const std::vector<BlendSample2D>& GetSamples() const { return m_Samples; }

		// Delaunay 三角剖分结果（样本下标三元组）；样本变更后惰性重建。编辑器用它画连线。
		const std::vector<BlendTriangle2D>& GetTriangles() const;

		// 重心坐标混合：命中三角形 3 个样本的 (index, weight)，权重和为 1；
		// 凸包外投影到最近边/点；退化（<3 点或共线）走 1D/最近点；永不输出 NaN。
		void Evaluate(const glm::vec2& param, std::vector<std::pair<size_t, float>>& outWeights) const;

	private:
		void RebuildTriangulation() const;

		std::vector<BlendSample2D> m_Samples;
		mutable std::vector<BlendTriangle2D> m_Triangles;
		mutable bool m_TrianglesDirty { true };
	};

	enum class AnimStateKind : uint8_t
	{
		SingleClip = 0,
		BlendSpace1D = 1,
		BlendSpace2D = 2
	};

	struct WLD_API BoneMaskEntry
	{
		std::string BoneName;
		float Weight { 1.0f };
	};

	struct WLD_API AnimLayer
	{
		std::string Name;
		std::string ClipName;
		std::string WeightParameter;
		std::vector<BoneMaskEntry> BoneMask;
	};

	struct WLD_API AnimTransition
	{
		std::string TargetState;
		float Duration { 0.2f }; // 跨状态平滑混合时间 (秒)
		std::string ConditionExpression;                    // 资产来源
		std::function<bool(const std::unordered_map<std::string, float>&)> Condition; // 代码来源，优先于表达式
	};

	struct WLD_API AnimState
	{
		std::string Name;
		AnimStateKind Kind { AnimStateKind::SingleClip };
		std::string ClipName;
		BlendSpace1D Blend1D;
		BlendSpace2D Blend2D;
		std::string ParameterX { "Speed" };   // blend1d / blend2d
		std::string ParameterY { "Direction" }; // blend2d
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
		const std::unordered_map<std::string, float>& GetParameters() const { return m_Parameters; }

		// 推进时间并进行状态切换与跨状态姿态融合
		void Update(float deltaSeconds);

		// 采样当前融合后的姿态（含 blend spaces + layers），不依赖 skin。
		// outPose.Joints 长度 = model.Nodes.size()；RootMotionDelta/RootRotationDelta 为本帧增量。
		bool EvaluatePose(const Asset::WModelData& model, AnimPose& outPose);

		// 旧接口：EvaluatePose + BuildPalette（不含场景 Foot IK —— 那属于 AnimGraphSystem）。
		bool EvaluatePalette(const Asset::WModelData& model, uint32_t skinIndex,
			std::vector<glm::mat4>& outPalette, glm::vec3& outRootMotionDelta);

		// 给定姿态直接求 skin 调色板（运行时在 Foot IK 之后调用）。
		bool BuildPalette(const Asset::WModelData& model, uint32_t skinIndex,
			const AnimPose& pose, std::vector<glm::mat4>& outPalette) const;

		const std::string& GetCurrentStateName() const { return m_CurrentStateName; }
		float GetTransitionProgress() const { return m_TransitionProgress; }
		float GetStateTime() const { return m_StateTime; }
		const std::string& GetTransitionTargetState() const { return m_TransitionTargetState; }
		bool IsInTransition() const { return m_InTransition; }

		// ---- 资产管理 ----
		void Clear();
		const std::map<std::string, AnimState>& GetStates() const { return m_States; }
		const AnimState* FindState(const std::string& name) const;
		bool RemoveState(const std::string& name);

		const std::vector<AnimLayer>& GetLayers() const { return m_Layers; }
		void SetLayers(std::vector<AnimLayer> layers) { m_Layers = std::move(layers); }
		void AddLayer(const AnimLayer& layer) { m_Layers.push_back(layer); }

		void SetModelHint(const std::string& logicalPath) { m_ModelHint = logicalPath; }
		const std::string& GetModelHint() const { return m_ModelHint; }
		void SetGraphName(const std::string& name) { m_GraphName = name; }
		const std::string& GetGraphName() const { return m_GraphName; }

		// 资产默认参数（构造实例时灌入 m_Parameters）
		void SetParameterDefault(const std::string& name, float value);
		const std::map<std::string, float>& GetParameterDefaults() const { return m_ParameterDefaults; }

		// 持久化
		bool SaveToFile(const std::string& path) const;
		bool LoadFromFile(const std::string& path);
		bool SaveToFile(const std::string& path, std::string* error) const;
		bool LoadFromFile(const std::string& path, std::string* error);

	private:
		void SampleStatePose(const AnimState& state, float time, const Asset::WModelData& model, AnimPose& outPose) const;
		void EnsureTransitionConditions();

		std::map<std::string, AnimState> m_States;
		std::string m_CurrentStateName;
		std::string m_InitialStateName;
		std::string m_TransitionTargetState;

		float m_StateTime { 0.0f };
		float m_TransitionTargetTime { 0.0f };
		float m_TransitionDuration { 0.2f };
		float m_TransitionElapsed { 0.0f };
		float m_TransitionProgress { 0.0f };
		bool m_InTransition { false };

		std::vector<AnimLayer> m_Layers;
		std::string m_ModelHint;
		std::string m_GraphName;
		std::map<std::string, float> m_ParameterDefaults;
		std::unordered_map<std::string, float> m_Parameters;

		// 根运动按“上一帧绝对根 TRS”求增量；EvaluatePose 每帧更新。
		glm::vec3 m_LastRootPos { 0.0f };
		glm::quat m_LastRootRot { 1.0f, 0.0f, 0.0f, 0.0f };
		bool m_HaveRootSample { false };
	};
}

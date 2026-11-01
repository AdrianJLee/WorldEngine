#pragma once

#include "World/Core/Export.h"
#include "World/Level/AI/EQS/EQSTypes.h"
#include "World/Level/AI/EQS/EQSContext.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace World::EQS
{
	class WLD_API IEQSTest
	{
	public:
		virtual ~IEQSTest() = default;
		virtual std::string GetName() const = 0;

		std::shared_ptr<IEQSContext> Context; // 相对谁测试？
		TestPurpose Purpose { TestPurpose::FilterAndScore };
		ScoringCurve Curve { ScoringCurve::Linear };
		float Weight { 1.0f };
		float FilterMin { 0.0f };
		float FilterMax { 1000.0f };
		float IdealValue { 8.0f }; // 高斯曲线最佳中心值
		float GaussianSigma { 1.25f };

		float NormalizeScore(float rawValue) const;
		virtual void RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems) = 0;
	};

	class WLD_API EQSTestRegistry
	{
	public:
		static EQSTestRegistry& Get();
		void Register(const std::string& name, std::shared_ptr<IEQSTest> test);
		std::shared_ptr<IEQSTest> Find(const std::string& name) const;

		template<typename T, typename... Args>
		void RegisterTest(const std::string& name, Args&&... args)
		{
			Register(name, std::make_shared<T>(std::forward<Args>(args)...));
		}

	private:
		EQSTestRegistry();
		std::unordered_map<std::string, std::shared_ptr<IEQSTest>> m_Tests;
	};

	// 1. EQSTest_Distance
	class WLD_API EQSTest_Distance : public IEQSTest
	{
	public:
		bool Use2DDistance { true };
		EQSTest_Distance();
		std::string GetName() const override { return "Test_Distance"; }
		void RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems) override;
	};

	// 2. EQSTest_Trace (视线遮蔽射线检测，双眼高，结合 CoverBox OBB)
	class WLD_API EQSTest_Trace : public IEQSTest
	{
	public:
		bool InvertOcclusion { false }; // false: 遮挡越高分越高 (Cover/Retreat); true: 视野开阔直瞄分高 (Flank)
		EQSTest_Trace();
		std::string GetName() const override { return "Test_Trace"; }
		void RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems) override;
	};

	// 3. EQSTest_Dot (与威胁/目标朝向夹角测试，侧翼夹角 90° 峰值)
	class WLD_API EQSTest_Dot : public IEQSTest
	{
	public:
		bool PeakAtPerpendicular { true }; // 90° 侧翼为峰值 (1.0 - abs(cos))
		EQSTest_Dot();
		std::string GetName() const override { return "Test_Dot"; }
		void RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems) override;
	};

	// 4. EQSTest_Overlap (检测候选点是否落入任何 OBB 障碍物内部，直接硬过滤)
	class WLD_API EQSTest_Overlap : public IEQSTest
	{
	public:
		float ExpandRadius { 0.15f };
		EQSTest_Overlap();
		std::string GetName() const override { return "Test_Overlap"; }
		void RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems) override;
	};

	// 5. EQSTest_Height (高程优势测试)
	class WLD_API EQSTest_Height : public IEQSTest
	{
	public:
		EQSTest_Height();
		std::string GetName() const override { return "Test_Height"; }
		void RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems) override;
	};

	// 6. EQSTest_WallProximity (计算到障碍物表面的贴墙距离，高斯曲线赋予紧贴掩体最佳加分)
	class WLD_API EQSTest_WallProximity : public IEQSTest
	{
	public:
		bool RequireOcclusion { true }; // 仅在被遮挡状态下给贴墙分
		EQSTest_WallProximity();
		std::string GetName() const override { return "Test_WallProximity"; }
		void RunTest(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& inOutItems) override;
	};
}

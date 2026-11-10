#pragma once

#include "World/Core/Export.h"
#include "World/Level/AI/EQS/EQSTypes.h"
#include "World/Level/AI/EQS/EQSGenerator.h"
#include "World/Level/AI/EQS/EQSTest.h"
#include "World/Level/AI/EQS/EQSContext.h"
#include <string>
#include <vector>
#include <memory>

namespace World::EQS
{
	struct WLD_API EQSQueryResult
	{
		bool Success { false };
		glm::vec3 BestPosition { 0.0f };
		float BestScore { -1.0f };
		std::vector<EQSCandidateItem> Items; // 包含单项得分明细与淘汰标记
	};

	class WLD_API EQSQuery
	{
	public:
		std::string Name { "TacticalQuery" };
		std::shared_ptr<IEQSGenerator> Generator;
		std::vector<std::shared_ptr<IEQSTest>> Tests;

		bool Execute(const EQSQueryContext& ctx, EQSQueryResult& outResult);

		// Builder 链式装配器
		class WLD_API Builder
		{
		public:
			Builder();
			Builder& Named(const std::string& name);
			Builder& UsingGenerator(std::shared_ptr<IEQSGenerator> gen);
			Builder& AddTest(std::shared_ptr<IEQSTest> test);
			std::shared_ptr<EQSQuery> Build();
		private:
			std::shared_ptr<EQSQuery> m_Query;
		};
	};
}

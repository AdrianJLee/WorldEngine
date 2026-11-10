#include "wldpch.h"
#include "World/Level/AI/EQS/EQSQuery.h"
#include <algorithm>

namespace World::EQS
{
	bool EQSQuery::Execute(const EQSQueryContext& ctx, EQSQueryResult& outResult)
	{
		outResult.Success = false;
		outResult.BestScore = -1e9f;
		outResult.BestPosition = glm::vec3(0.0f);
		outResult.Items.clear();

		if (!Generator)
			return false;

		Generator->GenerateItems(ctx, outResult.Items);
		if (outResult.Items.empty())
			return false;

		for (auto& test : Tests)
		{
			if (test)
				test->RunTest(ctx, outResult.Items);
		}

		bool found = false;
		float maxScore = -1e9f;
		glm::vec3 bestPos(0.0f);

		for (const auto& item : outResult.Items)
		{
			if (!item.FilteredOut)
			{
				if (!found || item.TotalScore > maxScore)
				{
					maxScore = item.TotalScore;
					bestPos = item.Position;
					found = true;
				}
			}
		}

		outResult.Success = found;
		outResult.BestScore = found ? maxScore : -1.0f;
		outResult.BestPosition = bestPos;
		return found;
	}

	EQSQuery::Builder::Builder()
		: m_Query(std::make_shared<EQSQuery>())
	{
	}

	EQSQuery::Builder& EQSQuery::Builder::Named(const std::string& name)
	{
		m_Query->Name = name;
		return *this;
	}

	EQSQuery::Builder& EQSQuery::Builder::UsingGenerator(std::shared_ptr<IEQSGenerator> gen)
	{
		m_Query->Generator = std::move(gen);
		return *this;
	}

	EQSQuery::Builder& EQSQuery::Builder::AddTest(std::shared_ptr<IEQSTest> test)
	{
		if (test)
			m_Query->Tests.push_back(std::move(test));
		return *this;
	}

	std::shared_ptr<EQSQuery> EQSQuery::Builder::Build()
	{
		auto query = m_Query;
		m_Query = std::make_shared<EQSQuery>();
		return query;
	}
}

#pragma once

#include <mutex>
#include <string>

namespace World::Gen
{
	class BudgetGuard
	{
	public:
		static BudgetGuard& Get();

		void SetMonthlyLimit(double limitUsd);
		double GetMonthlyLimit() const;

		double GetCurrentExpenditure() const;
		void RecordCost(double costUsd);
		void ResetExpenditure();

		bool CanAfford(double estimatedCostUsd, std::string& outReason) const;

		bool SaveToProject(const std::string& projectRoot) const;
		bool LoadFromProject(const std::string& projectRoot);

	private:
		mutable std::mutex m_Mutex;
		double m_MonthlyLimit = 50.0; // 默认 50 美元软上限保护
		double m_CurrentSpent = 0.0;
	};
}

#include "BudgetGuard.h"
#include <sstream>

namespace World::Gen
{
	BudgetGuard& BudgetGuard::Get()
	{
		static BudgetGuard instance;
		return instance;
	}

	void BudgetGuard::SetMonthlyLimit(double limitUsd)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_MonthlyLimit = limitUsd;
	}

	double BudgetGuard::GetMonthlyLimit() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_MonthlyLimit;
	}

	double BudgetGuard::GetCurrentExpenditure() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_CurrentSpent;
	}

	void BudgetGuard::RecordCost(double costUsd)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_CurrentSpent += costUsd;
	}

	void BudgetGuard::ResetExpenditure()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_CurrentSpent = 0.0;
	}

	bool BudgetGuard::CanAfford(double estimatedCostUsd, std::string& outReason) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_CurrentSpent + estimatedCostUsd > m_MonthlyLimit)
		{
			std::ostringstream ss;
			ss << "Budget limit exceeded: estimated cost $" << estimatedCostUsd
			   << " would exceed remaining budget $" << (m_MonthlyLimit - m_CurrentSpent);
			outReason = ss.str();
			return false;
		}
		return true;
	}
}

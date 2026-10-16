#include "BudgetGuard.h"
#include "World/WUI/WuiJson.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace World::Gen
{
	namespace fs = std::filesystem;
	using namespace World::Wui;

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
			ss << "Budget limit exceeded: current spent $" << m_CurrentSpent
			   << " + estimated $" << estimatedCostUsd << " > monthly limit $" << m_MonthlyLimit;
			outReason = ss.str();
			return false;
		}
		return true;
	}

	bool BudgetGuard::SaveToProject(const std::string& projectRoot) const
	{
		if (projectRoot.empty())
			return false;

		const fs::path p = fs::u8path(projectRoot) / ".we" / "ai" / "gen-budget.json";
		std::error_code ec;
		fs::create_directories(p.parent_path(), ec);

		std::ofstream out(p);
		if (!out)
			return false;

		std::lock_guard<std::mutex> lock(m_Mutex);
		out << "{\"monthly_limit\":" << m_MonthlyLimit << ",\"current_spent\":" << m_CurrentSpent << "}\n";
		return true;
	}

	bool BudgetGuard::LoadFromProject(const std::string& projectRoot)
	{
		if (projectRoot.empty())
			return false;

		const fs::path p = fs::u8path(projectRoot) / ".we" / "ai" / "gen-budget.json";
		if (!fs::exists(p))
			return false;

		std::ifstream in(p);
		if (!in)
			return false;

		std::ostringstream ss;
		ss << in.rdbuf();
		auto json = JsonValue::Parse(ss.str(), nullptr);
		if (!json || json->type != JsonValue::Type::Object)
			return false;

		std::lock_guard<std::mutex> lock(m_Mutex);
		if (const auto* lim = json->Find("monthly_limit"))
			m_MonthlyLimit = lim->AsNumber();
		if (const auto* sp = json->Find("current_spent"))
			m_CurrentSpent = sp->AsNumber();

		return true;
	}
}


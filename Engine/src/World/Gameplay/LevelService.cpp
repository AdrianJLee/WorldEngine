#include "wldpch.h"
#include "World/Gameplay/LevelService.h"

#include "World/Core/Log.h"

#include <algorithm>
#include <utility>

namespace World::Gameplay
{
	const char* LevelLoadStateName(LevelLoadState state)
	{
		switch (state)
		{
		case LevelLoadState::Idle:          return "Idle";
		case LevelLoadState::Reading:       return "Reading";
		case LevelLoadState::Deserializing: return "Deserializing";
		case LevelLoadState::Activating:    return "Activating";
		case LevelLoadState::Failed:        return "Failed";
		}
		return "Unknown";
	}

	void LevelService::SetLevelList(LevelList list)
	{
		m_LevelList = std::move(list);
	}

	bool LevelService::IsActive(const std::string& levelId) const
	{
		return std::find(m_ActiveLevelIds.begin(), m_ActiveLevelIds.end(), levelId) != m_ActiveLevelIds.end();
	}

	const std::string& LevelService::GetPrimaryLevel() const
	{
		static const std::string empty;
		return m_ActiveLevelIds.empty() ? empty : m_ActiveLevelIds.front();
	}

	Ref<Scene> LevelService::GetPrimaryScene() const
	{
		return m_ActiveLevels.empty() ? nullptr : m_ActiveLevels.front().Scene_;
	}

	Ref<Scene> LevelService::FindScene(const std::string& levelId) const
	{
		for (const ActiveLevel& level : m_ActiveLevels)
			if (level.Id == levelId)
				return level.Scene_;
		return nullptr;
	}

	bool LevelService::RequestLoad(const std::string& levelId, bool additive)
	{
		if (!m_Loader)
		{
			m_LastError = "LevelService::RequestLoad: no scene loader installed";
			m_State = LevelLoadState::Failed;
			Emit(LevelLoadState::Failed, levelId, 0.0f, m_LastError);
			return false;
		}

		const LevelEntry* entry = m_LevelList.Find(levelId);
		if (!entry)
		{
			m_LastError = "LevelService::RequestLoad: unknown level '" + levelId + "'";
			m_State = LevelLoadState::Failed;
			Emit(LevelLoadState::Failed, levelId, 0.0f, m_LastError);
			return false;
		}
		if (IsLoading())
		{
			m_LastError = "LevelService::RequestLoad: another level is already loading ('" + m_PendingLevelId + "')";
			return false;
		}
		if (!additive && IsActive(levelId) && m_ActiveLevelIds.front() == levelId)
			return false;   // 已经是主关卡,重复请求视为无操作

		m_PendingLevelId = levelId;
		m_PendingAdditive = additive;
		m_State = LevelLoadState::Reading;
		m_LastError.clear();
		Emit(m_State, levelId, 0.0f);
		return true;
	}

	void LevelService::Pump()
	{
		if (m_State != LevelLoadState::Reading)
			return;

		const LevelEntry* entry = m_LevelList.Find(m_PendingLevelId);
		if (!entry)
		{
			m_State = LevelLoadState::Failed;
			m_LastError = "level '" + m_PendingLevelId + "' disappeared from the level list";
			Emit(m_State, m_PendingLevelId, 0.0f, m_LastError);
			return;
		}

		m_State = LevelLoadState::Deserializing;
		Emit(m_State, m_PendingLevelId, 0.4f);

		std::string error;
		Ref<Scene> scene = m_Loader(entry->ScenePath, &error);
		if (!scene)
		{
			m_State = LevelLoadState::Failed;
			m_LastError = error.empty() ? ("failed to load level '" + m_PendingLevelId + "'") : error;
			WLD_CORE_ERROR("LevelService: {0}", m_LastError);
			Emit(m_State, m_PendingLevelId, 0.0f, m_LastError);
			return;
		}

		m_State = LevelLoadState::Activating;
		Emit(m_State, m_PendingLevelId, 0.8f);

		if (!m_PendingAdditive)
		{
			// 非叠加加载 = 替换主关卡:原有附加层先卸载,再替换主关卡。
			while (m_ActiveLevels.size() > 1)
				m_ActiveLevels.pop_back();
			m_ActiveLevels.clear();
			m_ActiveLevelIds.clear();
		}
		m_ActiveLevels.push_back({ m_PendingLevelId, scene });
		m_ActiveLevelIds.push_back(m_PendingLevelId);

		m_State = LevelLoadState::Idle;
		Emit(m_State, m_PendingLevelId, 1.0f);
		WLD_CORE_INFO("LevelService: level '{0}' active ({1} level(s) in stack)",
			m_PendingLevelId, m_ActiveLevels.size());
		m_PendingLevelId.clear();
		m_PendingAdditive = false;
	}

	void LevelService::Unload(const std::string& levelId)
	{
		const auto it = std::find_if(m_ActiveLevels.begin(), m_ActiveLevels.end(),
			[&levelId](const ActiveLevel& level) { return level.Id == levelId; });
		if (it == m_ActiveLevels.end())
			return;

		m_ActiveLevels.erase(it);
		m_ActiveLevelIds.erase(std::remove(m_ActiveLevelIds.begin(), m_ActiveLevelIds.end(), levelId),
			m_ActiveLevelIds.end());
		WLD_CORE_INFO("LevelService: level '{0}' unloaded ({1} level(s) left)", levelId, m_ActiveLevels.size());
	}

	void LevelService::UnloadAll()
	{
		// 逆序卸载(栈顶先走),与加载顺序相反。
		while (!m_ActiveLevels.empty())
		{
			const std::string id = m_ActiveLevels.back().Id;
			m_ActiveLevels.pop_back();
			m_ActiveLevelIds.erase(std::remove(m_ActiveLevelIds.begin(), m_ActiveLevelIds.end(), id),
				m_ActiveLevelIds.end());
		}
		m_State = LevelLoadState::Idle;
		m_PendingLevelId.clear();
		m_PendingAdditive = false;
	}

	void LevelService::Emit(LevelLoadState state, const std::string& levelId, float progress,
		const std::string& error)
	{
		if (!m_ProgressCallback)
			return;
		LevelLoadProgress report;
		report.LevelId = levelId;
		report.State = state;
		report.Progress = progress;
		report.Error = error;
		m_ProgressCallback(report);
	}
}

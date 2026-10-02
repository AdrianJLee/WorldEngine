#include "wldpch.h"
#include "World/Core/WorldContext.h"
#include "World/Schema/Generated/World/WorldSchemaRegistration.h"
#include "World/Utils/DynamicLibrary.h"

#include <algorithm>

namespace World
{
	WorldContext::WorldContext()
	{
		// 内建模块显式注册;不存在静态初始化期注册。
		if (!Schema::RegisterWorldSchemaModule(m_Schemas))
			WLD_CORE_ERROR("WorldContext: built-in World schema module reported a registration error");
	}

	WorldContext::~WorldContext()
	{
		m_Modules.UnloadAll(*this);
		Schema::UnregisterWorldSchemaModule(m_Schemas);
	}

	// ---- PURE-ECS:场景系统挂载钩子(契约见 WorldContext.h)----
	void WorldContext::AddSceneSystemsHook(void (*attach)(Scene&), void (*detach)(Scene&))
	{
		if (attach == nullptr)
			return;
		// 同一对指针重复登记 = no-op:模块被重复 Register(热重载路径)时不叠加。
		const auto existing = std::find_if(m_SceneSystemsHooks.begin(), m_SceneSystemsHooks.end(),
			[attach, detach](const SceneSystemsHook& hook) { return hook.Attach == attach && hook.Detach == detach; });
		if (existing != m_SceneSystemsHooks.end())
			return;
		m_SceneSystemsHooks.push_back({ attach, detach });
	}

	void WorldContext::RemoveSceneSystemsHook(void (*attach)(Scene&), void (*detach)(Scene&))
	{
		m_SceneSystemsHooks.erase(
			std::remove_if(m_SceneSystemsHooks.begin(), m_SceneSystemsHooks.end(),
				[attach, detach](const SceneSystemsHook& hook) { return hook.Attach == attach && hook.Detach == detach; }),
			m_SceneSystemsHooks.end());
	}

	void WorldContext::RunSceneAttachHooks(Scene& scene)
	{
		// 按登记顺序执行;就地遍历(钩子只 register/挂系统,不改钩子表)。
		for (const SceneSystemsHook& hook : m_SceneSystemsHooks)
		{
			if (hook.Attach)
				hook.Attach(scene);
		}
	}

	void WorldContext::RunSceneDetachHooks(Scene& scene)
	{
		// 逆序撤销:与 Attach 对称,后者挂上的系统先摘。
		for (auto it = m_SceneSystemsHooks.rbegin(); it != m_SceneSystemsHooks.rend(); ++it)
		{
			if (it->Detach)
				it->Detach(scene);
		}
	}
}

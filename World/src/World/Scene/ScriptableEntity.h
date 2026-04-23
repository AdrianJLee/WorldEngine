#pragma once
#include "World/Scene/Entity.h"

namespace World
{
	class ScriptableEntity
	{
	public:
		template<typename T>
		T& GetComponent()
		{
			return m_Entity.GetComponent<T>();
		}
	protected:
		virtual void OnCreate() {}
		virtual void OnUpdate(Timestep ts) {}
		virtual void OnDestroy() {}
	private:
		Entity m_Entity;
		friend class Scene;
	};
}



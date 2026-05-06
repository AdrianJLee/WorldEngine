#pragma once
#include "World.h"

namespace World
{
	class StressTest : public ScriptableEntity
	{
		REGISTER_SCRIPT(StressTest);
	public:
		virtual void OnCreate() override
		{
			m_Created.resize(Weight * Height);
			m_Created2.resize(Weight * Height);
		}
		virtual void OnUpdate(Timestep ts) override
		{
			CreateTest2();
			DestroyTest2();
		}
		virtual void OnDestroy() override
		{

		}
	private:
		void CreateTest1();
		void DestroyTest1();

		Entity* Create();
		void CreateTest2();
		void DestroyTest2();
	private:
		int Weight = 100;
		int Height = 100;
		std::vector<Entity> m_Created;
		std::vector<Entity*> m_Created2;
	};
}
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
			CreateTest2();


			StackTest1();
		}
		virtual void OnUpdate(Timestep ts) override
		{
			FrameTest1();

		}
		virtual void OnDestroy() override
		{
			DestroyTest2();
		}
	private:
		void CreateTest1();
		void DestroyTest1();

		Entity* Create();
		void CreateTest2();
		void DestroyTest2();

		void StackTest1();
		void StackTest2();
		void StackTest3();

		void FrameTest1();
		void FrameTest2();
	private:
		int Weight = 100;
		int Height = 100;
		std::vector<Entity> m_Created;
		std::vector<Entity*> m_Created2;
		ScopedStack m_Stack { 1024, "StressTest Stack" };
	};
}
#pragma once
#include "World.h"

namespace World
{


	enum class StressTestType :int
	{
		None = 0,
		Test1,
		Test2,
	};
	REFLECT_ENUM(StressTestType);
	PROPERTY_ENUM(StressTestType, None);
	PROPERTY_ENUM(StressTestType, Test1);
	PROPERTY_ENUM(StressTestType, Test2);

	class StressTest : public ScriptableEntity
	{
		REFLECT_BODY(StressTest, TypeCategory::Script);
	public:

		virtual void OnCreate() override
		{
			m_Created.resize(Weight * Height);
			m_Created2.resize(Weight * Height);
			CreateTest1();


			StackTest1();
		}
		virtual void OnUpdate(Timestep ts) override
		{
			FrameTest1();

		}
		virtual void OnDestroy() override
		{
			DestroyTest1();
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
		PROPERTY(Weight);
		int Weight = 1;
		PROPERTY(Height);
		int Height = 1;
		std::vector<Entity> m_Created;
		std::vector<Entity*> m_Created2;
		ScopedStack m_Stack { 1024, "StressTest Stack" };

		PROPERTY(m_Type);
		StressTestType m_Type = StressTestType::None;
	};
}
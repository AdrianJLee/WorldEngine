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
	WE_ENUM_SCHEMA(Game, StressTestType, Int32)
		WE_ENUM_VALUE(None);
		WE_ENUM_VALUE(Test1);
		WE_ENUM_VALUE(Test2);
	WE_ENUM_END

	class StressTest : public ScriptableEntity
	{
	public:

		virtual void OnCreate() override
		{
			m_Created = std::make_shared<std::vector<Entity>>();
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

		void StackTest1();
		void StackTest2();
		void StackTest3();

		void FrameTest1();
		void FrameTest2();
	private:
		int Weight = 1;
		int Height = 1;
		// The deferred batch owns its result list, never the script instance or a component reference.
		std::shared_ptr<std::vector<Entity>> m_Created = std::make_shared<std::vector<Entity>>();
		ScopedStack m_Stack { 1024, "StressTest Stack" };

		StressTestType m_Type = StressTestType::None;

		WE_SCHEMA_BODY(Game, StressTest, Script)
			WE_FIELD(Weight, Int32);
			WE_FIELD(Height, Int32);
			WE_FIELD(m_Type, Enum, Of(StressTestType));
		WE_SCHEMA_END
	};
}

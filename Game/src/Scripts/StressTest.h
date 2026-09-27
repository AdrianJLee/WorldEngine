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

		// CPPT-2:Asset 属性样例(资产类型名进 TypeName,面板走可搜索资产下拉;逻辑路径字符串)。
		Ref<Texture2D> Icon;

		WE_SCHEMA_BODY(Game, StressTest, Script)
			WE_SCHEMA_META(Category("Scripting/Examples"),
				Doc("Stress fixture: allocates and destroys entities to exercise the scripting lifecycle."))
			WE_FIELD(Weight, Int32, Default(1), Range(0.0f, 128.0f), Step(1.0f),
				Doc("Number of entities created by the first batch (declared default 1)."));
			WE_FIELD(Height, Int32, Default(1), Range(0.0f, 128.0f), Step(1.0f),
				Doc("Stack depth used by the stress fixture (declared default 1)."));
			WE_FIELD(m_Type, Enum, Of(StressTestType),
				Doc("Stress mode selector; stored as an integer and edited through an enum dropdown."));
			WE_FIELD(Icon, Asset, Of("Texture2D"),
				Doc("Asset sample: a texture path edited through the searchable asset dropdown."));
		WE_SCHEMA_END
	};
}

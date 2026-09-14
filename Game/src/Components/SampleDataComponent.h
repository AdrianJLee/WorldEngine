#pragma once

#include "World/Scene/Components.h"

namespace World
{
	// 纯数据示例组件：只做反射声明，不写任何 UI/编辑代码，
	// 由 Editor 的 InspectorRegistry 自动生成控件编辑。
	struct SampleDataComponent
	{
		float Health = 100.0f;

		float Speed = 1.0f;

		bool Enabled = true;

		glm::vec2 Offset { 0.0f, 0.0f };

		int32_t Count = 3;

		float Opacity = 1.0f;

		std::string DisplayName = "Sample";

		std::string Tags;

		WE_SCHEMA_BODY(Game, SampleDataComponent, Component)
			WE_FIELD(Health, Float);
			WE_FIELD(Speed, Float, Range(0.0f, 10.0f));
			WE_FIELD(Enabled, Bool);
			WE_FIELD(Offset, Vec2);
			WE_FIELD(Count, Int32);
			WE_FIELD(Opacity, Float, Range(0.0f, 1.0f));
			WE_FIELD(DisplayName, String, ReadOnly);
			WE_FIELD(Tags, String);
		WE_SCHEMA_END
	};
}

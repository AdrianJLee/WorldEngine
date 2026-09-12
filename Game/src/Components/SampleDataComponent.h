#pragma once

#include "World/Scene/Components.h"

namespace World
{
	// 纯数据示例组件：只做反射声明，不写任何 ImGui/编辑代码，
	// 由 Editor 的 InspectorRegistry 自动生成控件编辑。
	struct SampleDataComponent
	{
		REFLECT_BODY(SampleDataComponent, TypeCategory::Component);

		PROPERTY(Health);
		float Health = 100.0f;

		PROPERTY_RANGE(Speed, 0.0f, 10.0f);
		float Speed = 1.0f;

		PROPERTY(Enabled);
		bool Enabled = true;

		PROPERTY(Offset);
		glm::vec2 Offset { 0.0f, 0.0f };

		PROPERTY(Count);
		int32_t Count = 3;

		PROPERTY_RANGE(Opacity, 0.0f, 1.0f);
		float Opacity = 1.0f;

		PROPERTY_READONLY(DisplayName);
		std::string DisplayName = "Sample";

		PROPERTY(Tags);
		std::string Tags;
	};
}

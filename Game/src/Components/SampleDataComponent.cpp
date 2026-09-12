#include "SampleDataComponent.h"

namespace World
{
	namespace
	{
		// 取成员地址以 odr-use 内联静态注册器，保证组件在 Game.dll 加载时注册进 TypeRegistry。
		const auto s_SampleDataComponentAnchor = &SampleDataComponent::s_AutoRegister;
	}
}

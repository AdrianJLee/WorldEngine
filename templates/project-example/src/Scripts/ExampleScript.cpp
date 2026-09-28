#include "ExampleScript.h"

namespace World
{
	// 示例脚本本体完全在头文件里(成员 + WE_SCHEMA_BODY + 生命周期回调)。
	// 这个 .cpp 只需要出现在构建里,让示例作为一个翻译单元被编译;
	// 真正的"怎么写属性/容器"说明见 ExampleScript.h 顶部注释。
}

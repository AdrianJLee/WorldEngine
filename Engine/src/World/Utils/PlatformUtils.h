#pragma once
#include <string>
namespace World
{
	class FileDialogs
	{
	public:
		static std::string OpenFile(const char* filter);
		static std::string SaveFile(const char* filter);
		// 选择目标文件夹(打包输出等场景);取消返回空串。
		static std::string SelectFolder(const char* title);
	};

	class SystemUtils
	{
	public:
		// 设置指定窗口底层输入法(IME)开关状态
		static void SetIMEState(bool enable, void* windowHandle);
	};
}

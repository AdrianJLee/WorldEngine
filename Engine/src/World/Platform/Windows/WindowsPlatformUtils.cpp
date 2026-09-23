#include "wldpch.h"
#include "World/Utils/PlatformUtils.h"
#include "World/Core/Application.h"

// Windows 相关的头文件
#include <commdlg.h>
#include <shlobj.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

namespace World
{
	std::string FileDialogs::OpenFile(const char* filter)
	{
		// Windows结构体 配置对话框的所有细节
		OPENFILENAMEA ofn;
		// 存放用户选中的文件路径,260为经典路径长度
		CHAR szFile[260] = { 0 };
		// 将结构体内存全部清零。Windows 的旧 API 极其依赖这个动作，否则随机的内存垃圾会导致程序直接崩溃。
		ZeroMemory(&ofn, sizeof(ofn));
		// 版本校验
		ofn.lStructSize = sizeof(ofn);

		ofn.hwndOwner = glfwGetWin32Window((GLFWwindow*)Application::Get().GetWindow().GetNativeWindow());
		// 设置默认路径为当前目录
		ofn.lpstrFile = szFile;
		// 设置文件路径的最大长度
		ofn.nMaxFile = sizeof(szFile);
		ofn.lpstrFilter = filter;
		// 设置默认的过滤器选项，1表示第一个过滤器
		ofn.nFilterIndex = 1;
		// 安全检查标志
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
		// 打开文件对话框
		if (GetOpenFileNameA(&ofn) == TRUE)
		{
			return std::string(ofn.lpstrFile);
		}

		return std::string();
	}

	std::string FileDialogs::SaveFile(const char* filter)
	{
		OPENFILENAMEA ofn;
		CHAR szFile[260] = { 0 };
		ZeroMemory(&ofn, sizeof(ofn));
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = glfwGetWin32Window((GLFWwindow*)Application::Get().GetWindow().GetNativeWindow());
		ofn.lpstrFile = szFile;
		ofn.nMaxFile = sizeof(szFile);
		ofn.lpstrFilter = filter;
		ofn.nFilterIndex = 1;
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
		if (GetSaveFileNameA(&ofn) == TRUE)
		{
			return std::string(ofn.lpstrFile);
		}
		return std::string();
	}

	std::string FileDialogs::SelectFolder(const char* title)
	{
		// 文件夹选择对话框(标准"选取目标目录"体验,不会因同名文件夹而进入其中)。
		BROWSEINFOW info {};
		info.hwndOwner = glfwGetWin32Window((GLFWwindow*)Application::Get().GetWindow().GetNativeWindow());
		info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
		WCHAR titleBuffer[128] = { 0 };
		if (title)
			MultiByteToWideChar(CP_UTF8, 0, title, -1, titleBuffer, 128);
		info.lpszTitle = titleBuffer[0] ? titleBuffer : L"Select folder";

		std::string result;
		PIDLIST_ABSOLUTE selection = SHBrowseForFolderW(&info);
		if (selection)
		{
			WCHAR pathBuffer[MAX_PATH] = { 0 };
			if (SHGetPathFromIDListW(selection, pathBuffer))
			{
				const int length = WideCharToMultiByte(CP_UTF8, 0, pathBuffer, -1, nullptr, 0, nullptr, nullptr);
				if (length > 0)
				{
					result.resize(static_cast<size_t>(length) - 1);
					WideCharToMultiByte(CP_UTF8, 0, pathBuffer, -1, result.data(), length, nullptr, nullptr);
				}
			}
			CoTaskMemFree(selection);
		}
		return result;
	}

	void SystemUtils::SetIMEState(bool enable, void* windowHandle)
	{
		HWND hwnd = (HWND)windowHandle;
		if (!hwnd) return;

		static HIMC s_hImc = NULL;

		if (!enable)
		{
			// 想要禁用输入法时，获取现有输入法上下文并剥离
			HIMC current = ImmGetContext(hwnd);
			if (current != NULL)
			{
				s_hImc = current; // 暂存备份
				ImmAssociateContext(hwnd, NULL); // 取消关联，操作系统将停止抛出输入法弹窗
				ImmReleaseContext(hwnd, current);
			}
		}
		else
		{
			// 想要启用输入法时，将暂存的输入法还给操作系统
			if (s_hImc != NULL)
			{
				ImmAssociateContext(hwnd, s_hImc);
				s_hImc = NULL;
			}
		}
	}
}

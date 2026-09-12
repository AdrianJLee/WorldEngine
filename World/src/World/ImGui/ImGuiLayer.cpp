#include "wldpch.h"
#include "ImGuiLayer.h"

#include "World/Core/Application.h"
#include "World/Utils/PlatformUtils.h"


#include <imgui.h>
#include <ImGuizmo.h>
#include <GLFW/glfw3.h>

#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

namespace World
{
	ImFont* ImGuiLayer::s_BoldFont = nullptr;
	ImFont* ImGuiLayer::s_CjkFont = nullptr;

	ImFont* ImGuiLayer::GetDefaultFont()
	{
		return ImGui::GetIO().FontDefault;
	}

	void ImGuiLayer::ApplyImeState(bool enabled)
	{
		void* windowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		if (windowHandle)
			SystemUtils::SetIMEState(enabled, windowHandle);
	}

	World::ImGuiLayer::ImGuiLayer()
		: Layer("ImGuiLayer"), m_Time(0.0f)
	{}

	World::ImGuiLayer::~ImGuiLayer()
	{

	}


	void ImGuiLayer::OnAttach()
	{
		WLD_PROFILE_FUNCTION();

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;
		io.IniFilename = nullptr; // 布局由 WUI 的 wui-layout.json 管理
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;       // Enable Keyboard Controls
		//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;       // Enable Gamepad Controls

		// Setup Dear ImGui style
		ImGui::StyleColorsDark();
		//ImGui::StyleColorsClassic();



		// Set default ImGui font
		std::string fontPath = WLD_EDITOR_DIR + std::string("assets/fonts/Montserrat/static/Montserrat-Regular.ttf");
		io.FontDefault = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 15.0f);
		//TODO: 需要字体管理器，来加载不同的字体，并且在 ImGui 中切换字体
		// Bold font
		std::string boldFontPath = WLD_EDITOR_DIR + std::string("assets/fonts/Montserrat/static/Montserrat-Bold.ttf");
		s_BoldFont = io.Fonts->AddFontFromFileTTF(boldFontPath.c_str(), 15.0f);

		// CJK 回退字体:中文输入与界面文本(P3 起由 WUI 统一管理,此处先行加载)。
		std::string cjkFontPath = WLD_EDITOR_DIR + std::string("assets/fonts/NotoSansSC/NotoSansSC-Regular.ttf");
		ImFontConfig cjkConfig;
		cjkConfig.MergeMode = false;
		cjkConfig.OversampleH = 1;
		cjkConfig.OversampleV = 1;
		s_CjkFont = io.Fonts->AddFontFromFileTTF(cjkFontPath.c_str(), 16.0f, &cjkConfig, io.Fonts->GetGlyphRangesChineseFull());
		if (!s_CjkFont)
			WLD_CORE_WARN("Failed to load CJK font from {0}", cjkFontPath);

		SetDarkThemeColors();

		Application& app = Application::Get();
		GLFWwindow* window = static_cast<GLFWwindow*>(app.GetWindow().GetNativeWindow());
		// Setup Platform/Renderer bindings
		//将 ImGui 挂载到 GLFW 窗口系统上
		ImGui_ImplGlfw_InitForOpenGL(window, true);// 初始化 GLFW 后端
		//告诉 ImGui 如何利用 OpenGL 绘制图像
		ImGui_ImplOpenGL3_Init("#version 410");// 初始化 OpenGL 后端
	}

	void ImGuiLayer::OnDetach()
	{
		WLD_PROFILE_FUNCTION();

		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
	}
	void ImGuiLayer::OnImGuiRender()
	{
		WLD_PROFILE_FUNCTION();
	}
	void ImGuiLayer::OnEvent(Event& event)
	{
		if (m_BlockEvents)
		{
			ImGuiIO& io = ImGui::GetIO();
			// 如果 ImGui 想要捕获鼠标或键盘事件，那么就把事件标记为已处理，这样事件就不会传递给主程序了
			event.m_Handled |= event.IsInCategory(EventCategoryMouse) & io.WantCaptureMouse;
			event.m_Handled |= event.IsInCategory(EventCategoryKeyboard) & io.WantCaptureKeyboard;
		}
	}

	void ImGuiLayer::Begin()
	{
		WLD_PROFILE_FUNCTION();

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
		ImGuizmo::BeginFrame();
	}
	void ImGuiLayer::End()
	{
		WLD_PROFILE_FUNCTION();

		ImGuiIO& io = ImGui::GetIO();
		Application& app = Application::Get();
		io.DisplaySize = ImVec2((float)app.GetWindow().GetWidth(), (float)app.GetWindow().GetHeight());
		// Rendering
		ImGui::Render();
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

	}

	void ImGuiLayer::SetDarkThemeColors()
	{
		ImGuiStyle& style = ImGui::GetStyle();
		ImVec4* colors = style.Colors;
		colors[ImGuiCol_WindowBg] = ImVec4(0.1f, 0.105f, 0.11f, 1.0f);
		// Headers
		colors[ImGuiCol_Header] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		// Buttons
		colors[ImGuiCol_Button] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		// Frame BG
		colors[ImGuiCol_FrameBg] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		// Tabs
		colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		colors[ImGuiCol_TabHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
		colors[ImGuiCol_TabActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		colors[ImGuiCol_TabUnfocused] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);

		// Title
		colors[ImGuiCol_TitleBg] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
	}
}


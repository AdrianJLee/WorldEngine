#include "World/Core/Core.h"
#include "World/Core/Log.h"
#include "World/Core/Window.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/SceneRenderer.h"
#include "World/WUI/WuiRhiBackend.h"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
	using World::Renderer;
	using World::SceneRenderer;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	// 与 EditorLayer::ProcessPendingRendererChange 相同的重建顺序。
	void SwitchBackend(const char* name)
	{
		WLD_CORE_INFO("[hotswap] shutdown before switch to {0}", name);
		Renderer::Shutdown();
		WLD_CORE_INFO("[hotswap] init {0}", name);
		Renderer::Init(name);
		WLD_CORE_INFO("[hotswap] ready {0}", Renderer::GetBackendName());
	}
}

int main()
{
	World::Log::Init();
	try
	{
		World::WindowProps props("RendererHotswapTest", 640, 360);
		std::unique_ptr<World::Window> window(World::Window::Create(props));
		window->SetVisible(false);

		// 独立窗口 + 每窗口呈现目标:后端切换后宿主持有的 PresentTarget* 必须仍然有效。
		std::unique_ptr<World::Window> auxWindow(World::Window::CreateAuxiliary(
			World::WindowProps("RendererHotswapAux", 320, 200), window.get()));
		auxWindow->SetVisible(false);
		World::PresentTargetDesc auxDesc;
		auxDesc.NativeWindow = auxWindow->GetNativeWindow();
		auxDesc.Width = 320;
		auxDesc.Height = 200;
		auxDesc.DebugName = "HotswapAux";
		World::PresentTarget* auxTarget = Renderer::CreatePresentTarget(auxDesc);
		CHECK(auxTarget != nullptr);

		CHECK(window->GetNativeWindow() != nullptr);
		CHECK(Renderer::GetDevice() == nullptr);

		SceneRenderer sceneRenderer;
		World::Wui::WuiRhiBackend wuiBackend;
		World::Wui::WuiInputState wuiInput;

		// 往返切换 ≥3 次:GL → VK → GL ...
		for (int iteration = 0; iteration < 3; ++iteration)
		{
			SwitchBackend("opengl");
			CHECK(Renderer::GetBackendName() == "opengl");
			WLD_CORE_INFO("[hotswap] it{0}: sceneRenderer.Init (opengl)", iteration + 1);
			sceneRenderer.Init();
			sceneRenderer.OnResize(320, 180);
			WLD_CORE_INFO("[hotswap] it{0}: window update (opengl)", iteration + 1);
			// 主窗口 + 独立窗口各呈现一帧(切换后 PresentTarget* 仍须可用)。
			Renderer::BeginFramePresent(Renderer::MainPresentTarget());
			Renderer::EndFramePresent(Renderer::MainPresentTarget());
			WLD_CORE_INFO("[hotswap] it{0}: aux present (opengl)", iteration + 1);
			Renderer::BeginFramePresent(auxTarget);
			Renderer::EndFramePresent(auxTarget);
			wuiBackend.SetViewportSize({ 640.0f, 360.0f });
			if (wuiBackend.BeginFrame(wuiInput))
			{
				wuiBackend.Render({}, {});
			}
			wuiBackend.EndFrame();
			window->OnUpdate();
			auxWindow->OnUpdate();
			WLD_CORE_INFO("[hotswap] it{0}: sceneRenderer.Shutdown (opengl)", iteration + 1);
			sceneRenderer.Shutdown();

			SwitchBackend("vulkan");
			CHECK(Renderer::GetBackendName() == "vulkan");
			WLD_CORE_INFO("[hotswap] it{0}: sceneRenderer.Init (vulkan)", iteration + 1);
			sceneRenderer.Init();
			sceneRenderer.OnResize(320, 180);
			WLD_CORE_INFO("[hotswap] it{0}: window update (vulkan)", iteration + 1);
			Renderer::BeginFramePresent(Renderer::MainPresentTarget());
			Renderer::EndFramePresent(Renderer::MainPresentTarget());
			WLD_CORE_INFO("[hotswap] it{0}: aux present (vulkan)", iteration + 1);
			Renderer::BeginFramePresent(auxTarget);
			Renderer::EndFramePresent(auxTarget);
			window->OnUpdate();
			auxWindow->OnUpdate();
			WLD_CORE_INFO("[hotswap] it{0}: sceneRenderer.Shutdown (vulkan)", iteration + 1);
			sceneRenderer.Shutdown();

			std::printf("World.RendererHotswap: iteration %d ok (gl<->vk)\n", iteration + 1);
		}

		Renderer::Shutdown();
		std::printf("World.RendererHotswap: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.RendererHotswap: FAILED: %s\n", error.what());
		return 1;
	}
}

#pragma once

#include "World/WUI/WuiBackend.h"
#include "World/WUI/WuiInputCollector.h"
#include "World/RHI/Rhi.h"
#include "World/Renderer/Renderer.h"

#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

struct stbtt_fontinfo;

namespace World::Wui
{
	// Backend v2:WUI 绘制命令经 RHI 2D 批管线呈现(OpenGL 默认帧缓冲 /
	// Vulkan 交换链),输入直接来自 GLFW 事件,不依赖第三方 UI 库。
	class WLD_API WuiRhiBackend final : public WuiBackend
	{
	public:
		WuiRhiBackend();
		~WuiRhiBackend() override;

		// Application::OnEvent 转发的静态入口(进程内只有一个 WUI 宿主)。
		static void FeedKey(uint32_t keyCode, bool down, bool repeat);
		static void FeedChar(uint32_t codepoint);
		static void FeedMouseButton(int button, bool down);
		static void FeedMouseMove(float x, float y);
		static void FeedMouseScroll(float dx, float dy);

		// ---- 独立窗口支持:本实例单独收集输入与视口,不与主窗口共享 ----
		void UseLocalInput(glm::vec2 viewport);
		// P4-UX2f:调用方传的是**物理像素**客户区(独立窗口每帧都会刷新尺寸)。
		// 必须同时更新 m_PhysicalViewport 与设计视口 —— 只写 m_Viewport 会让 RHI 视口
		// 停留在旧尺寸(浮窗 resize 后视口与帧缓冲不匹配,Vulkan 下报错甚至 device lost)。
		void SetViewportSize(glm::vec2 viewport)
		{
			m_PhysicalViewport = viewport;
			const float scale = UiScale() > 0.0f ? UiScale() : 1.0f;
			m_Viewport = viewport / scale;
		}
		void SetCursorWindow(void* nativeWindow) { m_CursorWindow = nativeWindow; }
		void LocalKey(uint32_t keyCode, bool down, bool repeat) { m_LocalInput.OnKey(keyCode, down, repeat); }
		void LocalChar(uint32_t codepoint) { m_LocalInput.OnChar(codepoint); }
		void LocalMouseButton(int button, bool down) { m_LocalInput.OnMouseButton(button, down); }
		void LocalMouseMove(float x, float y) { m_LocalInput.OnMouseMove(x, y); }
		void LocalMouseScroll(float dx, float dy) { m_LocalInput.OnMouseScroll(dx, dy); }

		bool BeginFrame(WuiInputState& input) override;
		void Render(const std::vector<WuiDrawCommand>& commands, const std::vector<WuiDrawCommand>& overlayCommands) override;
		void EndFrame(WuiCursor cursor = WuiCursor::Arrow) override;

		// 宿主销毁前调用:注销"设备释放钩子"并释放本实例持有的 RHI 资源。
		// 钩子捕获 this,若宿主先析构,Renderer::Shutdown 会对已释放对象调用 ReleaseResources()
		// (实测退出码 0xC0000005)。独占各窗口的 WUI 后端必须显式走这一步。
		void ReleaseDeviceResources();

	private:
		struct Glyph
		{
			float X = 0, Y = 0, W = 0, H = 0;   // 图集内像素坐标
			float OffsetX = 0, OffsetY = 0;     // 相对笔尖/基线偏移(基字号)
			float Advance = 0;                  // 基字号下步进
			float PixelSize = 16.0f;            // 烘焙该字形时的像素高度(按字号分桶,避免放大糊)
		};
		struct FontFace
		{
			std::vector<unsigned char> Ttf;
			stbtt_fontinfo* Info = nullptr;
			float BaseSize = 16.0f;
			uint32_t AtlasW = 1024, AtlasH = 1024;
			std::vector<unsigned char> Atlas;
			uint32_t CursorX = 1, CursorY = 1, RowH = 0;
			bool AtlasDirty = false;
			Rhi::Handle<Rhi::Texture> AtlasTexture;
			// 键 = (烘焙像素高度 << 32) | 码点:字号缩放时按目标尺寸重新烘焙,
			// 否则大字号会复用基字号位图被放大成模糊(用户实测"放大字体后有点模糊")。
			std::unordered_map<uint64_t, Glyph> Glyphs;
		};

		void EnsureResources();
		void ReleaseResources();
		void DrawList(const std::vector<WuiDrawCommand>& commands);
		void PushQuad(const WuiRect& rect, const WuiColor& color, const WuiRect& uv);
		// 任意四边形(D7-D4):用于斜线/箭头/圆环等轴对齐矩形画不出的形状。
		void PushQuadVertices(const std::array<glm::vec2, 4>& positions, const WuiColor& color);
		void PushSolidQuad(const WuiRect& rect, const WuiColor& color);
		void SetActiveTexture(const Rhi::Handle<Rhi::Texture>& texture);
		void Flush();
		void ApplyScissor(const WuiRect& rect);
		Rhi::Handle<Rhi::DescriptorSet> TextureSetFor(const Rhi::Handle<Rhi::Texture>& texture);
		// 内容代变化时丢弃"按纹理缓存"的描述符集。Vulkan 下每个描述符集自带一个
		// descriptor pool,立即析构会在旧命令缓冲仍在飞时触发
		// VUID-vkDestroyDescriptorPool-descriptorPool-00303,因此统一走
		// Renderer::QueueRelease 延迟到该槽位 GPU 工作完成之后。
		void ReleaseStaleTextureSets();

		// 字体面索引(P4-UX3 起):0 = Inter Regular(缺失回落 Montserrat),1 = Inter Bold(同),
		// 2 = NotoSansSC 子集(CJK 回落),3 = JetBrains Mono Regular,4 = JetBrains Mono Bold。
		FontFace* PrimaryFace(WuiFontFamily family, bool bold);
		// 逐码点选面:主面缺该字形时回落 Noto('\t'/'\r'/'\n' 一律走主面,advance 特判)。
		FontFace* FaceForCodepoint(WuiFontFamily family, bool bold, uint32_t codepoint);
		Glyph& Bake(FontFace& face, uint32_t codepoint, float pixelSize);
		float AdvanceOf(FontFace* face, uint32_t codepoint, float fontSize);
		// 逐码点选面的度量;byteOffset >= 0 时只累计 [0, byteOffset) 的宽度。
		float MeasureText(std::string_view text, float fontSize, WuiFontFamily family, bool bold, int byteOffset = -1);
		void DrawText(const WuiDrawCommand& command);
		void DrawRectCommand(const WuiDrawCommand& command, bool outline);
		void DrawImageCommand(const WuiDrawCommand& command);

		static WuiInputCollector s_Input;
		WuiInputCollector m_LocalInput;
		bool m_UseLocalInput = false;
		void* m_CursorWindow = nullptr;

		// 设计单位视口(= 物理像素 / UiScale)用于布局与投影;物理视口用于 RHI 视口/裁剪。
		glm::vec2 m_Viewport {};
		glm::vec2 m_PhysicalViewport {};
		float m_Fps = 0;
		double m_LastTime = -1;
		void* m_DeviceKey = nullptr;

		// 帧深 2:命令缓冲/UBO/全局描述符集/纹理描述符集/顶点索引缓冲都按帧槽位环形。
		// 与 Renderer::FramesInFlight 同源(见 Renderer.h 的说明)。
		static constexpr uint32_t kFramesInFlight = Renderer::FramesInFlight;
		uint32_t FrameSlot() const;
		uint64_t SlotVertexBase() const;
		uint64_t SlotIndexBase() const;

		Rhi::Handle<Rhi::CommandBuffer> m_Cmds[kFramesInFlight];
		Rhi::Handle<Rhi::Shader> m_Shader;
		Rhi::Handle<Rhi::Pipeline> m_Pipeline;
		Rhi::Handle<Rhi::Buffer> m_Vbs[kFramesInFlight], m_Ibs[kFramesInFlight], m_Ubos[kFramesInFlight];
		Rhi::Handle<Rhi::DescriptorSetLayout> m_TextureLayout;
		Rhi::Handle<Rhi::DescriptorSet> m_GlobalSets[kFramesInFlight];
		Rhi::Handle<Rhi::Sampler> m_Sampler;
		Rhi::Handle<Rhi::Texture> m_WhiteTexture;
		Rhi::Handle<Rhi::RenderPass> m_UiPass;
		Rhi::Handle<Rhi::Texture> m_UiColor;
		Rhi::Handle<Rhi::Framebuffer> m_UiFramebuffer;
		uint32_t m_UiWidth = 0, m_UiHeight = 0;
		std::vector<FontFace> m_Faces;
		glm::mat4 m_Projection { 1.0f };

		struct Vertex { float X, Y; float R, G, B, A; float U, V; };
		std::vector<Vertex> m_Vertices;
		std::vector<uint32_t> m_Indices;
		Rhi::Handle<Rhi::Texture> m_ActiveTexture;
		bool m_TextureChanged = true;
		std::vector<WuiRect> m_ClipStack;
		WuiRect m_CurrentClip {};
		bool m_IsVulkan = false;
		// 每帧为每个批次分配独立的缓冲区区间:同一命令缓冲里的所有绘制
		// 在提交后才执行,若各批次复用同一段数据,GPU 只能看到最后一批。
		uint64_t m_FrameVertexBytes = 0;
		uint64_t m_FrameIndexBytes = 0;
		// 每个纹理一份描述符集:一个描述符集在一帧内被多次改写时,GPU 执行
		// 整条命令缓冲只能看到最后一次写入,导致除最后一张外的贴图全部采样错误。
		std::unordered_map<const void*, Rhi::Handle<Rhi::DescriptorSet>> m_TextureSets[kFramesInFlight];
		// 见构造函数/EnsureResources:描述符集缓存按纹理对象地址做键,地址会被复用,
		// 因此用注册表的内容代(任何"真的换了句柄"都 +1)失效缓存。
		uint32_t m_TextureContentRevision = 0;
	};
}

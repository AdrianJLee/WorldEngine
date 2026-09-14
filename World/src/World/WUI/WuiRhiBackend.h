#pragma once

#include "World/WUI/WuiBackend.h"
#include "World/WUI/WuiInputCollector.h"
#include "World/RHI/Rhi.h"

#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

struct stbtt_fontinfo;

namespace World::Wui
{
	// Backend v2:WUI 绘制命令经 RHI 2D 批管线呈现(OpenGL 默认帧缓冲 /
	// Vulkan 交换链),输入直接来自 GLFW 事件,不依赖 ImGui。
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

		bool BeginFrame(WuiInputState& input) override;
		void Render(const std::vector<WuiDrawCommand>& commands, const std::vector<WuiDrawCommand>& overlayCommands) override;
		void EndFrame(WuiCursor cursor = WuiCursor::Arrow) override;

	private:
		struct Glyph
		{
			float X = 0, Y = 0, W = 0, H = 0;   // 图集内像素坐标
			float OffsetX = 0, OffsetY = 0;     // 相对笔尖/基线偏移(基字号)
			float Advance = 0;                  // 基字号下步进
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
			std::unordered_map<uint32_t, Glyph> Glyphs;
		};

		void EnsureResources();
		void ReleaseResources();
		void DrawList(const std::vector<WuiDrawCommand>& commands);
		void PushQuad(const WuiRect& rect, const WuiColor& color, const WuiRect& uv);
		void PushSolidQuad(const WuiRect& rect, const WuiColor& color);
		void SetActiveTexture(const Rhi::Handle<Rhi::Texture>& texture);
		void Flush();
		void ApplyScissor(const WuiRect& rect);
		Rhi::Handle<Rhi::DescriptorSet> TextureSetFor(const Rhi::Handle<Rhi::Texture>& texture);

		FontFace& FaceFor(const std::string& text, bool bold);
		Glyph& Bake(FontFace& face, uint32_t codepoint);
		float Measure(FontFace& face, const std::string& text, float fontSize, int byteOffset = -1);
		float AdvanceOf(FontFace& face, uint32_t codepoint, float fontSize);
		void DrawText(const WuiDrawCommand& command);
		void DrawRectCommand(const WuiDrawCommand& command, bool outline);
		void DrawImageCommand(const WuiDrawCommand& command);

		static WuiInputCollector s_Input;

		glm::vec2 m_Viewport {};
		float m_Fps = 0;
		double m_LastTime = -1;
		void* m_DeviceKey = nullptr;

		Rhi::Handle<Rhi::CommandBuffer> m_Cmd;
		Rhi::Handle<Rhi::Shader> m_Shader;
		Rhi::Handle<Rhi::Pipeline> m_Pipeline;
		Rhi::Handle<Rhi::Buffer> m_Vb, m_Ib, m_Ubo;
		Rhi::Handle<Rhi::DescriptorSetLayout> m_TextureLayout;
		Rhi::Handle<Rhi::DescriptorSet> m_GlobalSet;
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
		std::unordered_map<const void*, Rhi::Handle<Rhi::DescriptorSet>> m_TextureSets;
		uint32_t m_TextureGeneration = 0;
	};
}

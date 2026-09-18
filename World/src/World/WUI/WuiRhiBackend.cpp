#include "wldpch.h"
#include "WuiRhiBackend.h"

#include "World/Core/Application.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/ShaderUtils.h"
#include "World/RHI/RhiTextureBridge.h"
#include "WuiTextureRegistry.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <GLFW/glfw3.h>


#include <algorithm>
#include <cmath>
#include <fstream>

namespace World::Wui
{
	WuiInputCollector WuiRhiBackend::s_Input;

	namespace
	{
		constexpr uint32_t MaxQuads = 20000;

		void DecodeUtf8(std::string_view text, std::vector<uint32_t>& codepoints, std::vector<int>& byteOffsets)
		{
			codepoints.clear();
			byteOffsets.clear();
			size_t i = 0;
			while (i < text.size())
			{
				const int start = static_cast<int>(i);
				const unsigned char c = static_cast<unsigned char>(text[i]);
				uint32_t cp = 0;
				if (c < 0x80) { cp = c; i += 1; }
				else if ((c >> 5) == 0x6 && i + 1 < text.size()) { cp = ((c & 0x1Fu) << 6) | (text[i + 1] & 0x3Fu); i += 2; }
				else if ((c >> 4) == 0xE && i + 2 < text.size()) { cp = ((c & 0x0Fu) << 12) | ((text[i + 1] & 0x3Fu) << 6) | (text[i + 2] & 0x3Fu); i += 3; }
				else if ((c >> 3) == 0x1E && i + 3 < text.size()) { cp = ((c & 0x07u) << 18) | ((text[i + 1] & 0x3Fu) << 12) | ((text[i + 2] & 0x3Fu) << 6) | (text[i + 3] & 0x3Fu); i += 4; }
				else { cp = 0xFFFD; i += 1; }
				codepoints.push_back(cp);
				byteOffsets.push_back(start);
			}
		}

		bool LoadFile(const std::filesystem::path& path, std::vector<unsigned char>& bytes)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return false;
			stream.seekg(0, std::ios::end);
			const std::streamsize size = stream.tellg();
			stream.seekg(0, std::ios::beg);
			if (size <= 0)
				return false;
			bytes.resize(static_cast<size_t>(size));
			stream.read(reinterpret_cast<char*>(bytes.data()), size);
			return stream.good();
		}
	}

	WuiRhiBackend::WuiRhiBackend()
	{
		m_Faces.resize(5);
		// 真实字形度量钩子:WuiContext::MeasureTextWidth / WuiCodeEditor 的命中测试、
		// 行宽与 caret 定位都走这里;headless 测试可注入假度量。
		SetTextMeasureHook(this, [this](std::string_view text, float fontSize, WuiFontFamily family)
		{
			return MeasureText(text, fontSize, family, false);
		});
	}

	WuiRhiBackend::~WuiRhiBackend()
	{
		ReleaseDeviceResources();
		ClearTextMeasureHook(this);
		for (FontFace& face : m_Faces)
			delete face.Info;
	}

	void WuiRhiBackend::ReleaseDeviceResources()
	{
		// 先摘钩子再放资源:钩子是 [this] 的 lambda,留在 Renderer 的钩子表里会在
		// Renderer::Shutdown(设备销毁前)对已析构的后端调用 ReleaseResources()。
		Renderer::UnregisterDeviceReleaseHook(this);
		ReleaseResources();
	}

	void WuiRhiBackend::FeedKey(uint32_t keyCode, bool down, bool repeat) { s_Input.OnKey(keyCode, down, repeat); }
	void WuiRhiBackend::FeedChar(uint32_t codepoint) { s_Input.OnChar(codepoint); }
	void WuiRhiBackend::FeedMouseButton(int button, bool down) { s_Input.OnMouseButton(button, down); }
	void WuiRhiBackend::FeedMouseMove(float x, float y) { s_Input.OnMouseMove(x, y); }
	void WuiRhiBackend::FeedMouseScroll(float dx, float dy) { s_Input.OnMouseScroll(dx, dy); }

	bool WuiRhiBackend::BeginFrame(WuiInputState& input)
	{
		if (Application::HasInstance() && !m_UseLocalInput)
		{
			m_Viewport = { static_cast<float>(Application::Get().GetWindow().GetWidth()),
				static_cast<float>(Application::Get().GetWindow().GetHeight()) };
		}
		const double now = glfwGetTime();
		if (m_LastTime > 0 && now > m_LastTime)
			m_Fps = static_cast<float>(1.0 / (now - m_LastTime));
		m_LastTime = now;
		if (m_UseLocalInput)
			m_LocalInput.BeginFrame(input, m_Viewport, m_Fps);
		else
			s_Input.BeginFrame(input, m_Viewport, m_Fps);
		return true;
	}

	void WuiRhiBackend::UseLocalInput(glm::vec2 viewport)
	{
		m_UseLocalInput = true;
		m_Viewport = viewport;
		m_CursorWindow = nullptr;
	}

	void WuiRhiBackend::ReleaseResources()
	{
		for (uint32_t slot = 0; slot < kFramesInFlight; ++slot)
		{
			m_Cmds[slot] = nullptr;
			m_Vbs[slot] = nullptr;
			m_Ibs[slot] = nullptr;
			m_Ubos[slot] = nullptr;
			m_GlobalSets[slot] = nullptr;
			m_TextureSets[slot].clear();
		}
		m_Shader = nullptr;
		m_Pipeline = nullptr;
		m_TextureLayout = nullptr;
		m_Sampler = nullptr;
		m_WhiteTexture = nullptr;
		m_UiPass = nullptr;
		m_UiColor = nullptr;
		m_UiFramebuffer = nullptr;
		m_UiWidth = m_UiHeight = 0;
		m_ActiveTexture = nullptr;
		for (FontFace& face : m_Faces)
			face.AtlasTexture = nullptr;
		++m_TextureGeneration;
		m_TextureChanged = true;
		m_DeviceKey = nullptr;
	}

	void WuiRhiBackend::EnsureResources()
	{
		const Rhi::Handle<Rhi::Device>& device = Renderer::GetDevice();
		if (!device)
			return;
		if (device.get() == m_DeviceKey && m_Pipeline)
			return;
		// 设备销毁前主动释放:句柄析构必须发生在设备仍存活时。
		Renderer::RegisterDeviceReleaseHook(this, [this] { ReleaseResources(); });
		ReleaseResources();
		m_DeviceKey = device.get();
		m_IsVulkan = Renderer::GetBackendName() == "vulkan";

		for (uint32_t slot = 0; slot < kFramesInFlight; ++slot)
			m_Cmds[slot] = device->CreateCommandBuffer("WuiBackend");

		Rhi::ShaderDesc shaderDesc;
		shaderDesc.DebugName = "WUI";
		shaderDesc.Stages.push_back(ShaderCompiler::CompileStage(
			Rhi::ShaderStage::Vertex, "assets/shaders/Wui_Ui.hlsl", "VSMain", "vs_6_0"));
		shaderDesc.Stages.push_back(ShaderCompiler::CompileStage(
			Rhi::ShaderStage::Fragment, "assets/shaders/Wui_Ui.hlsl", "PSMain", "ps_6_0"));
		m_Shader = device->CreateShader(shaderDesc);

		Rhi::DescriptorSetLayoutDesc textureLayoutDesc;
		// 着色器用 [[vk::combinedImageSampler]] 声明合并采样器,SPIR-V 与 GLSL 交叉
		// 编译产物统一为 binding 1 的 combined image sampler。
		textureLayoutDesc.Bindings.push_back({ 1, Rhi::DescriptorType::CombinedImageSampler,
			Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), 1 });
		m_TextureLayout = device->CreateDescriptorSetLayout(textureLayoutDesc);
		for (uint32_t slot = 0; slot < kFramesInFlight; ++slot)
			m_GlobalSets[slot] = device->CreateDescriptorSet(Renderer::GetGlobalDescriptorSetLayout());

		Rhi::SamplerDesc samplerDesc;
		samplerDesc.MinFilter = Rhi::Filter::Linear;
		samplerDesc.MagFilter = Rhi::Filter::Linear;
		samplerDesc.AddressU = Rhi::SamplerAddressMode::ClampToEdge;
		samplerDesc.AddressV = Rhi::SamplerAddressMode::ClampToEdge;
		m_Sampler = device->CreateSampler(samplerDesc);

		Rhi::RenderPassDesc passDesc;
		Rhi::RenderPassAttachment colorAttachment;
		colorAttachment.Format = m_IsVulkan ? Rhi::Format::B8G8R8A8_UNORM : Rhi::Format::R8G8B8A8_UNORM;
		colorAttachment.Samples = Rhi::SampleCount::Count1;
		// 该通道仅用于 OpenGL 离屏层(Vulkan 直接渲染到呈现通道):
		// 每帧清除,避免 Load + Undefined 初始布局的非法组合。
		colorAttachment.Load = Rhi::LoadOp::Clear;
		colorAttachment.Store = Rhi::StoreOp::Store;
		colorAttachment.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
		colorAttachment.FinalLayout = m_IsVulkan ? Rhi::AttachmentLayout::Present : Rhi::AttachmentLayout::ShaderReadOnly;
		passDesc.Attachments = { colorAttachment };
		Rhi::SubpassDesc subpass;
		subpass.ColorAttachments = { { 0, Rhi::AttachmentLayout::ColorAttachment } };
		passDesc.Subpasses = { subpass };
		passDesc.DebugName = "WuiTarget";
		m_UiPass = device->CreateRenderPass(passDesc);

		Rhi::PipelineDesc pipelineDesc;
		pipelineDesc.Shader = m_Shader;
		pipelineDesc.RenderPass = m_IsVulkan ? Renderer::GetPresentRenderPass() : m_UiPass;
		pipelineDesc.DescriptorSetLayouts = { Renderer::GetGlobalDescriptorSetLayout(), m_TextureLayout };
		pipelineDesc.Topology = Rhi::PrimitiveTopology::TriangleList;
		pipelineDesc.Cull = Rhi::CullMode::None;
		pipelineDesc.VertexBindings.push_back({ 0, sizeof(Vertex), false });
		pipelineDesc.VertexAttributes = {
			{ 0, 0, Rhi::Format::R32G32_SFLOAT, 0 },
			{ 1, 0, Rhi::Format::R32G32B32A32_SFLOAT, 8 },
			{ 2, 0, Rhi::Format::R32G32_SFLOAT, 24 },
		};
		pipelineDesc.Blends.push_back({
			true,
			Rhi::BlendFactor::SrcAlpha, Rhi::BlendFactor::OneMinusSrcAlpha, Rhi::BlendOp::Add,
			Rhi::BlendFactor::SrcAlpha, Rhi::BlendFactor::OneMinusSrcAlpha, Rhi::BlendOp::Add,
			0xF });
		pipelineDesc.DebugName = "WUI";
		m_Pipeline = device->CreatePipeline(pipelineDesc);

		Rhi::BufferDesc vertexDesc;
		// 按帧槽位分段:每个槽位一段,保证帧 N+1 的上传不覆盖帧 N 仍在读的数据。
		vertexDesc.Size = static_cast<uint64_t>(MaxQuads) * kFramesInFlight * 4 * sizeof(Vertex);
		vertexDesc.Usage = Rhi::BufferUsageVertex;
		vertexDesc.Memory = Rhi::MemoryHint::HostVisible;
		Rhi::BufferDesc indexDesc;
		indexDesc.Size = static_cast<uint64_t>(MaxQuads) * kFramesInFlight * 6 * sizeof(uint32_t);
		indexDesc.Usage = Rhi::BufferUsageIndex;
		indexDesc.Memory = Rhi::MemoryHint::HostVisible;
		Rhi::BufferDesc uboDesc;
		uboDesc.Size = sizeof(glm::mat4);
		uboDesc.Usage = Rhi::BufferUsageUniform;
		uboDesc.Memory = Rhi::MemoryHint::HostVisible;
		for (uint32_t slot = 0; slot < kFramesInFlight; ++slot)
		{
			m_Vbs[slot] = device->CreateBuffer(vertexDesc);
			m_Ibs[slot] = device->CreateBuffer(indexDesc);
			m_Ubos[slot] = device->CreateBuffer(uboDesc);
			Rhi::DescriptorWrite globalWrite;
			globalWrite.Binding = 0;
			globalWrite.Type = Rhi::DescriptorType::UniformBuffer;
			globalWrite.Buffer = m_Ubos[slot];
			m_GlobalSets[slot]->Update({ globalWrite });
		}

		Rhi::TextureDesc whiteDesc;
		whiteDesc.Type = Rhi::TextureType::Texture2D;
		whiteDesc.Format = Rhi::Format::R8G8B8A8_UNORM;
		whiteDesc.Extent = { 1, 1, 1 };
		whiteDesc.Usage = Rhi::TextureUsageSampled;
		m_WhiteTexture = device->CreateTexture(whiteDesc);
		const unsigned char white[4] = { 255, 255, 255, 255 };
		m_WhiteTexture->SetData(white, 4);

		const std::string fontPaths[5] = {
			std::string(WLD_EDITOR_DIR) + "assets/fonts/Montserrat/static/Montserrat-Regular.ttf",
			std::string(WLD_EDITOR_DIR) + "assets/fonts/Montserrat/static/Montserrat-Bold.ttf",
			std::string(WLD_EDITOR_DIR) + "assets/fonts/NotoSansSC/NotoSansSC-Subset.ttf",
			std::string(WLD_EDITOR_DIR) + "assets/fonts/JetBrainsMono/JetBrainsMono-Regular.ttf",
			std::string(WLD_EDITOR_DIR) + "assets/fonts/JetBrainsMono/JetBrainsMono-Bold.ttf",
		};
		for (size_t i = 0; i < m_Faces.size(); ++i)
		{
			FontFace& face = m_Faces[i];
			face.Glyphs.clear();
			if (!face.Info)
				face.Info = new stbtt_fontinfo;
			if (!LoadFile(fontPaths[i], face.Ttf) || !stbtt_InitFont(face.Info, face.Ttf.data(),
				stbtt_GetFontOffsetForIndex(face.Ttf.data(), 0)))
			{
				WLD_CORE_WARN("WUI backend failed to load font {0}", fontPaths[i]);
				continue;
			}
			face.Atlas.assign(static_cast<size_t>(face.AtlasW) * face.AtlasH * 4, 0);
			Rhi::TextureDesc atlasDesc;
			atlasDesc.Type = Rhi::TextureType::Texture2D;
			atlasDesc.Format = Rhi::Format::R8G8B8A8_UNORM;
			atlasDesc.Extent = { face.AtlasW, face.AtlasH, 1 };
			atlasDesc.Usage = Rhi::TextureUsageSampled;
			face.AtlasTexture = device->CreateTexture(atlasDesc);
		}
		m_Vertices.clear();
		m_Indices.clear();
		m_ActiveTexture = m_WhiteTexture;
		m_TextureChanged = true;
	}

	WuiRhiBackend::FontFace* WuiRhiBackend::PrimaryFace(WuiFontFamily family, bool bold)
	{
		if (m_Faces.size() < 5)
			return nullptr;
		if (family == WuiFontFamily::Monospace)
			return &m_Faces[bold ? 4 : 3];
		return &m_Faces[bold ? 1 : 0];
	}

	WuiRhiBackend::FontFace* WuiRhiBackend::FaceForCodepoint(WuiFontFamily family, bool bold, uint32_t codepoint)
	{
		FontFace* primary = PrimaryFace(family, bold);
		// '\t' 的 advance 是特判的(4 空格),'\r'/'\n' 不可见:一律走主面。
		if (codepoint == '\t' || codepoint == '\r' || codepoint == '\n' || codepoint == 0)
			return primary;
		// 主面缺该字形时回落 Noto(CJK、以及 JetBrains Mono 未覆盖的码位)。
		if (primary && primary->Info && stbtt_FindGlyphIndex(primary->Info, static_cast<int>(codepoint)) != 0)
			return primary;
		FontFace* fallback = m_Faces.size() > 2 ? &m_Faces[2] : nullptr;
		if (fallback && fallback->Info)
			return fallback;
		return primary ? primary : fallback;
	}

	WuiRhiBackend::Glyph& WuiRhiBackend::Bake(FontFace& face, uint32_t codepoint, float pixelSize)
	{
		// 字号按整数分桶:缓存数量可控,误差 <0.5px 的缩放肉眼看不出。
		const uint32_t sizeKey = static_cast<uint32_t>(std::max(6.0f, std::round(pixelSize)));
		const uint64_t key = (static_cast<uint64_t>(sizeKey) << 32) | codepoint;
		auto it = face.Glyphs.find(key);
		if (it != face.Glyphs.end())
			return it->second;
		Glyph glyph;
		glyph.PixelSize = static_cast<float>(sizeKey);
		const float scale = stbtt_ScaleForPixelHeight(face.Info, glyph.PixelSize);
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		stbtt_GetCodepointBitmapBox(face.Info, codepoint, scale, scale, &x0, &y0, &x1, &y1);
		const int width = x1 - x0;
		const int height = y1 - y0;
		if (width > 0 && height > 0)
		{
			if (face.CursorX + width + 1 >= face.AtlasW)
			{
				face.CursorX = 1;
				face.CursorY += face.RowH + 1;
				face.RowH = 0;
			}
			if (face.CursorY + height + 1 < face.AtlasH)
			{
				std::vector<unsigned char> bitmap(static_cast<size_t>(width) * height);
				stbtt_MakeCodepointBitmap(face.Info, bitmap.data(), width, height, width, scale, scale, codepoint);
				for (int row = 0; row < height; ++row)
					for (int col = 0; col < width; ++col)
					{
						const size_t dst = (static_cast<size_t>(face.CursorY + row) * face.AtlasW + face.CursorX + col) * 4;
						const unsigned char v = bitmap[static_cast<size_t>(row) * width + col];
						face.Atlas[dst] = 255;
						face.Atlas[dst + 1] = 255;
						face.Atlas[dst + 2] = 255;
						face.Atlas[dst + 3] = v;
					}
				glyph.X = static_cast<float>(face.CursorX);
				glyph.Y = static_cast<float>(face.CursorY);
				glyph.W = static_cast<float>(width);
				glyph.H = static_cast<float>(height);
				face.CursorX += width + 1;
				face.RowH = std::max(face.RowH, static_cast<uint32_t>(height));
				face.AtlasDirty = true;
			}
		}
		if (glyph.W <= 0.0f || glyph.H <= 0.0f)
		{
			// 图集满(或该码点无位图):回退到同码点已缓存的任意字号,宁可轻微缩放也不要缺字。
			for (auto& entry : face.Glyphs)
				if ((entry.first & 0xFFFFFFFFull) == codepoint)
					return entry.second;
			if (width > 0 && height > 0)
				WLD_CORE_WARN("[wui-font] glyph atlas full for U+{0:X} at {1}px (size buckets too many?)",
					codepoint, sizeKey);
		}
		glyph.OffsetX = static_cast<float>(x0);
		glyph.OffsetY = static_cast<float>(y0);
		int advance = 0;
		stbtt_GetCodepointHMetrics(face.Info, codepoint, &advance, nullptr);
		glyph.Advance = advance * scale;
		const auto result = face.Glyphs.emplace(key, glyph);
		return result.first->second;
	}

	float WuiRhiBackend::AdvanceOf(FontFace* face, uint32_t codepoint, float fontSize)
	{
		// CRLF 的 '\r' 原样保留在文本里,但不占宽度(渲染与列计算都视为不可见)。
		if (codepoint == '\r' || codepoint == '\n')
			return 0.0f;
		if (!face || !face->Info)
			return fontSize * (codepoint < 0x80 ? 0.6f : 1.0f);
		int advance = 0;
		// Tab 策略:文件里已有的 '\t' 原样保留,度量按 4 空格宽度。
		stbtt_GetCodepointHMetrics(face->Info, codepoint == '\t' ? ' ' : static_cast<int>(codepoint), &advance, nullptr);
		float width = advance * stbtt_ScaleForPixelHeight(face->Info, fontSize);
		if (codepoint == '\t')
			width *= 4.0f;
		return width;
	}

	float WuiRhiBackend::MeasureText(std::string_view text, float fontSize, WuiFontFamily family, bool bold, int byteOffset)
	{
		std::vector<uint32_t> codepoints;
		std::vector<int> offsets;
		DecodeUtf8(text, codepoints, offsets);
		float width = 0;
		for (size_t i = 0; i < codepoints.size(); ++i)
		{
			if (byteOffset >= 0 && offsets[i] >= byteOffset)
				break;
			width += AdvanceOf(FaceForCodepoint(family, bold, codepoints[i]), codepoints[i], fontSize);
		}
		return width;
	}

	void WuiRhiBackend::PushQuad(const WuiRect& rect, const WuiColor& color, const WuiRect& uv)
	{
		if (m_Vertices.size() + 4 > MaxQuads * 4)
			Flush();
		const uint32_t base = static_cast<uint32_t>(m_Vertices.size());
		const float x1 = rect.X + rect.W;
		const float y1 = rect.Y + rect.H;
		m_Vertices.push_back({ rect.X, rect.Y, color.R, color.G, color.B, color.A, uv.X, uv.Y });
		m_Vertices.push_back({ x1, rect.Y, color.R, color.G, color.B, color.A, uv.X + uv.W, uv.Y });
		m_Vertices.push_back({ x1, y1, color.R, color.G, color.B, color.A, uv.X + uv.W, uv.Y + uv.H });
		m_Vertices.push_back({ rect.X, y1, color.R, color.G, color.B, color.A, uv.X, uv.Y + uv.H });
		const uint32_t quad = base / 4;
		m_Indices.insert(m_Indices.end(), { quad * 4, quad * 4 + 1, quad * 4 + 2, quad * 4, quad * 4 + 2, quad * 4 + 3 });
	}

	void WuiRhiBackend::PushSolidQuad(const WuiRect& rect, const WuiColor& color)
	{
		PushQuad(rect, color, { -1.0f, 0.0f, 0.0f, 0.0f });
	}

	void WuiRhiBackend::PushQuadVertices(const std::array<glm::vec2, 4>& positions, const WuiColor& color)
	{
		if (m_Vertices.size() + 4 > MaxQuads * 4)
			Flush();
		const uint32_t base = static_cast<uint32_t>(m_Vertices.size());
		// 实心四边形:UV 落在白纹理上(-1 表示"不采样贴图"的既有约定)。
		for (const glm::vec2& position : positions)
			m_Vertices.push_back({ position.x, position.y, color.R, color.G, color.B, color.A, -1.0f, 0.0f });
		const uint32_t quad = base / 4;
		m_Indices.insert(m_Indices.end(), { quad * 4, quad * 4 + 1, quad * 4 + 2, quad * 4, quad * 4 + 2, quad * 4 + 3 });
	}

	void WuiRhiBackend::SetActiveTexture(const Rhi::Handle<Rhi::Texture>& texture)
	{
		if (texture.get() == m_ActiveTexture.get())
			return;
		Flush();
		m_ActiveTexture = texture;
		m_TextureChanged = true;
	}

	Rhi::Handle<Rhi::DescriptorSet> WuiRhiBackend::TextureSetFor(const Rhi::Handle<Rhi::Texture>& texture)
	{
		const uint32_t slot = FrameSlot();
		auto& textureSets = m_TextureSets[slot];
		const void* key = texture.get();
		auto it = textureSets.find(key);
		if (it != textureSets.end())
			return it->second;
		Rhi::Handle<Rhi::DescriptorSet> set = Renderer::GetDevice()->CreateDescriptorSet(m_TextureLayout);
		if (set)
		{
			Rhi::DescriptorWrite write;
			write.Binding = 1;
			write.Type = Rhi::DescriptorType::CombinedImageSampler;
			write.Texture = texture;
			write.Sampler = m_Sampler;
			set->Update({ write });
		}
		textureSets.emplace(key, set);
		return set;
	}

	uint32_t WuiRhiBackend::FrameSlot() const
	{
		return static_cast<uint32_t>(Renderer::FrameSlot()) % kFramesInFlight;
	}

	uint64_t WuiRhiBackend::SlotVertexBase() const
	{
		return static_cast<uint64_t>(FrameSlot()) * MaxQuads * 4 * sizeof(Vertex);
	}

	uint64_t WuiRhiBackend::SlotIndexBase() const
	{
		return static_cast<uint64_t>(FrameSlot()) * MaxQuads * 6 * sizeof(uint32_t);
	}

	void WuiRhiBackend::Flush()
	{
		const uint32_t slot = FrameSlot();
		if (m_Vertices.empty() || !m_Cmds[slot])
			return;
		if (m_TextureChanged)
			m_TextureChanged = false;
		// 每个批次写入独立的缓冲区区间:同一命令缓冲在提交后才由 GPU 执行,
		// 复用同一段内存会让所有绘制都读到最后一个批次的数据(界面成片缺失)。
		const uint64_t vertexBytes = m_Vertices.size() * sizeof(Vertex);
		const uint64_t indexBytes = m_Indices.size() * sizeof(uint32_t);
		const uint64_t vertexBase = SlotVertexBase();
		const uint64_t indexBase = SlotIndexBase();
		if (m_FrameVertexBytes + vertexBytes > vertexBase + MaxQuads * 4 * sizeof(Vertex) ||
			m_FrameIndexBytes + indexBytes > indexBase + static_cast<uint64_t>(MaxQuads) * 6 * sizeof(uint32_t))
		{
			m_FrameVertexBytes = vertexBase;
			m_FrameIndexBytes = indexBase;
		}
		const uint64_t vertexOffset = m_FrameVertexBytes;
		const uint64_t indexOffset = m_FrameIndexBytes;
		m_FrameVertexBytes += vertexBytes;
		m_FrameIndexBytes += indexBytes;
		// 顶点/索引走槽位缓冲的直接映射写:
		//   ① Vulkan 的 vkCmdUpdateBuffer/CopyBuffer 不允许录制在 render pass 内
		//      (VUID-vkCmdUpdateBuffer-renderpass),而批次上传发生在 Begin/EndRenderPass 之间;
		//   ② 缓冲按帧槽位环形化 + BeginFrame 等待该槽位的帧栅栏,GPU 上一轮引用已结束,
		//      映射写不会与在飞绘制冲突;CPU 侧只剩一次 memcpy(无驱动同步点)。
		m_Vbs[slot]->SetData(m_Vertices.data(), vertexBytes, vertexOffset);
		m_Ibs[slot]->SetData(m_Indices.data(), indexBytes, indexOffset);
		m_Cmds[slot]->BindPipeline(m_Pipeline);
		m_Cmds[slot]->BindDescriptorSet(m_GlobalSets[slot], 0);
		m_Cmds[slot]->BindDescriptorSet(TextureSetFor(m_ActiveTexture), 1);
		m_Cmds[slot]->BindVertexBuffer(0, m_Vbs[slot], vertexOffset);
		m_Cmds[slot]->BindIndexBuffer(m_Ibs[slot], indexOffset);
		m_Cmds[slot]->DrawIndexed(static_cast<uint32_t>(m_Indices.size()));
		m_Vertices.clear();
		m_Indices.clear();
	}

	void WuiRhiBackend::ApplyScissor(const WuiRect& rect)
	{
		const uint32_t slot = FrameSlot();
		if (!m_Cmds[slot])
			return;
		Rhi::Scissor scissor;
		scissor.X = std::max(0, static_cast<int32_t>(rect.X));
		scissor.Width = static_cast<uint32_t>(std::max(0.0f, rect.W));
		if (m_IsVulkan)
		{
			scissor.Y = std::max(0, static_cast<int32_t>(rect.Y));
		}
		else
		{
			const int32_t top = static_cast<int32_t>(rect.Y);
			const int32_t height = std::max(0, static_cast<int32_t>(rect.H));
			scissor.Y = std::max(0, static_cast<int32_t>(m_Viewport.y) - top - height);
		}
		scissor.Height = static_cast<uint32_t>(std::max(0.0f, rect.H));
			m_Cmds[slot]->SetScissor(scissor);
	}

	void WuiRhiBackend::DrawText(const WuiDrawCommand& command)
	{
		const float fontSize = command.FontSize > 0 ? command.FontSize : 15.0f;
		FontFace* baselineFace = PrimaryFace(command.Family, command.Bold);
		float baseline = command.Rect.Y + fontSize * 0.8f;
		if (baselineFace && baselineFace->Info)
		{
			int ascent = 0;
			stbtt_GetFontVMetrics(baselineFace->Info, &ascent, nullptr, nullptr);
			baseline = command.Rect.Y + ascent * stbtt_ScaleForPixelHeight(baselineFace->Info, fontSize);
		}

		if (command.TextSelStart >= 0 && command.TextSelEnd > command.TextSelStart)
		{
			const float x0 = MeasureText(command.Text, fontSize, command.Family, command.Bold, command.TextSelStart);
			const float x1 = MeasureText(command.Text, fontSize, command.Family, command.Bold, command.TextSelEnd);
			PushSolidQuad({ command.Rect.X + x0, command.Rect.Y, x1 - x0, fontSize },
				{ 0.3f, 0.5f, 0.9f, 0.45f });
		}

		std::vector<uint32_t> codepoints;
		std::vector<int> offsets;
		DecodeUtf8(command.Text, codepoints, offsets);
		float pen = command.Rect.X;
		for (uint32_t cp : codepoints)
		{
			FontFace* face = FaceForCodepoint(command.Family, command.Bold, cp);
			const float advance = AdvanceOf(face, cp, fontSize);
			// '\t' 按 4 空格推进但无字形;'\r'/'\n' 不可见。
			if (face && face->Info && cp != '\t' && cp != '\r' && cp != '\n')
			{
				Glyph& glyph = Bake(*face, cp, fontSize);
				// 按烘焙像素尺寸缩放;字号分桶后 sizeRatio≈1(大字号不再被放大糊)。
				const float sizeRatio = fontSize / (glyph.PixelSize > 0.0f ? glyph.PixelSize : face->BaseSize);
				const float w = glyph.W * sizeRatio;
				const float h = glyph.H * sizeRatio;
				if (w > 0 && h > 0)
				{
					SetActiveTexture(face->AtlasTexture);
					PushQuad({ pen + glyph.OffsetX * sizeRatio, baseline + glyph.OffsetY * sizeRatio, w, h },
						command.Color,
						{ glyph.X / face->AtlasW, glyph.Y / face->AtlasH, glyph.W / face->AtlasW, glyph.H / face->AtlasH });
				}
			}
			pen += advance;
		}

		// W9 代码编辑器 caret:按真实度量定位,0.5s 闪烁,1.5px 宽。
		if (command.TextCaretByte >= 0)
		{
			const float caretX = command.Rect.X + MeasureText(command.Text, fontSize, command.Family, command.Bold, command.TextCaretByte);
			const bool visible = std::fmod(glfwGetTime(), 1.0) < 0.5;
			if (visible)
				PushSolidQuad({ caretX, command.Rect.Y, 1.5f, fontSize }, command.Color);
		}

		if (command.TextCursorByte >= 0)
		{
			const float cursorX = command.Rect.X + MeasureText(command.Text, fontSize, command.Family, command.Bold, command.TextCursorByte);
			PushSolidQuad({ cursorX, command.Rect.Y, 1.0f, fontSize }, command.Color);
		}
	}

	void WuiRhiBackend::DrawRectCommand(const WuiDrawCommand& command, bool outline)
	{
		if (!outline)
		{
			PushSolidQuad(command.Rect, command.Color);
			return;
		}
		const float t = command.Thickness > 0 ? command.Thickness : 1.0f;
		const WuiRect& r = command.Rect;
		PushSolidQuad({ r.X, r.Y, r.W, t }, command.Color);
		PushSolidQuad({ r.X, r.Y + r.H - t, r.W, t }, command.Color);
		PushSolidQuad({ r.X, r.Y + t, t, r.H - 2 * t }, command.Color);
		PushSolidQuad({ r.X + r.W - t, r.Y + t, t, r.H - 2 * t }, command.Color);
	}

	void WuiRhiBackend::DrawImageCommand(const WuiDrawCommand& command)
	{
		Rhi::Handle<Rhi::Texture> texture = WuiTextureRegistry::Get().Resolve(command.Image);
		if (!texture)
		{
			static int s_MissingLogged = 0;
			if (s_MissingLogged < 12)
			{
				WLD_CORE_WARN("[wui-img] unresolved image id={0} rect=({1},{2},{3},{4})",
					command.Image, command.Rect.X, command.Rect.Y, command.Rect.W, command.Rect.H);
				++s_MissingLogged;
			}
			return;
		}
		SetActiveTexture(texture);
		PushQuad(command.Rect, command.Color, command.Uv);
	}

	void WuiRhiBackend::DrawList(const std::vector<WuiDrawCommand>& commands)
	{
		for (const WuiDrawCommand& command : commands)
		{
			switch (command.Kind)
			{
				case WuiDrawKind::ClipPush:
				{
					m_ClipStack.push_back(m_CurrentClip);
					WuiRect next = m_CurrentClip;
					const float x0 = std::max(next.X, command.Rect.X);
					const float y0 = std::max(next.Y, command.Rect.Y);
					const float x1 = std::min(next.X + next.W, command.Rect.X + command.Rect.W);
					const float y1 = std::min(next.Y + next.H, command.Rect.Y + command.Rect.H);
					next = { x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0) };
					m_CurrentClip = next;
					Flush();
					ApplyScissor(next);
					break;
				}
				case WuiDrawKind::ClipPop:
				{
					if (m_ClipStack.empty())
						m_CurrentClip = { 0, 0, m_Viewport.x, m_Viewport.y };
					else
					{
						m_CurrentClip = m_ClipStack.back();
						m_ClipStack.pop_back();
					}
					Flush();
					ApplyScissor(m_CurrentClip);
					break;
				}
				case WuiDrawKind::Rect: DrawRectCommand(command, false); break;
				case WuiDrawKind::RectOutline: DrawRectCommand(command, true); break;
				case WuiDrawKind::Text: DrawText(command); break;
				case WuiDrawKind::Image: DrawImageCommand(command); break;
				case WuiDrawKind::Quad: PushQuadVertices(command.Vertices, command.Color); break;
			}
		}
	}

	void WuiRhiBackend::Render(const std::vector<WuiDrawCommand>& commands, const std::vector<WuiDrawCommand>& overlayCommands)
	{
		if (!Renderer::GetDevice())
			return;
		// 只在设备真正切换时失效注册表;首帧 m_DeviceKey 还是空指针,
		// 之前会把宿主在 OnAttach 里注册好的图标/场景纹理全部清掉。
		if (m_DeviceKey && Renderer::GetDevice().get() != m_DeviceKey)
			WuiTextureRegistry::Get().Clear();
		// 注册表重建后,按纹理缓存的描述符集会指向已销毁的贴图,需要一并失效。
		if (WuiTextureRegistry::Get().Generation() != m_TextureGeneration)
		{
			for (uint32_t slot = 0; slot < kFramesInFlight; ++slot)
				m_TextureSets[slot].clear();
			m_TextureGeneration = WuiTextureRegistry::Get().Generation();
		}
		EnsureResources();
		const uint32_t slot = FrameSlot();
		if (!m_Cmds[slot] || !m_Pipeline)
			return;

		Rhi::Handle<Rhi::RenderPass> pass;
		Rhi::Handle<Rhi::Framebuffer> framebuffer;
		if (m_IsVulkan)
		{
			pass = Renderer::GetPresentRenderPass();
			framebuffer = Renderer::GetPresentFramebuffer();
			if (!framebuffer)
				return;
		}
		else
		{
			const uint32_t width = std::max(1u, static_cast<uint32_t>(m_Viewport.x));
			const uint32_t height = std::max(1u, static_cast<uint32_t>(m_Viewport.y));
			if (m_UiWidth != width || m_UiHeight != height || !m_UiFramebuffer)
			{
				m_UiWidth = width;
				m_UiHeight = height;
				// 旧的离屏目标可能仍在飞:延迟释放,避免 resize 时回收在用资源。
				if (m_UiFramebuffer || m_UiColor)
				{
					auto oldFramebuffer = m_UiFramebuffer;
					auto oldColor = m_UiColor;
					Renderer::QueueRelease([oldFramebuffer, oldColor]() {});
				}
				Rhi::TextureDesc colorDesc;
				colorDesc.Type = Rhi::TextureType::Texture2D;
				colorDesc.Format = Rhi::Format::R8G8B8A8_UNORM;
				colorDesc.Extent = { width, height, 1 };
				colorDesc.Usage = Rhi::TextureUsageColorAttachment | Rhi::TextureUsageSampled;
				m_UiColor = Renderer::GetDevice()->CreateTexture(colorDesc);
				Rhi::FramebufferDesc framebufferDesc;
				framebufferDesc.RenderPass = m_UiPass;
				framebufferDesc.Extent = { width, height };
				framebufferDesc.Attachments = { m_UiColor };
				framebufferDesc.DebugName = "WuiOffscreen";
				m_UiFramebuffer = Renderer::GetDevice()->CreateFramebuffer(framebufferDesc);
			}
			pass = m_UiPass;
			framebuffer = m_UiFramebuffer;
		}

		const auto prebake = [&](const std::vector<WuiDrawCommand>& list)
		{
			for (const WuiDrawCommand& command : list)
				if (command.Kind == WuiDrawKind::Text)
				{
					std::vector<uint32_t> codepoints;
					std::vector<int> offsets;
					DecodeUtf8(command.Text, codepoints, offsets);
					for (uint32_t cp : codepoints)
					{
						if (cp == '\t' || cp == '\r' || cp == '\n')
							continue;
						FontFace* face = FaceForCodepoint(command.Family, command.Bold, cp);
						if (face && face->Info)
							Bake(*face, cp, command.FontSize > 0.0f ? command.FontSize : 15.0f);
					}
				}
		};
		prebake(commands);
		prebake(overlayCommands);
		// 预解析图像贴图:GL→Vulkan 包装包含一次离屏上传(提交并等待队列),
		// 必须在命令缓冲开始录制之前完成,否则上传不可靠(图标/场景贴图全黑)。
		for (const WuiDrawCommand& command : commands)
			if (command.Kind == WuiDrawKind::Image)
				WuiTextureRegistry::Get().Resolve(command.Image);
		for (const WuiDrawCommand& command : overlayCommands)
			if (command.Kind == WuiDrawKind::Image)
				WuiTextureRegistry::Get().Resolve(command.Image);
		for (FontFace& face : m_Faces)
			if (face.AtlasDirty && face.AtlasTexture)
			{
				face.AtlasTexture->SetData(face.Atlas.data(), face.Atlas.size());
				face.AtlasDirty = false;
			}

		// WUI 矩形是“原点在左上、Y 向下”。GL 的 NDC +Y 在窗口上方,Vulkan 的
		// NDC +Y 在窗口下方,所以要按后端取相反的 Y 顺序,才能让 rect.Y=0 落在
		// 屏幕顶部并与 ApplyScissor 的裁剪矩形一致(否则界面整体上下颠倒)。
		m_Projection = m_IsVulkan
			? glm::ortho(0.0f, m_Viewport.x, 0.0f, m_Viewport.y, -1.0f, 1.0f)
			: glm::ortho(0.0f, m_Viewport.x, m_Viewport.y, 0.0f, -1.0f, 1.0f);

		m_Vertices.clear();
		m_Indices.clear();
		m_ClipStack.clear();
		m_CurrentClip = { 0, 0, m_Viewport.x, m_Viewport.y };
		m_FrameVertexBytes = SlotVertexBase();
		m_FrameIndexBytes = SlotIndexBase();
		m_ActiveTexture = m_WhiteTexture;
		m_TextureChanged = true;

		m_Cmds[slot]->Begin();
		// 投影 UBO 走槽位缓冲的映射写(理由同批次顶点:render pass 内不能录制上传命令)。
		m_Ubos[slot]->SetData(&m_Projection, sizeof(glm::mat4), 0);
		Rhi::ClearValue clear;
		clear.Color = { 0, 0, 0, 0 };
		m_Cmds[slot]->BeginRenderPass(pass, framebuffer, { clear });
		m_Cmds[slot]->SetViewport({ 0, 0, m_Viewport.x, m_Viewport.y, 0.0f, 1.0f });
		ApplyScissor(m_CurrentClip);
		DrawList(commands);
		DrawList(overlayCommands);
		Flush();
		m_Cmds[slot]->EndRenderPass();
		m_Cmds[slot]->End();
		Renderer::SubmitUi(m_Cmds[slot]);

		if (!m_IsVulkan)
		{
			Rhi::BlitFramebufferToBackbuffer(m_UiFramebuffer, { m_UiWidth, m_UiHeight });
		}
	}

	void WuiRhiBackend::EndFrame(WuiCursor cursor)
	{
		if (m_UseLocalInput)
			m_LocalInput.EndFrame();
		else
			s_Input.EndFrame();
		GLFWwindow* window = m_CursorWindow
			? static_cast<GLFWwindow*>(m_CursorWindow)
			: (Application::HasInstance()
				? static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow())
				: nullptr);
		if (!window)
			return;
		int index = 0;
		int shape = GLFW_ARROW_CURSOR;
		switch (cursor)
		{
			case WuiCursor::IBeam: index = 1; shape = GLFW_IBEAM_CURSOR; break;
			case WuiCursor::ResizeEW: index = 2; shape = GLFW_HRESIZE_CURSOR; break;
			case WuiCursor::ResizeNS: index = 3; shape = GLFW_VRESIZE_CURSOR; break;
			case WuiCursor::Hand: index = 4; shape = GLFW_HAND_CURSOR; break;
			default: index = 0; shape = GLFW_ARROW_CURSOR; break;
		}
		static GLFWcursor* cursors[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
		// 只在形状变化时写入,避免每帧重复设置造成光标闪烁。
		static int currentIndex = -1;
		if (index != currentIndex)
		{
			if (!cursors[index])
				cursors[index] = glfwCreateStandardCursor(shape);
			if (cursors[index])
				glfwSetCursor(window, cursors[index]);
			currentIndex = index;
		}
	}
}




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

		void DecodeUtf8(const std::string& text, std::vector<uint32_t>& codepoints, std::vector<int>& byteOffsets)
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
		m_Faces.resize(3);
	}

	WuiRhiBackend::~WuiRhiBackend()
	{
		ReleaseResources();
		for (FontFace& face : m_Faces)
			delete face.Info;
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
		m_Cmd = nullptr;
		m_Shader = nullptr;
		m_Pipeline = nullptr;
		m_Vb = nullptr;
		m_Ib = nullptr;
		m_Ubo = nullptr;
		m_TextureLayout = nullptr;
		m_GlobalSet = nullptr;
		m_Sampler = nullptr;
		m_WhiteTexture = nullptr;
		m_UiPass = nullptr;
		m_UiColor = nullptr;
		m_UiFramebuffer = nullptr;
		m_UiWidth = m_UiHeight = 0;
		m_ActiveTexture = nullptr;
		for (FontFace& face : m_Faces)
			face.AtlasTexture = nullptr;
		// 每纹理描述符集也是设备对象:不清空会在旧设备销毁后被析构(驱动层崩溃)。
		m_TextureSets.clear();
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

		m_Cmd = device->CreateCommandBuffer("WuiBackend");

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
		m_GlobalSet = device->CreateDescriptorSet(Renderer::GetGlobalDescriptorSetLayout());

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
		vertexDesc.Size = static_cast<uint64_t>(MaxQuads) * 4 * sizeof(Vertex);
		vertexDesc.Usage = Rhi::BufferUsageVertex;
		vertexDesc.Memory = Rhi::MemoryHint::HostVisible;
		m_Vb = device->CreateBuffer(vertexDesc);
		Rhi::BufferDesc indexDesc;
		indexDesc.Size = static_cast<uint64_t>(MaxQuads) * 6 * sizeof(uint32_t);
		indexDesc.Usage = Rhi::BufferUsageIndex;
		indexDesc.Memory = Rhi::MemoryHint::HostVisible;
		m_Ib = device->CreateBuffer(indexDesc);
		Rhi::BufferDesc uboDesc;
		uboDesc.Size = sizeof(glm::mat4);
		uboDesc.Usage = Rhi::BufferUsageUniform;
		uboDesc.Memory = Rhi::MemoryHint::HostVisible;
		m_Ubo = device->CreateBuffer(uboDesc);
		Rhi::DescriptorWrite globalWrite;
		globalWrite.Binding = 0;
		globalWrite.Type = Rhi::DescriptorType::UniformBuffer;
		globalWrite.Buffer = m_Ubo;
		m_GlobalSet->Update({ globalWrite });

		Rhi::TextureDesc whiteDesc;
		whiteDesc.Type = Rhi::TextureType::Texture2D;
		whiteDesc.Format = Rhi::Format::R8G8B8A8_UNORM;
		whiteDesc.Extent = { 1, 1, 1 };
		whiteDesc.Usage = Rhi::TextureUsageSampled;
		m_WhiteTexture = device->CreateTexture(whiteDesc);
		const unsigned char white[4] = { 255, 255, 255, 255 };
		m_WhiteTexture->SetData(white, 4);

		const std::string fontPaths[3] = {
			std::string(WLD_EDITOR_DIR) + "assets/fonts/Montserrat/static/Montserrat-Regular.ttf",
			std::string(WLD_EDITOR_DIR) + "assets/fonts/Montserrat/static/Montserrat-Bold.ttf",
			std::string(WLD_EDITOR_DIR) + "assets/fonts/NotoSansSC/NotoSansSC-Subset.ttf",
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

	WuiRhiBackend::FontFace& WuiRhiBackend::FaceFor(const std::string& text, bool bold)
	{
		for (unsigned char c : text)
			if (c > 127)
				return m_Faces[2];
		return bold ? m_Faces[1] : m_Faces[0];
	}

	WuiRhiBackend::Glyph& WuiRhiBackend::Bake(FontFace& face, uint32_t codepoint)
	{
		auto it = face.Glyphs.find(codepoint);
		if (it != face.Glyphs.end())
			return it->second;
		Glyph glyph;
		const float scale = stbtt_ScaleForPixelHeight(face.Info, face.BaseSize);
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
		glyph.OffsetX = static_cast<float>(x0);
		glyph.OffsetY = static_cast<float>(y0);
		int advance = 0;
		stbtt_GetCodepointHMetrics(face.Info, codepoint, &advance, nullptr);
		glyph.Advance = advance * scale;
		const auto result = face.Glyphs.emplace(codepoint, glyph);
		return result.first->second;
	}

	float WuiRhiBackend::AdvanceOf(FontFace& face, uint32_t codepoint, float fontSize)
	{
		if (!face.Info)
			return fontSize * 0.5f;
		int advance = 0;
		stbtt_GetCodepointHMetrics(face.Info, codepoint, &advance, nullptr);
		return advance * stbtt_ScaleForPixelHeight(face.Info, fontSize);
	}

	float WuiRhiBackend::Measure(FontFace& face, const std::string& text, float fontSize, int byteOffset)
	{
		std::vector<uint32_t> codepoints;
		std::vector<int> offsets;
		DecodeUtf8(text, codepoints, offsets);
		float width = 0;
		for (size_t i = 0; i < codepoints.size(); ++i)
		{
			if (byteOffset >= 0 && offsets[i] >= byteOffset)
				break;
			width += AdvanceOf(face, codepoints[i], fontSize);
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
		const void* key = texture.get();
		auto it = m_TextureSets.find(key);
		if (it != m_TextureSets.end())
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
		m_TextureSets.emplace(key, set);
		return set;
	}

	void WuiRhiBackend::Flush()
	{
		if (m_Vertices.empty() || !m_Cmd)
			return;
		if (m_TextureChanged)
			m_TextureChanged = false;
		// 每个批次写入独立的缓冲区区间:同一命令缓冲在提交后才由 GPU 执行,
		// 复用同一段内存会让所有绘制都读到最后一个批次的数据(界面成片缺失)。
		const uint64_t vertexBytes = m_Vertices.size() * sizeof(Vertex);
		const uint64_t indexBytes = m_Indices.size() * sizeof(uint32_t);
		if (m_FrameVertexBytes + vertexBytes > MaxQuads * 4 * sizeof(Vertex) ||
			m_FrameIndexBytes + indexBytes > static_cast<uint64_t>(MaxQuads) * 6 * sizeof(uint32_t))
		{
			m_FrameVertexBytes = 0;
			m_FrameIndexBytes = 0;
		}
		const uint64_t vertexOffset = m_FrameVertexBytes;
		const uint64_t indexOffset = m_FrameIndexBytes;
		m_FrameVertexBytes += vertexBytes;
		m_FrameIndexBytes += indexBytes;
		m_Vb->SetData(m_Vertices.data(), vertexBytes, vertexOffset);
		m_Ib->SetData(m_Indices.data(), indexBytes, indexOffset);
		m_Cmd->BindPipeline(m_Pipeline);
		m_Cmd->BindDescriptorSet(m_GlobalSet, 0);
		m_Cmd->BindDescriptorSet(TextureSetFor(m_ActiveTexture), 1);
		m_Cmd->BindVertexBuffer(0, m_Vb, vertexOffset);
		m_Cmd->BindIndexBuffer(m_Ib, indexOffset);
		m_Cmd->DrawIndexed(static_cast<uint32_t>(m_Indices.size()));
		m_Vertices.clear();
		m_Indices.clear();
	}

	void WuiRhiBackend::ApplyScissor(const WuiRect& rect)
	{
		if (!m_Cmd)
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
		m_Cmd->SetScissor(scissor);
	}

	void WuiRhiBackend::DrawText(const WuiDrawCommand& command)
	{
		FontFace& face = FaceFor(command.Text, command.Bold);
		if (!face.Info)
			return;
		const float fontSize = command.FontSize > 0 ? command.FontSize : 15.0f;
		const float scale = stbtt_ScaleForPixelHeight(face.Info, fontSize);
		const float sizeRatio = fontSize / face.BaseSize;
		int ascent = 0;
		stbtt_GetFontVMetrics(face.Info, &ascent, nullptr, nullptr);
		const float baseline = command.Rect.Y + ascent * scale;

		if (command.TextSelStart >= 0 && command.TextSelEnd > command.TextSelStart)
		{
			const float x0 = Measure(face, command.Text, fontSize, command.TextSelStart);
			const float x1 = Measure(face, command.Text, fontSize, command.TextSelEnd);
			PushSolidQuad({ command.Rect.X + x0, command.Rect.Y, x1 - x0, fontSize },
				{ 0.3f, 0.5f, 0.9f, 0.45f });
		}

		std::vector<uint32_t> codepoints;
		std::vector<int> offsets;
		DecodeUtf8(command.Text, codepoints, offsets);
		float pen = command.Rect.X;
		SetActiveTexture(face.AtlasTexture);
		for (uint32_t cp : codepoints)
		{
			Glyph& glyph = Bake(face, cp);
			const float w = glyph.W * sizeRatio;
			const float h = glyph.H * sizeRatio;
			if (w > 0 && h > 0)
				PushQuad({ pen + glyph.OffsetX * sizeRatio, baseline + glyph.OffsetY * sizeRatio, w, h },
					command.Color,
					{ glyph.X / face.AtlasW, glyph.Y / face.AtlasH, glyph.W / face.AtlasW, glyph.H / face.AtlasH });
			pen += glyph.Advance * sizeRatio;
		}

		if (command.TextCursorByte >= 0)
		{
			const float cursorX = command.Rect.X + Measure(face, command.Text, fontSize, command.TextCursorByte);
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
			m_TextureSets.clear();
			m_TextureGeneration = WuiTextureRegistry::Get().Generation();
		}
		EnsureResources();
		if (!m_Cmd || !m_Pipeline)
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
					FontFace& face = FaceFor(command.Text, command.Bold);
					if (face.Info)
						for (uint32_t cp : codepoints)
							Bake(face, cp);
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
		m_Ubo->SetData(&m_Projection, sizeof(glm::mat4));

		m_Vertices.clear();
		m_Indices.clear();
		m_ClipStack.clear();
		m_CurrentClip = { 0, 0, m_Viewport.x, m_Viewport.y };
		m_FrameVertexBytes = 0;
		m_FrameIndexBytes = 0;
		m_ActiveTexture = m_WhiteTexture;
		m_TextureChanged = true;

		m_Cmd->Begin();
		Rhi::ClearValue clear;
		clear.Color = { 0, 0, 0, 0 };
		m_Cmd->BeginRenderPass(pass, framebuffer, { clear });
		m_Cmd->SetViewport({ 0, 0, m_Viewport.x, m_Viewport.y, 0.0f, 1.0f });
		ApplyScissor(m_CurrentClip);
		DrawList(commands);
		DrawList(overlayCommands);
		Flush();
		m_Cmd->EndRenderPass();
		m_Cmd->End();
		Renderer::SubmitUi(m_Cmd);

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

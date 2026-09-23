#pragma once

namespace World
{
	// 定义帧缓冲纹理格式
	enum class FramebufferTextureFormat
	{
		None = 0,
		// Color
		RGBA8,
		RED_INTEGER,

		// Depth/stencil
		Depth24Stencil8,

		// Default
		Depth = Depth24Stencil8,
	};

	// 定义帧缓冲纹理规范
	struct FramebufferTextureSpecification
	{
		FramebufferTextureSpecification(FramebufferTextureFormat format)
			: TextureFormat(format)
		{}

		FramebufferTextureFormat TextureFormat = FramebufferTextureFormat::None;

		// TODO: Filtering/wrap
	};

	// 定义帧缓冲附件规范，允许用户指定多个附件（例如多个颜色附件和一个深度附件）
	struct FramebufferAttachmentSpecification
	{
		FramebufferAttachmentSpecification() = default;
		FramebufferAttachmentSpecification(std::initializer_list<FramebufferTextureSpecification> attachments)
			: Attachments(attachments)
		{}

		std::vector<FramebufferTextureSpecification> Attachments;
	};

	// 定义帧缓冲规范，包含帧缓冲的尺寸、附件规格等信息
	struct FramebufferSpecification
	{
		uint32_t Width, Height;

		FramebufferAttachmentSpecification Attachments;

		// TODO: Support for multiple color attachments
		uint32_t Samples = 1;

		// TODO: Support for depth attachment
		bool SwapChainTarget = false;
	};

	// 将渲染的内容渲染到一个纹理对象上，可以用于后续的屏幕空间效果或者作为一个离屏渲染目标
	class Framebuffer
	{
	public:
		static Ref<Framebuffer> Create(const FramebufferSpecification& spec);

	public:
		virtual ~Framebuffer() = default;
		virtual void Resize(uint32_t width, uint32_t height) = 0;
		virtual const FramebufferSpecification& GetSpecification() const = 0;
		virtual uint32_t GetColorAttachmentRendererID(size_t index = 0) const = 0;
		virtual void Bind() = 0;
		virtual void Unbind() = 0;
		virtual int ReadPixel(uint32_t attachmentIndex, int x, int y) = 0;
		virtual void ClearAttachment(uint32_t attachmentIndex, int value) = 0;
	};
}



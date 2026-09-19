#include "wldpch.h"
#include "OpenGLRenderPass.h"
#include "OpenGLTexture.h"

#include <glad/glad.h>

namespace World::Rhi::OpenGL
{
	namespace
	{
		bool IsDepthStencilFormat(Format format)
		{
			return format == Format::D16_UNORM || format == Format::D32_SFLOAT ||
				format == Format::D24_UNORM_S8_UINT || format == Format::D32_SFLOAT_S8_UINT;
		}
	}

	OpenGLFramebuffer::OpenGLFramebuffer(const FramebufferDesc& desc)
		: m_Desc(desc)
	{
		WLD_CORE_ASSERT(desc.RenderPass, "FramebufferDesc requires a render pass");
		const auto& passDesc = desc.RenderPass->GetDesc();

		// P4-4a:resolve 目标(SubpassDesc::ResolveAttachments 指向的单采样附件)不是绘制附件。
		// GL 要求同一个 FBO 里所有附件的采样数一致,把单采样 resolve 目标挂进来必然
		// GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE —— 该通道的全部绘制被静默丢弃。
		// 它们只在通道末由 OpenGLCommandBuffer 的 blit resolve 写入(见 EndRenderPass 回放),
		// 因此这里既不挂载、也不进 draw buffers。
		std::vector<bool> isResolveTarget(passDesc.Attachments.size(), false);
		for (const SubpassDesc& subpass : passDesc.Subpasses)
			for (uint32_t index : subpass.ResolveAttachments)
				if (index < isResolveTarget.size())
					isResolveTarget[index] = true;

		glCreateFramebuffers(1, &m_ID);
		m_AttachmentIDs.resize(passDesc.Attachments.size(), 0);
		std::vector<GLenum> drawBuffers;
		for (size_t i = 0; i < passDesc.Attachments.size() && i < desc.Attachments.size(); i++)
		{
			if (isResolveTarget[i])
				continue;
			const auto& attachment = passDesc.Attachments[i];
			const auto texture = std::dynamic_pointer_cast<OpenGLTexture>(desc.Attachments[i]);
			if (!texture)
				continue;
			m_AttachmentIDs[i] = texture->GetID();

			if (IsDepthStencilFormat(attachment.Format))
			{
				// 挂点必须与纹理的实际分量匹配:带模板的(D24S8/D32S8)用 DEPTH_STENCIL,
				// 纯深度的(D16/D32_SFLOAT)必须用 DEPTH —— 否则 core profile 判定
				// INCOMPLETE_ATTACHMENT(0x8CD6),该渲染通道的所有绘制被静默丢弃
				// (实测:D4 阴影图 D32_SFLOAT 被挂到 DEPTH_STENCIL 后,GL 阴影通道整帧不写)。
				const bool hasStencil = attachment.Format == Format::D24_UNORM_S8_UINT ||
					attachment.Format == Format::D32_SFLOAT_S8_UINT;
				glNamedFramebufferTexture(m_ID,
					hasStencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, texture->GetID(), 0);
			}
			else
			{
				glNamedFramebufferTexture(m_ID, GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i), texture->GetID(), 0);
				drawBuffers.push_back(GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i));
			}
		}
		if (!drawBuffers.empty())
			glNamedFramebufferDrawBuffers(m_ID, static_cast<GLsizei>(drawBuffers.size()), drawBuffers.data());
		else
			// 纯深度/模板目标(如 D4 的阴影通道 FBO):core profile 下必须显式声明
			// "没有颜色输出"。默认 draw buffer = GL_COLOR_ATTACHMENT0,而本 FBO 没有颜色附件,
			// 完整性检查因此失败 → 该通道的所有绘制被**静默丢弃**(实测:阴影图从未被写入,
			// 采样恒判"全在阴影里";GL 日志里只有一行 'framebuffer ... is incomplete')。
			glNamedFramebufferDrawBuffer(m_ID, GL_NONE);

		const GLenum status = glCheckNamedFramebufferStatus(m_ID, GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
			WLD_CORE_WARN("OpenGL framebuffer '{0}' is incomplete (status=0x{1:x})", desc.DebugName,
				static_cast<uint32_t>(status));
		// 诊断(WLD_GL_TRACE_DRAW):把 FBO id 与它挂的纹理对起来 ——
		// "预览画到了哪个目标、读回读的是哪张纹理"必须能对上号。
		static const bool traceFramebuffers = std::getenv("WLD_GL_TRACE_DRAW") != nullptr;
		if (traceFramebuffers)
		{
			std::string attachments;
			for (size_t i = 0; i < m_AttachmentIDs.size(); ++i)
			{
				if (i)
					attachments += ",";
				attachments += std::to_string(m_AttachmentIDs[i]);
			}
			WLD_CORE_INFO("[gl-fbo] id={0} name='{1}' attachments=[{2}] ctx={3}", m_ID, desc.DebugName,
				attachments, reinterpret_cast<uintptr_t>(wglGetCurrentContext()));
		}
	}

	OpenGLFramebuffer::~OpenGLFramebuffer()
	{
		if (m_ID)
			glDeleteFramebuffers(1, &m_ID);
	}

	uint32_t OpenGLFramebuffer::GetAttachmentID(size_t index) const
	{
		return index < m_AttachmentIDs.size() ? m_AttachmentIDs[index] : 0;
	}

	int OpenGLFramebuffer::ReadPixel(uint32_t attachmentIndex, int x, int y)
	{
		if (attachmentIndex >= m_AttachmentIDs.size())
			return -1;
		// P4-4a:多采样附件不能 glReadPixels(一律 GL_INVALID_OPERATION,读回的是垃圾值)。
		// 读回请用"resolve 目标(单采样)对应的附件下标",或走 RHI 的
		// CopyTextureToBuffer(拾取路径就是这么做的:读 EntityTexture)。
		if (attachmentIndex < m_Desc.Attachments.size())
			if (const auto texture = std::dynamic_pointer_cast<OpenGLTexture>(m_Desc.Attachments[attachmentIndex]))
				if (texture->GetDesc().Samples != SampleCount::Count1)
				{
					WLD_CORE_WARN("ReadPixel on multisampled attachment {0} of framebuffer '{1}' is not supported; "
						"read the single-sampled resolve target instead", attachmentIndex, m_Desc.DebugName);
					return -1;
				}
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ID);
		glReadBuffer(GL_COLOR_ATTACHMENT0 + attachmentIndex);
		int pixel = -1;
		glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_INT, &pixel);
		glReadBuffer(GL_NONE);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		return pixel;
	}
}

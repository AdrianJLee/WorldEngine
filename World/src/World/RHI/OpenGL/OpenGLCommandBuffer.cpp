#include "wldpch.h"
#include "OpenGLCommandBuffer.h"
#include "OpenGLHelpers.h"
#include "OpenGLBuffer.h"
#include "OpenGLDescriptorSet.h"
#include "OpenGLPipeline.h"
#include "OpenGLQueryPool.h"
#include "OpenGLRenderPass.h"
#include "OpenGLTexture.h"

namespace World::Rhi::OpenGL
{
	namespace
	{
		bool IsIntegerFormat(Format format)
		{
			switch (format)
			{
				case Format::R8_UINT:
				case Format::R8G8_UINT:
				case Format::R8G8B8A8_UINT:
				case Format::R16_UINT:
				case Format::R16G16_UINT:
				case Format::R16G16B16A16_UINT:
				case Format::R32_UINT:
				case Format::R32G32_UINT:
				case Format::R32G32B32A32_UINT:
				case Format::R8_SINT:
				case Format::R8G8_SINT:
				case Format::R8G8B8A8_SINT:
				case Format::R16_SINT:
				case Format::R16G16_SINT:
				case Format::R16G16B16A16_SINT:
				case Format::R32_SINT:
				case Format::R32G32_SINT:
				case Format::R32G32B32A32_SINT:
					return true;
				default:
					return false;
			}
		}
	}

	void OpenGLCommandBuffer::Begin()
	{
		m_InRenderPass = false;
		m_CurrentPipeline = nullptr;
	}

	void OpenGLCommandBuffer::End()
	{
		// GL 立即模式:录制即执行。
	}

	void OpenGLCommandBuffer::BeginLabel(const std::string& label)
	{
		glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, static_cast<GLsizei>(label.size()), label.c_str());
	}

	void OpenGLCommandBuffer::EndLabel()
	{
		glPopDebugGroup();
	}

	void OpenGLCommandBuffer::BeginRenderPass(const Handle<RenderPass>& pass,
		const Handle<Framebuffer>& framebuffer, const std::vector<ClearValue>& clears)
	{
		const auto glFramebuffer = std::dynamic_pointer_cast<OpenGLFramebuffer>(framebuffer);
		const GLuint fbo = glFramebuffer ? glFramebuffer->GetID() : 0;
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		m_InRenderPass = true;

		const auto& desc = pass->GetDesc();
		if (desc.Subpasses.empty())
			return;

		const auto& subpass = desc.Subpasses[0];
		Extent2D extent = glFramebuffer ? framebuffer->GetDesc().Extent : Extent2D{};
		if (extent.Width == 0 || extent.Height == 0)
			extent = Extent2D{ 1, 1 };
		glViewport(0, 0, extent.Width, extent.Height);
		glScissor(0, 0, extent.Width, extent.Height);
		glEnable(GL_SCISSOR_TEST);

		const size_t clearCount = clears.size();
		for (size_t i = 0; i < subpass.ColorAttachments.size(); i++)
		{
			const uint32_t attachmentIndex = subpass.ColorAttachments[i].Index;
			const auto& attachment = desc.Attachments[attachmentIndex];
			if (attachment.Load != LoadOp::Clear)
				continue;
			ClearColor color{};
			if (attachmentIndex < clearCount)
				color = clears[attachmentIndex].Color;
			if (IsIntegerFormat(attachment.Format))
			{
				// 合同无整数清除字段;ClearColor 的存储按位复用整数清除值。
				glClearBufferiv(GL_COLOR, static_cast<GLint>(i), reinterpret_cast<const GLint*>(&color));
			}
			else
				glClearBufferfv(GL_COLOR, static_cast<GLint>(i), &color.R);
		}

		if (subpass.HasDepthStencil())
		{
			const auto& attachment = desc.Attachments[subpass.DepthStencilAttachment.Index];
			if (attachment.Load == LoadOp::Clear)
			{
				ClearDepthStencil ds{};
				if (subpass.DepthStencilAttachment.Index < clearCount && clears[subpass.DepthStencilAttachment.Index].IsDepthStencil)
					ds = clears[subpass.DepthStencilAttachment.Index].DepthStencil;
				glClearBufferfi(GL_DEPTH_STENCIL, 0, ds.Depth, static_cast<GLint>(ds.Stencil));
			}
		}
	}

	void OpenGLCommandBuffer::NextSubpass()
	{
		// GL 后端单子通道执行;多子通道依赖与输入附件未降级。
	}

	void OpenGLCommandBuffer::EndRenderPass()
	{
		m_InRenderPass = false;
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	void OpenGLCommandBuffer::SetViewport(const Viewport& viewport)
	{
		glViewport(static_cast<GLint>(viewport.X), static_cast<GLint>(viewport.Y),
			static_cast<GLsizei>(viewport.Width), static_cast<GLsizei>(viewport.Height));
		glDepthRange(viewport.MinDepth, viewport.MaxDepth);
	}

	void OpenGLCommandBuffer::SetScissor(const Scissor& scissor)
	{
		glEnable(GL_SCISSOR_TEST);
		glScissor(scissor.X, scissor.Y, scissor.Width, scissor.Height);
	}

	void OpenGLCommandBuffer::BindPipeline(const Handle<Pipeline>& pipeline)
	{
		m_CurrentPipeline = pipeline;
		if (pipeline)
			std::static_pointer_cast<OpenGLPipeline>(pipeline)->Bind();
	}

	void OpenGLCommandBuffer::BindDescriptorSet(const Handle<DescriptorSet>& set, uint32_t /*firstSet*/)
	{
		if (set)
			std::static_pointer_cast<OpenGLDescriptorSet>(set)->Bind();
	}

	void OpenGLCommandBuffer::BindVertexBuffer(uint32_t binding, const Handle<Buffer>& buffer, uint64_t offset)
	{
		if (!m_CurrentPipeline)
			return;
		const auto pipeline = std::static_pointer_cast<OpenGLPipeline>(m_CurrentPipeline);
		const auto glBuffer = std::dynamic_pointer_cast<OpenGLBuffer>(buffer);
		if (!glBuffer)
			return;

		uint32_t stride = 0;
		for (const auto& vertexBinding : pipeline->GetDesc().VertexBindings)
			if (vertexBinding.Binding == binding)
				stride = vertexBinding.Stride;
		glVertexArrayVertexBuffer(pipeline->GetVertexArray(), binding, glBuffer->GetID(),
			static_cast<GLintptr>(offset), static_cast<GLsizei>(stride));
	}

	void OpenGLCommandBuffer::BindIndexBuffer(const Handle<Buffer>& buffer, uint64_t offset, IndexType indexType)
	{
		m_IndexType = indexType;
		m_IndexBufferOffset = offset;
		if (!m_CurrentPipeline)
			return;
		const auto pipeline = std::static_pointer_cast<OpenGLPipeline>(m_CurrentPipeline);
		const auto glBuffer = std::dynamic_pointer_cast<OpenGLBuffer>(buffer);
		if (glBuffer)
			glVertexArrayElementBuffer(pipeline->GetVertexArray(), glBuffer->GetID());
	}

	void OpenGLCommandBuffer::PushConstants(ShaderStageFlags /*stages*/, uint32_t /*offset*/, uint32_t /*size*/, const void* /*data*/)
	{
		if (!m_PushConstantsWarned)
		{
			WLD_CORE_WARN("OpenGL backend does not implement push constants; use uniform buffers");
			m_PushConstantsWarned = true;
		}
	}

	void OpenGLCommandBuffer::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
	{
		GLenum topology = GL_TRIANGLES;
		if (m_CurrentPipeline)
			topology = ToGLTopology(std::static_pointer_cast<OpenGLPipeline>(m_CurrentPipeline)->GetDesc().Topology);
		if (instanceCount > 1 || firstInstance > 0)
			glDrawArraysInstancedBaseInstance(topology, firstVertex, vertexCount, instanceCount, firstInstance);
		else
			glDrawArrays(topology, firstVertex, vertexCount);
	}

	void OpenGLCommandBuffer::DrawIndexed(uint32_t indexCount, uint32_t instanceCount,
		uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
	{
		GLenum topology = GL_TRIANGLES;
		if (m_CurrentPipeline)
		{
			topology = ToGLTopology(std::static_pointer_cast<OpenGLPipeline>(m_CurrentPipeline)->GetDesc().Topology);
		}
		const GLenum type = m_IndexType == IndexType::UInt16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
		const void* offset = reinterpret_cast<const void*>(static_cast<uintptr_t>(m_IndexBufferOffset) +
			static_cast<uintptr_t>(firstIndex) *
			(m_IndexType == IndexType::UInt16 ? sizeof(uint16_t) : sizeof(uint32_t)));
		glDrawElementsInstancedBaseVertexBaseInstance(topology, indexCount, type, offset,
			instanceCount, vertexOffset, firstInstance);
	}

	void OpenGLCommandBuffer::DrawIndirect(const Handle<Buffer>& args, uint64_t offset, uint32_t drawCount, uint32_t stride)
	{
		const auto glBuffer = std::dynamic_pointer_cast<OpenGLBuffer>(args);
		if (!glBuffer)
			return;
		glBindBuffer(GL_DRAW_INDIRECT_BUFFER, glBuffer->GetID());
		GLenum topology = GL_TRIANGLES;
		if (m_CurrentPipeline)
			topology = ToGLTopology(std::static_pointer_cast<OpenGLPipeline>(m_CurrentPipeline)->GetDesc().Topology);
		const auto ptr = reinterpret_cast<const void*>(static_cast<uintptr_t>(offset));
		if (drawCount > 1)
			glMultiDrawArraysIndirect(topology, ptr, drawCount, stride);
		else
			glDrawArraysIndirect(topology, ptr);
	}

	void OpenGLCommandBuffer::DrawIndexedIndirect(const Handle<Buffer>& args, uint64_t offset, uint32_t drawCount, uint32_t stride)
	{
		const auto glBuffer = std::dynamic_pointer_cast<OpenGLBuffer>(args);
		if (!glBuffer)
			return;
		glBindBuffer(GL_DRAW_INDIRECT_BUFFER, glBuffer->GetID());
		GLenum topology = GL_TRIANGLES;
		if (m_CurrentPipeline)
			topology = ToGLTopology(std::static_pointer_cast<OpenGLPipeline>(m_CurrentPipeline)->GetDesc().Topology);
		const auto ptr = reinterpret_cast<const void*>(static_cast<uintptr_t>(offset));
		const GLenum type = m_IndexType == IndexType::UInt16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
		if (drawCount > 1)
			glMultiDrawElementsIndirect(topology, type, ptr, drawCount, stride);
		else
			glDrawElementsIndirect(topology, type, ptr);
	}

	void OpenGLCommandBuffer::Dispatch(uint32_t groupX, uint32_t groupY, uint32_t groupZ)
	{
		glDispatchCompute(groupX, groupY, groupZ);
	}

	void OpenGLCommandBuffer::PipelineBarrier(const std::vector<ResourceBarrier>& /*barriers*/)
	{
		glMemoryBarrier(GL_ALL_BARRIER_BITS);
	}

	void OpenGLCommandBuffer::CopyBuffer(const Handle<Buffer>& src, const Handle<Buffer>& dst,
		uint64_t srcOffset, uint64_t dstOffset, uint64_t size)
	{
		const auto srcGl = std::dynamic_pointer_cast<OpenGLBuffer>(src);
		const auto dstGl = std::dynamic_pointer_cast<OpenGLBuffer>(dst);
		if (!srcGl || !dstGl)
			return;
		glBindBuffer(GL_COPY_READ_BUFFER, srcGl->GetID());
		glBindBuffer(GL_COPY_WRITE_BUFFER, dstGl->GetID());
		glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, srcOffset, dstOffset, size);
	}

	void OpenGLCommandBuffer::UpdateBuffer(const Handle<Buffer>& dst, const void* data, uint64_t size,
		uint64_t offset)
	{
		const auto dstGl = std::dynamic_pointer_cast<OpenGLBuffer>(dst);
		if (!dstGl || !data)
			return;
		// GL 立即模式:直接写缓冲(上下文由调用线程持有)。
		glNamedBufferSubData(dstGl->GetID(), offset, size, data);
	}

	void OpenGLCommandBuffer::CopyBufferToTexture(const Handle<Buffer>& src, const Handle<Texture>& dst,
		uint64_t srcOffset, uint32_t mip, uint32_t /*layer*/)
	{
		const auto srcGl = std::dynamic_pointer_cast<OpenGLBuffer>(src);
		const auto dstGl = std::dynamic_pointer_cast<OpenGLTexture>(dst);
		if (!srcGl || !dstGl)
			return;
		const auto& desc = dstGl->GetDesc();
		const auto extent = Extent3D{ std::max(1u, desc.Extent.Width >> mip), std::max(1u, desc.Extent.Height >> mip), std::max(1u, desc.Extent.Depth >> mip) };
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, srcGl->GetID());
		glTextureSubImage2D(dstGl->GetID(), mip, 0, 0, extent.Width, extent.Height,
			ToGLDataFormat(desc.Format), ToGLDataType(desc.Format), reinterpret_cast<const void*>(static_cast<uintptr_t>(srcOffset)));
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	}

	void OpenGLCommandBuffer::CopyTextureToBuffer(const Handle<Texture>& src, const Handle<Buffer>& dst,
		uint64_t dstOffset, uint32_t mip, uint32_t /*layer*/)
	{
		const auto srcGl = std::dynamic_pointer_cast<OpenGLTexture>(src);
		const auto dstGl = std::dynamic_pointer_cast<OpenGLBuffer>(dst);
		if (!srcGl || !dstGl)
			return;
		glBindBuffer(GL_PIXEL_PACK_BUFFER, dstGl->GetID());
		glGetTextureImage(srcGl->GetID(), mip, ToGLDataFormat(srcGl->GetDesc().Format),
			ToGLDataType(srcGl->GetDesc().Format), static_cast<GLsizei>(dstGl->GetDesc().Size - dstOffset),
			reinterpret_cast<void*>(static_cast<uintptr_t>(dstOffset)));
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	}

	void OpenGLCommandBuffer::CopyTexture(const Handle<Texture>& src, const Handle<Texture>& dst,
		uint32_t srcMip, uint32_t /*srcLayer*/, uint32_t dstMip, uint32_t /*dstLayer*/)
	{
		const auto srcGl = std::dynamic_pointer_cast<OpenGLTexture>(src);
		const auto dstGl = std::dynamic_pointer_cast<OpenGLTexture>(dst);
		if (!srcGl || !dstGl)
			return;
		const auto& srcDesc = srcGl->GetDesc();
		const auto& dstDesc = dstGl->GetDesc();
		const auto extent = Extent3D{
			std::min(std::max(1u, srcDesc.Extent.Width >> srcMip), std::max(1u, dstDesc.Extent.Width >> dstMip)),
			std::min(std::max(1u, srcDesc.Extent.Height >> srcMip), std::max(1u, dstDesc.Extent.Height >> dstMip)),
			1 };
		glCopyImageSubData(srcGl->GetID(), GL_TEXTURE_2D, srcMip, 0, 0, 0,
			dstGl->GetID(), GL_TEXTURE_2D, dstMip, 0, 0, 0,
			extent.Width, extent.Height, extent.Depth);
	}

	void OpenGLCommandBuffer::ResolveTexture(const Handle<Texture>& src, const Handle<Texture>& dst,
		uint32_t srcMip, uint32_t dstMip, uint32_t /*layer*/)
	{
		const auto srcGl = std::dynamic_pointer_cast<OpenGLTexture>(src);
		const auto dstGl = std::dynamic_pointer_cast<OpenGLTexture>(dst);
		if (!srcGl || !dstGl)
			return;
		GLuint readFbo = 0, drawFbo = 0;
		glCreateFramebuffers(1, &readFbo);
		glCreateFramebuffers(1, &drawFbo);
		glNamedFramebufferTexture(readFbo, GL_COLOR_ATTACHMENT0, srcGl->GetID(), srcMip);
		glNamedFramebufferTexture(drawFbo, GL_COLOR_ATTACHMENT0, dstGl->GetID(), dstMip);
		const auto extent = dstGl->GetDesc().Extent;
		glBlitNamedFramebuffer(readFbo, drawFbo, 0, 0,
			std::max(1u, srcGl->GetDesc().Extent.Width >> srcMip), std::max(1u, srcGl->GetDesc().Extent.Height >> srcMip),
			0, 0, std::max(1u, extent.Width >> dstMip), std::max(1u, extent.Height >> dstMip),
			GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glDeleteFramebuffers(1, &readFbo);
		glDeleteFramebuffers(1, &drawFbo);
	}

	void OpenGLCommandBuffer::GenerateMipmaps(const Handle<Texture>& texture)
	{
		const auto glTexture = std::dynamic_pointer_cast<OpenGLTexture>(texture);
		if (glTexture)
			glGenerateTextureMipmap(glTexture->GetID());
	}

	void OpenGLCommandBuffer::ResetQueryPool(const Handle<QueryPool>& pool, uint32_t first, uint32_t count)
	{
		const auto glPool = std::dynamic_pointer_cast<OpenGLQueryPool>(pool);
		if (!glPool)
			return;
		const uint32_t end = count == 0 ? glPool->GetCount() : std::min(glPool->GetCount(), first + count);
		for (uint32_t i = first; i < end; i++)
		{
			glBeginQuery(glPool->GetTarget(), glPool->GetQuery(i));
			glEndQuery(glPool->GetTarget());
		}
	}

	void OpenGLCommandBuffer::BeginQuery(const Handle<QueryPool>& pool, uint32_t index, QueryType type)
	{
		const auto glPool = std::dynamic_pointer_cast<OpenGLQueryPool>(pool);
		if (!glPool)
			return;
		const GLenum target = type == QueryType::Timestamp ? GL_TIMESTAMP : GL_ANY_SAMPLES_PASSED;
		glBeginQuery(target, glPool->GetQuery(index));
	}

	void OpenGLCommandBuffer::EndQuery(const Handle<QueryPool>& pool, uint32_t index)
	{
		const auto glPool = std::dynamic_pointer_cast<OpenGLQueryPool>(pool);
		if (!glPool)
			return;
		glEndQuery(glPool->GetTarget());
	}

	void OpenGLCommandBuffer::WriteTimestamp(const Handle<QueryPool>& pool, uint32_t index)
	{
		const auto glPool = std::dynamic_pointer_cast<OpenGLQueryPool>(pool);
		if (!glPool)
			return;
		glQueryCounter(glPool->GetQuery(index), GL_TIMESTAMP);
	}

	void OpenGLCommandBuffer::CopyQueryResults(const Handle<QueryPool>& pool, const Handle<Buffer>& dst,
		uint32_t first, uint32_t count)
	{
		const auto glPool = std::dynamic_pointer_cast<OpenGLQueryPool>(pool);
		const auto dstGl = std::dynamic_pointer_cast<OpenGLBuffer>(dst);
		if (!glPool || !dstGl)
			return;
		const uint32_t end = count == 0 ? glPool->GetCount() : std::min(glPool->GetCount(), first + count);
		std::vector<GLuint64> results(end - first);
		for (uint32_t i = first; i < end; i++)
			glGetQueryObjectui64v(glPool->GetQuery(i), GL_QUERY_RESULT, &results[i - first]);
		dstGl->SetData(results.data(), results.size() * sizeof(GLuint64));
	}
}

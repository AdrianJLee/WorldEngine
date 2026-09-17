#include "wldpch.h"
#include "OpenGLCommandBuffer.h"
#include "OpenGLHelpers.h"
#include "OpenGLBuffer.h"
#include "OpenGLDescriptorSet.h"
#include "OpenGLPipeline.h"
#include "OpenGLQueryPool.h"
#include "OpenGLRenderPass.h"
#include "OpenGLTexture.h"

#include <algorithm>

namespace World::Rhi::OpenGL
{
	namespace
	{
		// ---- GL 渲染诊断(无障碍化的一部分:agent 看不到屏幕,只能靠数据判断"画没画进去") ----
		// 诊断开关只读一次(逐 draw 调 getenv 会拖慢回放)。
		bool TraceDraws()
		{
			static const bool enabled = std::getenv("WLD_GL_TRACE_DRAW") != nullptr;
			return enabled;
		}

		const char* FboDumpDir()
		{
			static const char* dir = std::getenv("WLD_GL_DUMP_FBO");
			return (dir && *dir) ? dir : nullptr;
		}

		// 把"刚结束的渲染通道"的颜色附件直接写 PPM(不经过 RHI 纹理读回),用于区分
		// "通道根本没画进去"和"纹理读回读错了对象"。只抓 ≤512² 的离屏目标,每个 FBO 最多 2 张。
		void DumpCurrentFramebuffer(const char* dir, const char* passName)
		{
			GLint fbo = 0;
			GLint viewport[4] = { 0, 0, 0, 0 };
			glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
			glGetIntegerv(GL_VIEWPORT, viewport);
			const int width = viewport[2];
			const int height = viewport[3];
			if (fbo <= 0 || width <= 0 || height <= 0 || width > 1024 || height > 1024)
				return;
			static int dumped[128] = {};
			if (fbo >= 128 || dumped[fbo] >= 2)
				return;
			++dumped[fbo];
			GLint attached = 0;
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &attached);
			GLint depthAttached = 0;
			GLint depthType = 0;
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
				GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &depthAttached);
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
				GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &depthType);
			WLD_CORE_INFO("[gl-dump] pass='{0}' fbo={1} depthAttachment=name:{2} type:0x{3}",
				passName ? passName : "?", fbo, depthAttached, depthType);
			// 深度范围:min < 清值(1.0) 说明**有片元真的过了深度测试**,
			// 可用来区分"几何根本没光栅化"与"光栅化了但颜色没写进去"。
			std::vector<float> depth(static_cast<size_t>(width) * height);
			glReadPixels(0, 0, width, height, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
			float depthMin = 1.0f;
			float depthMax = 0.0f;
			for (float value : depth)
			{
				depthMin = std::min(depthMin, value);
				depthMax = std::max(depthMax, value);
			}
			WLD_CORE_INFO("[gl-dump] pass='{0}' fbo={1} status=0x{2} colorAttachmentTexture={3}",
				passName ? passName : "?", fbo, glCheckFramebufferStatus(GL_FRAMEBUFFER), attached);
			WLD_CORE_INFO("[gl-dump] pass='{0}' fbo={1} depthRange=[{2}, {3}]",
				passName ? passName : "?", fbo, depthMin, depthMax);
			std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
			glReadBuffer(GL_COLOR_ATTACHMENT0);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
			const std::string path = std::string(dir) + "/fbo-" + std::to_string(fbo) + "-"
				+ std::to_string(width) + "x" + std::to_string(height) + ".ppm";
			if (FILE* file = std::fopen(path.c_str(), "wb"))
			{
				std::fprintf(file, "P6\n%d %d\n255\n", width, height);
				for (int row = 0; row < height; ++row)
					std::fwrite(pixels.data() + static_cast<size_t>(height - 1 - row) * width * 3, 1,
						static_cast<size_t>(width) * 3, file);
				std::fclose(file);
				WLD_CORE_INFO("[gl-dump] wrote {0}", path);
			}
		}

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

	GLCommand& OpenGLCommandBuffer::Push(GLCommandKind kind)
	{
		GLCommand& command = m_Commands.emplace_back();
		command.Kind = kind;
		return command;
	}

	// ---- 录制:只写数据,不触碰 GL 状态机(可在任意线程执行) ----

	void OpenGLCommandBuffer::Begin()
	{
		m_Commands.clear();
		m_Recording = true;
	}

	void OpenGLCommandBuffer::End()
	{
		m_Recording = false;
	}

	void OpenGLCommandBuffer::BeginLabel(const std::string& label)
	{
		Push(GLCommandKind::BeginLabel).Label = label;
	}

	void OpenGLCommandBuffer::EndLabel()
	{
		Push(GLCommandKind::EndLabel);
	}

	void OpenGLCommandBuffer::BeginRenderPass(const Handle<RenderPass>& pass,
		const Handle<Framebuffer>& framebuffer, const std::vector<ClearValue>& clears)
	{
		GLCommand& command = Push(GLCommandKind::BeginRenderPass);
		command.Pass = pass;
		command.Framebuffer_ = framebuffer;
		command.Clears = clears;
	}

	void OpenGLCommandBuffer::NextSubpass()
	{
		// GL 后端单子通道执行;多子通道依赖与输入附件未降级。
	}

	void OpenGLCommandBuffer::EndRenderPass()
	{
		Push(GLCommandKind::EndRenderPass);
	}

	void OpenGLCommandBuffer::SetViewport(const Viewport& viewport)
	{
		Push(GLCommandKind::SetViewport).Viewport_ = viewport;
	}

	void OpenGLCommandBuffer::SetScissor(const Scissor& scissor)
	{
		Push(GLCommandKind::SetScissor).Scissor_ = scissor;
	}

	void OpenGLCommandBuffer::BindPipeline(const Handle<Pipeline>& pipeline)
	{
		Push(GLCommandKind::BindPipeline).Pipeline_ = pipeline;
	}

	void OpenGLCommandBuffer::BindDescriptorSet(const Handle<DescriptorSet>& set, uint32_t firstSet)
	{
		GLCommand& command = Push(GLCommandKind::BindDescriptorSet);
		command.DescriptorSet_ = set;
		command.FirstSet = firstSet;
	}

	void OpenGLCommandBuffer::BindVertexBuffer(uint32_t binding, const Handle<Buffer>& buffer, uint64_t offset)
	{
		GLCommand& command = Push(GLCommandKind::BindVertexBuffer);
		command.Binding = binding;
		command.BufferA = buffer;
		command.OffsetA = offset;
	}

	void OpenGLCommandBuffer::BindIndexBuffer(const Handle<Buffer>& buffer, uint64_t offset, IndexType indexType)
	{
		GLCommand& command = Push(GLCommandKind::BindIndexBuffer);
		command.BufferA = buffer;
		command.OffsetA = offset;
		command.IndexType_ = indexType;
	}

	void OpenGLCommandBuffer::PushConstants(ShaderStageFlags stages, uint32_t /*offset*/, uint32_t /*size*/, const void* /*data*/)
	{
		if (!m_PushConstantsWarned)
		{
			WLD_CORE_WARN("OpenGL backend does not implement push constants; use uniform buffers");
			m_PushConstantsWarned = true;
		}
		(void)stages;
	}

	void OpenGLCommandBuffer::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
	{
		GLCommand& command = Push(GLCommandKind::Draw);
		command.Count0 = vertexCount;
		command.Count1 = instanceCount;
		command.Count2 = firstVertex;
		command.Count3 = firstInstance;
	}

	void OpenGLCommandBuffer::DrawIndexed(uint32_t indexCount, uint32_t instanceCount,
		uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
	{
		GLCommand& command = Push(GLCommandKind::DrawIndexed);
		command.Count0 = indexCount;
		command.Count1 = instanceCount;
		command.Count2 = firstIndex;
		command.Signed0 = vertexOffset;
		command.Count3 = firstInstance;
	}

	void OpenGLCommandBuffer::DrawIndirect(const Handle<Buffer>& args, uint64_t offset, uint32_t drawCount, uint32_t stride)
	{
		GLCommand& command = Push(GLCommandKind::DrawIndirect);
		command.BufferA = args;
		command.OffsetA = offset;
		command.Count0 = drawCount;
		command.Count1 = stride;
	}

	void OpenGLCommandBuffer::DrawIndexedIndirect(const Handle<Buffer>& args, uint64_t offset, uint32_t drawCount, uint32_t stride)
	{
		GLCommand& command = Push(GLCommandKind::DrawIndexedIndirect);
		command.BufferA = args;
		command.OffsetA = offset;
		command.Count0 = drawCount;
		command.Count1 = stride;
	}

	void OpenGLCommandBuffer::Dispatch(uint32_t groupX, uint32_t groupY, uint32_t groupZ)
	{
		GLCommand& command = Push(GLCommandKind::Dispatch);
		command.Count0 = groupX;
		command.Count1 = groupY;
		command.Count2 = groupZ;
	}

	void OpenGLCommandBuffer::PipelineBarrier(const std::vector<ResourceBarrier>& barriers)
	{
		Push(GLCommandKind::PipelineBarrier).Barriers = barriers;
	}

	void OpenGLCommandBuffer::CopyBuffer(const Handle<Buffer>& src, const Handle<Buffer>& dst,
		uint64_t srcOffset, uint64_t dstOffset, uint64_t size)
	{
		GLCommand& command = Push(GLCommandKind::CopyBuffer);
		command.BufferA = src;
		command.BufferB = dst;
		command.OffsetA = srcOffset;
		command.OffsetB = dstOffset;
		command.Size = size;
	}

	void OpenGLCommandBuffer::UpdateBuffer(const Handle<Buffer>& dst, const void* data, uint64_t size, uint64_t offset)
	{
		if (!dst || !data || size == 0)
			return;
		GLCommand& command = Push(GLCommandKind::UpdateBuffer);
		command.BufferA = dst;
		command.OffsetA = offset;
		command.Size = size;
		command.Data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
	}

	void OpenGLCommandBuffer::CopyBufferToTexture(const Handle<Buffer>& src, const Handle<Texture>& dst,
		uint64_t srcOffset, uint32_t mip, uint32_t layer)
	{
		GLCommand& command = Push(GLCommandKind::CopyBufferToTexture);
		command.BufferA = src;
		command.TextureA = dst;
		command.OffsetA = srcOffset;
		command.Count0 = mip;
		command.Count1 = layer;
	}

	void OpenGLCommandBuffer::CopyTextureToBuffer(const Handle<Texture>& src, const Handle<Buffer>& dst,
		uint64_t dstOffset, uint32_t mip, uint32_t layer)
	{
		GLCommand& command = Push(GLCommandKind::CopyTextureToBuffer);
		command.TextureA = src;
		command.BufferA = dst;
		command.OffsetA = dstOffset;
		command.Count0 = mip;
		command.Count1 = layer;
	}

	void OpenGLCommandBuffer::CopyTexture(const Handle<Texture>& src, const Handle<Texture>& dst,
		uint32_t srcMip, uint32_t srcLayer, uint32_t dstMip, uint32_t dstLayer)
	{
		GLCommand& command = Push(GLCommandKind::CopyTexture);
		command.TextureA = src;
		command.TextureB = dst;
		command.Count0 = srcMip;
		command.Count1 = srcLayer;
		command.Count2 = dstMip;
		command.Count3 = dstLayer;
	}

	void OpenGLCommandBuffer::ResolveTexture(const Handle<Texture>& src, const Handle<Texture>& dst,
		uint32_t srcMip, uint32_t dstMip, uint32_t layer)
	{
		GLCommand& command = Push(GLCommandKind::ResolveTexture);
		command.TextureA = src;
		command.TextureB = dst;
		command.Count0 = srcMip;
		command.Count1 = dstMip;
		command.Count2 = layer;
	}

	void OpenGLCommandBuffer::GenerateMipmaps(const Handle<Texture>& texture)
	{
		Push(GLCommandKind::GenerateMipmaps).TextureA = texture;
	}

	void OpenGLCommandBuffer::ResetQueryPool(const Handle<QueryPool>& pool, uint32_t first, uint32_t count)
	{
		GLCommand& command = Push(GLCommandKind::ResetQueryPool);
		command.QueryPool_ = pool;
		command.Count0 = first;
		command.Count1 = count;
	}

	void OpenGLCommandBuffer::BeginQuery(const Handle<QueryPool>& pool, uint32_t index, QueryType type)
	{
		GLCommand& command = Push(GLCommandKind::BeginQuery);
		command.QueryPool_ = pool;
		command.Count0 = index;
		command.QueryType_ = type;
	}

	void OpenGLCommandBuffer::EndQuery(const Handle<QueryPool>& pool, uint32_t index)
	{
		GLCommand& command = Push(GLCommandKind::EndQuery);
		command.QueryPool_ = pool;
		command.Count0 = index;
	}

	void OpenGLCommandBuffer::WriteTimestamp(const Handle<QueryPool>& pool, uint32_t index)
	{
		GLCommand& command = Push(GLCommandKind::WriteTimestamp);
		command.QueryPool_ = pool;
		command.Count0 = index;
	}

	void OpenGLCommandBuffer::CopyQueryResults(const Handle<QueryPool>& pool, const Handle<Buffer>& dst,
		uint32_t first, uint32_t count)
	{
		GLCommand& command = Push(GLCommandKind::CopyQueryResults);
		command.QueryPool_ = pool;
		command.BufferA = dst;
		command.Count0 = first;
		command.Count1 = count;
	}

	// ---- 回放:渲染线程执行,顺序与旧"立即模式"实现逐一对应 ----

	void OpenGLCommandBuffer::Replay()
	{
		Handle<Pipeline> currentPipeline;
		IndexType indexType = IndexType::UInt32;
		uint64_t indexBufferOffset = 0;
		const char* currentPassName = nullptr;

		for (const GLCommand& command : m_Commands)
		{
			switch (command.Kind)
			{
				case GLCommandKind::BeginLabel:
					glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, static_cast<GLsizei>(command.Label.size()),
						command.Label.c_str());
					break;
				case GLCommandKind::EndLabel:
					glPopDebugGroup();
					break;
				case GLCommandKind::BeginRenderPass:
				{
					const auto glFramebuffer = std::dynamic_pointer_cast<OpenGLFramebuffer>(command.Framebuffer_);
					const GLuint fbo = glFramebuffer ? glFramebuffer->GetID() : 0;
					glBindFramebuffer(GL_FRAMEBUFFER, fbo);
					if (!command.Pass)
						break;
					const auto& desc = command.Pass->GetDesc();
					currentPassName = desc.DebugName.c_str();
					if (desc.Subpasses.empty())
						break;
					const auto& subpass = desc.Subpasses[0];
					Extent2D extent = (glFramebuffer && command.Framebuffer_)
						? command.Framebuffer_->GetDesc().Extent : Extent2D{};
					if (extent.Width == 0 || extent.Height == 0)
						extent = Extent2D{ 1, 1 };
					glViewport(0, 0, extent.Width, extent.Height);
					glScissor(0, 0, extent.Width, extent.Height);
					glEnable(GL_SCISSOR_TEST);

					const size_t clearCount = command.Clears.size();
					for (size_t i = 0; i < subpass.ColorAttachments.size(); i++)
					{
						const uint32_t attachmentIndex = subpass.ColorAttachments[i].Index;
						const auto& attachment = desc.Attachments[attachmentIndex];
						if (attachment.Load != LoadOp::Clear)
							continue;
						// 关键:glClearBuffer* 受**写入掩码**影响 —— 上一个绑定管线的 colorMask/depthMask
						// 会静默吞掉这次 clear(实测:透明管线留下 depthMask=FALSE 后,下一个通道的
						// 深度清零被跳过 → 深度全是 0 → LEQUAL 拒绝所有片元 → 画面只剩清屏色)。
						// 清除前显式打开写掩码,后续管线绑定会重新设置自己的状态。
						glColorMaski(static_cast<GLuint>(i), GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
						ClearColor color {};
						if (attachmentIndex < clearCount)
							color = command.Clears[attachmentIndex].Color;
						if (IsIntegerFormat(attachment.Format))
							glClearNamedFramebufferiv(fbo, GL_COLOR, static_cast<GLint>(i),
								reinterpret_cast<const GLint*>(&color));
						else
							glClearNamedFramebufferfv(fbo, GL_COLOR, static_cast<GLint>(i), &color.R);
					}
					if (subpass.HasDepthStencil())
					{
						const auto& attachment = desc.Attachments[subpass.DepthStencilAttachment.Index];
						if (attachment.Load == LoadOp::Clear)
						{
							// 同上:深度/模板清零同样受 depthMask / stencilMask 影响。
							glDepthMask(GL_TRUE);
							glStencilMask(0xFF);
							ClearDepthStencil ds {};
							if (subpass.DepthStencilAttachment.Index < clearCount &&
								command.Clears[subpass.DepthStencilAttachment.Index].IsDepthStencil)
								ds = command.Clears[subpass.DepthStencilAttachment.Index].DepthStencil;
							if (TraceDraws())
								WLD_CORE_INFO("[gl-clear] fbo={0} depthIndex={1} clearCount={2} depth={3} stencil={4}",
									fbo, subpass.DepthStencilAttachment.Index, clearCount, ds.Depth, ds.Stencil);
							// 用 DSA 版本按 FBO 直接清除(不依赖当前绑定的 draw framebuffer 状态)。
							glClearNamedFramebufferfi(fbo, GL_DEPTH_STENCIL, 0, ds.Depth, static_cast<GLint>(ds.Stencil));
						}
					}
					break;
				}
				case GLCommandKind::EndRenderPass:
					if (const char* dumpDir = FboDumpDir())
						DumpCurrentFramebuffer(dumpDir, currentPassName);
					glBindFramebuffer(GL_FRAMEBUFFER, 0);
					break;
				case GLCommandKind::SetViewport:
					glViewport(static_cast<GLint>(command.Viewport_.X), static_cast<GLint>(command.Viewport_.Y),
						static_cast<GLsizei>(command.Viewport_.Width), static_cast<GLsizei>(command.Viewport_.Height));
					glDepthRange(command.Viewport_.MinDepth, command.Viewport_.MaxDepth);
					break;
				case GLCommandKind::SetScissor:
					glEnable(GL_SCISSOR_TEST);
					glScissor(command.Scissor_.X, command.Scissor_.Y, command.Scissor_.Width, command.Scissor_.Height);
					break;
				case GLCommandKind::BindPipeline:
					currentPipeline = command.Pipeline_;
					if (currentPipeline)
						std::static_pointer_cast<OpenGLPipeline>(currentPipeline)->Bind();
					break;
				case GLCommandKind::BindDescriptorSet:
					if (command.DescriptorSet_)
						std::static_pointer_cast<OpenGLDescriptorSet>(command.DescriptorSet_)->Bind();
					break;
				case GLCommandKind::BindVertexBuffer:
				{
					if (!currentPipeline)
						break;
					const auto pipeline = std::static_pointer_cast<OpenGLPipeline>(currentPipeline);
					const auto glBuffer = std::dynamic_pointer_cast<OpenGLBuffer>(command.BufferA);
					if (!glBuffer)
						break;
					uint32_t stride = 0;
					for (const auto& vertexBinding : pipeline->GetDesc().VertexBindings)
						if (vertexBinding.Binding == command.Binding)
							stride = vertexBinding.Stride;
					glVertexArrayVertexBuffer(pipeline->GetVertexArray(), command.Binding, glBuffer->GetID(),
						static_cast<GLintptr>(command.OffsetA), static_cast<GLsizei>(stride));
					break;
				}
				case GLCommandKind::BindIndexBuffer:
				{
					indexType = command.IndexType_;
					indexBufferOffset = command.OffsetA;
					if (!currentPipeline)
						break;
					const auto pipeline = std::static_pointer_cast<OpenGLPipeline>(currentPipeline);
					const auto glBuffer = std::dynamic_pointer_cast<OpenGLBuffer>(command.BufferA);
					if (glBuffer)
						glVertexArrayElementBuffer(pipeline->GetVertexArray(), glBuffer->GetID());
					break;
				}
				case GLCommandKind::Draw:
				{
					GLenum topology = GL_TRIANGLES;
					if (currentPipeline)
						topology = ToGLTopology(std::static_pointer_cast<OpenGLPipeline>(currentPipeline)->GetDesc().Topology);
					if (command.Count1 > 1 || command.Count3 > 0)
						glDrawArraysInstancedBaseInstance(topology, command.Count2, command.Count0,
							command.Count1, command.Count3);
					else
						glDrawArrays(topology, command.Count2, command.Count0);
					break;
				}
				case GLCommandKind::DrawIndexed:
				{
					GLenum topology = GL_TRIANGLES;
					if (currentPipeline)
						topology = ToGLTopology(std::static_pointer_cast<OpenGLPipeline>(currentPipeline)->GetDesc().Topology);
					const GLenum type = indexType == IndexType::UInt16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
					const void* offset = reinterpret_cast<const void*>(static_cast<uintptr_t>(indexBufferOffset) +
						static_cast<uintptr_t>(command.Count2) *
						(indexType == IndexType::UInt16 ? sizeof(uint16_t) : sizeof(uint32_t)));
					// WLD_GL_TRACE_DRAW=1:记录一次绘制的 program/VAO/FBO/视口/裁剪与关键状态。
					// "命令录了、没报错、画面却是清屏色"时,只有这些数据能区分
					// "几何被剔除/深度拒绝/颜色写被关/画到了别的目标"。
					GLint traceFbo = 0;
					GLint traceViewport[4] = { 0, 0, 0, 0 };
					if (TraceDraws())
					{
						glGetIntegerv(GL_FRAMEBUFFER_BINDING, &traceFbo);
						glGetIntegerv(GL_VIEWPORT, traceViewport);
						// 只记录离屏目标(场景目标 / 预览目标 / 独立窗口后端目标),
						// 窗口默认帧缓冲(fbo=0)的 UI 绘制会瞬间打满条数上限。
						// 大网格(预览球 6912 索引)单独计数,避免被 UI 绘制挤掉。
						static int tracedOther = 0;
						static std::unordered_map<GLint, int> tracedMeshPerFbo;
						const bool bigMesh = command.Count0 >= 4096;
						// 大网格(预览球)按 FBO 分别限流:每个离屏目标都能留下前 10 次绘制,
						// 否则第一个面板的逐帧绘制会把条数吃光,看不到第二个面板。
						const bool log = traceFbo != 0
							&& (bigMesh ? tracedMeshPerFbo[traceFbo] < 10 : tracedOther < 120);
						if (log)
						{
							GLint program = 0, vao = 0, fbo = 0, viewport[4] = { 0 }, scissor[4] = { 0 };
							glGetIntegerv(GL_CURRENT_PROGRAM, &program);
							glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
							fbo = traceFbo;
							for (int i = 0; i < 4; ++i)
								viewport[i] = traceViewport[i];
							glGetIntegerv(GL_SCISSOR_BOX, scissor);
							GLint depthFunc = 0, colorMask[4] = { 0 };
							glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
							glGetIntegerv(GL_COLOR_WRITEMASK, colorMask);
							WLD_CORE_INFO("[gl-draw] program={0} vao={1} fbo={2} count={3} vp=({4},{5},{6},{7}) "
								"scissor=({8},{9},{10},{11}) blend0={12} depthTest={13} depthFunc=0x{14} colorMask={15}{16}{17}{18} ctx={19}",
								program, vao, fbo, command.Count0, viewport[0], viewport[1], viewport[2], viewport[3],
								scissor[0], scissor[1], scissor[2], scissor[3],
								glIsEnabledi(GL_BLEND, 0) != GL_FALSE ? 1 : 0,
								glIsEnabled(GL_DEPTH_TEST) != GL_FALSE ? 1 : 0, depthFunc,
								colorMask[0], colorMask[1], colorMask[2], colorMask[3],
								reinterpret_cast<uintptr_t>(wglGetCurrentContext()));
							if (bigMesh)
								++tracedMeshPerFbo[traceFbo];
							else
								++tracedOther;
						}
					}
					glDrawElementsInstancedBaseVertexBaseInstance(topology, command.Count0, type, offset,
						command.Count1, command.Signed0, command.Count3);
					break;
				}
				case GLCommandKind::DrawIndirect:
				{
					const auto glBuffer = std::dynamic_pointer_cast<OpenGLBuffer>(command.BufferA);
					if (!glBuffer)
						break;
					glBindBuffer(GL_DRAW_INDIRECT_BUFFER, glBuffer->GetID());
					GLenum topology = GL_TRIANGLES;
					if (currentPipeline)
						topology = ToGLTopology(std::static_pointer_cast<OpenGLPipeline>(currentPipeline)->GetDesc().Topology);
					const auto ptr = reinterpret_cast<const void*>(static_cast<uintptr_t>(command.OffsetA));
					if (command.Count0 > 1)
						glMultiDrawArraysIndirect(topology, ptr, command.Count0, command.Count1);
					else
						glDrawArraysIndirect(topology, ptr);
					break;
				}
				case GLCommandKind::DrawIndexedIndirect:
				{
					const auto glBuffer = std::dynamic_pointer_cast<OpenGLBuffer>(command.BufferA);
					if (!glBuffer)
						break;
					glBindBuffer(GL_DRAW_INDIRECT_BUFFER, glBuffer->GetID());
					GLenum topology = GL_TRIANGLES;
					if (currentPipeline)
						topology = ToGLTopology(std::static_pointer_cast<OpenGLPipeline>(currentPipeline)->GetDesc().Topology);
					const auto ptr = reinterpret_cast<const void*>(static_cast<uintptr_t>(command.OffsetA));
					const GLenum type = indexType == IndexType::UInt16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
					if (command.Count0 > 1)
						glMultiDrawElementsIndirect(topology, type, ptr, command.Count0, command.Count1);
					else
						glDrawElementsIndirect(topology, type, ptr);
					break;
				}
				case GLCommandKind::Dispatch:
					glDispatchCompute(command.Count0, command.Count1, command.Count2);
					break;
				case GLCommandKind::PipelineBarrier:
					glMemoryBarrier(GL_ALL_BARRIER_BITS);
					break;
				case GLCommandKind::CopyBuffer:
				{
					const auto srcGl = std::dynamic_pointer_cast<OpenGLBuffer>(command.BufferA);
					const auto dstGl = std::dynamic_pointer_cast<OpenGLBuffer>(command.BufferB);
					if (!srcGl || !dstGl)
						break;
					glBindBuffer(GL_COPY_READ_BUFFER, srcGl->GetID());
					glBindBuffer(GL_COPY_WRITE_BUFFER, dstGl->GetID());
					glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
						command.OffsetA, command.OffsetB, command.Size);
					break;
				}
				case GLCommandKind::UpdateBuffer:
				{
					const auto dstGl = std::dynamic_pointer_cast<OpenGLBuffer>(command.BufferA);
					if (dstGl && !command.Data.empty())
						glNamedBufferSubData(dstGl->GetID(), command.OffsetA, command.Data.size(), command.Data.data());
					break;
				}
				case GLCommandKind::CopyBufferToTexture:
				{
					const auto srcGl = std::dynamic_pointer_cast<OpenGLBuffer>(command.BufferA);
					const auto dstGl = std::dynamic_pointer_cast<OpenGLTexture>(command.TextureA);
					if (!srcGl || !dstGl)
						break;
					const auto& desc = dstGl->GetDesc();
					const uint32_t mip = command.Count0;
					const auto extent = Extent3D{ std::max(1u, desc.Extent.Width >> mip),
						std::max(1u, desc.Extent.Height >> mip), std::max(1u, desc.Extent.Depth >> mip) };
					glBindBuffer(GL_PIXEL_UNPACK_BUFFER, srcGl->GetID());
					glTextureSubImage2D(dstGl->GetID(), mip, 0, 0, extent.Width, extent.Height,
						ToGLDataFormat(desc.Format), ToGLDataType(desc.Format),
						reinterpret_cast<const void*>(static_cast<uintptr_t>(command.OffsetA)));
					glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
					break;
				}
				case GLCommandKind::CopyTextureToBuffer:
				{
					const auto srcGl = std::dynamic_pointer_cast<OpenGLTexture>(command.TextureA);
					const auto dstGl = std::dynamic_pointer_cast<OpenGLBuffer>(command.BufferA);
					if (!srcGl || !dstGl)
						break;
					const uint32_t mip = command.Count0;
					if (TraceDraws())
						WLD_CORE_INFO("[gl-readback] texture={0} mip={1} bytes={2}", srcGl->GetID(), mip,
							dstGl->GetDesc().Size - command.OffsetA);
					glBindBuffer(GL_PIXEL_PACK_BUFFER, dstGl->GetID());
					glGetTextureImage(srcGl->GetID(), mip, ToGLDataFormat(srcGl->GetDesc().Format),
						ToGLDataType(srcGl->GetDesc().Format),
						static_cast<GLsizei>(dstGl->GetDesc().Size - command.OffsetA),
						reinterpret_cast<void*>(static_cast<uintptr_t>(command.OffsetA)));
					glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
					break;
				}
				case GLCommandKind::CopyTexture:
				{
					const auto srcGl = std::dynamic_pointer_cast<OpenGLTexture>(command.TextureA);
					const auto dstGl = std::dynamic_pointer_cast<OpenGLTexture>(command.TextureB);
					if (!srcGl || !dstGl)
						break;
					const auto& srcDesc = srcGl->GetDesc();
					const auto& dstDesc = dstGl->GetDesc();
					const uint32_t srcMip = command.Count0;
					const uint32_t dstMip = command.Count2;
					const auto extent = Extent3D{
						std::min(std::max(1u, srcDesc.Extent.Width >> srcMip), std::max(1u, dstDesc.Extent.Width >> dstMip)),
						std::min(std::max(1u, srcDesc.Extent.Height >> srcMip), std::max(1u, dstDesc.Extent.Height >> dstMip)),
						1 };
					glCopyImageSubData(srcGl->GetID(), GL_TEXTURE_2D, srcMip, 0, 0, 0,
						dstGl->GetID(), GL_TEXTURE_2D, dstMip, 0, 0, 0,
						extent.Width, extent.Height, extent.Depth);
					break;
				}
				case GLCommandKind::ResolveTexture:
				{
					const auto srcGl = std::dynamic_pointer_cast<OpenGLTexture>(command.TextureA);
					const auto dstGl = std::dynamic_pointer_cast<OpenGLTexture>(command.TextureB);
					if (!srcGl || !dstGl)
						break;
					const uint32_t srcMip = command.Count0;
					const uint32_t dstMip = command.Count1;
					GLuint readFbo = 0, drawFbo = 0;
					glCreateFramebuffers(1, &readFbo);
					glCreateFramebuffers(1, &drawFbo);
					glNamedFramebufferTexture(readFbo, GL_COLOR_ATTACHMENT0, srcGl->GetID(), srcMip);
					glNamedFramebufferTexture(drawFbo, GL_COLOR_ATTACHMENT0, dstGl->GetID(), dstMip);
					const auto extent = dstGl->GetDesc().Extent;
					glBlitNamedFramebuffer(readFbo, drawFbo, 0, 0,
						std::max(1u, srcGl->GetDesc().Extent.Width >> srcMip),
						std::max(1u, srcGl->GetDesc().Extent.Height >> srcMip),
						0, 0, std::max(1u, extent.Width >> dstMip), std::max(1u, extent.Height >> dstMip),
						GL_COLOR_BUFFER_BIT, GL_NEAREST);
					glDeleteFramebuffers(1, &readFbo);
					glDeleteFramebuffers(1, &drawFbo);
					break;
				}
				case GLCommandKind::GenerateMipmaps:
				{
					const auto glTexture = std::dynamic_pointer_cast<OpenGLTexture>(command.TextureA);
					if (glTexture)
						glGenerateTextureMipmap(glTexture->GetID());
					break;
				}
				case GLCommandKind::ResetQueryPool:
				{
					const auto glPool = std::dynamic_pointer_cast<OpenGLQueryPool>(command.QueryPool_);
					if (!glPool)
						break;
					const uint32_t end = command.Count1 == 0 ? glPool->GetCount()
						: std::min(glPool->GetCount(), command.Count0 + command.Count1);
					for (uint32_t i = command.Count0; i < end; i++)
					{
						glBeginQuery(glPool->GetTarget(), glPool->GetQuery(i));
						glEndQuery(glPool->GetTarget());
					}
					break;
				}
				case GLCommandKind::BeginQuery:
				{
					const auto glPool = std::dynamic_pointer_cast<OpenGLQueryPool>(command.QueryPool_);
					if (!glPool)
						break;
					const GLenum target = command.QueryType_ == QueryType::Timestamp ? GL_TIMESTAMP : GL_ANY_SAMPLES_PASSED;
					glBeginQuery(target, glPool->GetQuery(command.Count0));
					break;
				}
				case GLCommandKind::EndQuery:
				{
					const auto glPool = std::dynamic_pointer_cast<OpenGLQueryPool>(command.QueryPool_);
					if (glPool)
						glEndQuery(glPool->GetTarget());
					break;
				}
				case GLCommandKind::WriteTimestamp:
				{
					const auto glPool = std::dynamic_pointer_cast<OpenGLQueryPool>(command.QueryPool_);
					if (glPool)
						glQueryCounter(glPool->GetQuery(command.Count0), GL_TIMESTAMP);
					break;
				}
				case GLCommandKind::CopyQueryResults:
				{
					const auto glPool = std::dynamic_pointer_cast<OpenGLQueryPool>(command.QueryPool_);
					const auto dstGl = std::dynamic_pointer_cast<OpenGLBuffer>(command.BufferA);
					if (!glPool || !dstGl)
						break;
					const uint32_t end = command.Count1 == 0 ? glPool->GetCount()
						: std::min(glPool->GetCount(), command.Count0 + command.Count1);
					std::vector<GLuint64> results(end - command.Count0);
					for (uint32_t i = command.Count0; i < end; i++)
						glGetQueryObjectui64v(glPool->GetQuery(i), GL_QUERY_RESULT, &results[i - command.Count0]);
					dstGl->SetData(results.data(), results.size() * sizeof(GLuint64));
					break;
				}
			}
		}
		m_Commands.clear();
	}
}

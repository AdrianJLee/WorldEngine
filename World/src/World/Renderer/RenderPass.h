#pragma once
#include "Framebuffer.h"

#include <glm/glm.hpp>

namespace World
{
	struct RenderPassSpecification
	{
		Ref<Framebuffer> TargetFramebuffer;
		glm::vec4 ClearColor = { 0.1f, 0.1f, 0.1f, 1.0f };
		bool ClearOnColor = true;
		bool ClearOnDepth = true;

		// 以后可以添加模板缓冲清除、多附件清除等

	};
	class RenderPass
	{
	public:
		static Ref<RenderPass> Create(const RenderPassSpecification& spec);
	public:
		virtual ~RenderPass() = default;

		virtual const RenderPassSpecification& GetSpecification() const = 0;
		virtual RenderPassSpecification& GetSpecification() = 0;
	};
}
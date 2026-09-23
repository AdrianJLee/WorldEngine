#pragma once

#include "World/RHI/RhiSync.h"

#include <glad/glad.h>

namespace World::Rhi::OpenGL
{
	class OpenGLQueryPool : public QueryPool
	{
	public:
		OpenGLQueryPool(QueryType type, uint32_t count);
		~OpenGLQueryPool() override;

		QueryType GetType() const override { return m_Type; }
		uint32_t GetCount() const override { return m_Count; }

		GLuint GetQuery(uint32_t index) const { return m_Queries[index]; }
		GLenum GetTarget() const { return m_Target; }

	private:
		QueryType m_Type;
		uint32_t m_Count;
		GLenum m_Target;
		std::vector<GLuint> m_Queries;
	};
}

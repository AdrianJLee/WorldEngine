#include "wldpch.h"
#include "OpenGLQueryPool.h"

namespace World::Rhi::OpenGL
{
	OpenGLQueryPool::OpenGLQueryPool(QueryType type, uint32_t count)
		: m_Type(type), m_Count(count), m_Target(type == QueryType::Timestamp ? GL_TIMESTAMP : GL_ANY_SAMPLES_PASSED)
	{
		m_Queries.resize(count);
		glGenQueries(count, m_Queries.data());
	}

	OpenGLQueryPool::~OpenGLQueryPool()
	{
		if (!m_Queries.empty())
			glDeleteQueries(m_Count, m_Queries.data());
	}
}

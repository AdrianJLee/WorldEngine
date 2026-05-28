#pragma once
#include <glm/glm.hpp>
#include "World/Reflection/Reflection.h"
namespace World
{
	class Camera
	{
		REFLECT_BODY(Camera, TypeCategory::NormalClass);

	public:
		Camera() {};
		Camera(const glm::mat4& projectionMatrix)
			:m_ProjectionMatrix(projectionMatrix)
		{};

		const glm::mat4& GetProjectionMatrix() const { return m_ProjectionMatrix; }
	protected:
		PROPERTY(m_ProjectionMatrix);
		glm::mat4 m_ProjectionMatrix;
	};
}
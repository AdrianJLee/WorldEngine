#pragma once
#include <glm/glm.hpp>
namespace World
{
	class Camera
	{
	public:
		Camera() {};
		Camera(const glm::mat4& projectionMatrix)
			:m_ProjectionMatrix(projectionMatrix)
		{
		};

		const glm::mat4& GetProjectionMatrix() const { return m_ProjectionMatrix; }
	protected:
		glm::mat4 m_ProjectionMatrix;
	};
}
#pragma once

// P6:固定步长 → 渲染插值的纯函数。
//
// 为什么单独一个头:这是**可单测**的数学(端点、中点、旋转方向),而 SceneRenderer 的
// 抽取路径需要完整渲染器才能驱动。把它提出来让 `render-extract` 与测试共用同一份实现,
// 避免"测试验的是一份、产品跑的是另一份"。
//
// 口径(Unity `Rigidbody.interpolation` / Bevy `TransformInterpolation` 同一):
//   * 只插值**表现层**变换,不动权威模拟数据;
//   * 位置线性、旋转球面(slerp)、缩放取 `current`(物理步不改缩放);
//   * alpha ∈ [0,1]:0 = 上一固定步,1 = 当前位姿。

#include "World/Core/Export.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace World
{
	// 取矩阵第 index 列的归一化方向(退化时回退到单位轴);
	// 用于从可能带缩放的矩阵里提取纯旋转。
	inline glm::vec3 NormalizedMatrixColumn(const glm::mat4& matrix, int index)
	{
		const glm::vec3 column(matrix[index]);
		const float length = glm::length(column);
		if (length > 1e-8f) return column / length;
		return glm::vec3(index == 0 ? 1.0f : 0.0f, index == 1 ? 1.0f : 0.0f, index == 2 ? 1.0f : 0.0f);
	}

	inline glm::mat4 InterpolateRigidTransform(const glm::mat4& previous, const glm::mat4& current, float alpha)
	{
		// 先夹取:alpha 来自累加器余量,理论上已归一,但纯函数不该依赖调用方。
		if (alpha <= 0.0f) return previous;
		if (alpha >= 1.0f) return current;

		const glm::vec3 previousPosition(previous[3]);
		const glm::vec3 currentPosition(current[3]);
		const glm::mat3 previousRotation(NormalizedMatrixColumn(previous, 0),
			NormalizedMatrixColumn(previous, 1), NormalizedMatrixColumn(previous, 2));
		const glm::mat3 currentRotation(NormalizedMatrixColumn(current, 0),
			NormalizedMatrixColumn(current, 1), NormalizedMatrixColumn(current, 2));
		const glm::quat rotation = glm::slerp(glm::quat_cast(previousRotation), glm::quat_cast(currentRotation), alpha);
		const glm::vec3 scale(glm::length(glm::vec3(current[0])), glm::length(glm::vec3(current[1])),
			glm::length(glm::vec3(current[2])));

		glm::mat4 result = glm::translate(glm::mat4(1.0f), glm::mix(previousPosition, currentPosition, alpha))
			* glm::mat4_cast(rotation);
		result[0] *= scale.x;
		result[1] *= scale.y;
		result[2] *= scale.z;
		return result;
	}
}

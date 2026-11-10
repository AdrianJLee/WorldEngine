#pragma once

#include "World/Core/Export.h"
#include <glm/vec3.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace World
{
	class ScriptBindingContext;
	struct ScriptServiceBinding;

	struct AiStimulusRecord
	{
		glm::vec3 position { 0.0f };
		float radius { 0.0f };
		float loudness { 0.0f };
		std::string tag;
		double timestamp { 0.0 };
	};

	// Luau 音频与音乐全局服务绑定 (只读全局表 Audio 与 Music, 支持 3D/2D 空间声像、3D 次世代空间参数与 DSP 时钟、Combo 连击音高递进、FFT频谱与AI声学刺激)
	WLD_API bool RegisterAudioBindings(ScriptBindingContext& bindings, std::string* error = nullptr);

	// 音频面 API 描述表 (Audio 与 Music 两个只读全局表),供运行时注册与 LuaStubGenerator 存根生成共用
	WLD_API const ScriptServiceBinding* AudioServiceBindings(std::size_t* count);

	// 获取最近广播的 AI 声学刺激记录 (供 C++ AI 感知系统与单元测试消费)
	WLD_API std::vector<AiStimulusRecord> GetRecentAiStimuli();
	WLD_API void ClearRecentAiStimuli();

	// RTPC 实时参数控制与查询 (供全局与声部参数映射及单元测试消费)
	WLD_API void SetAudioParameter(const std::string& name, float value);
	WLD_API float GetAudioParameter(const std::string& name);
	WLD_API void SetVoiceAudioParameter(uint32_t voiceHandle, const std::string& name, float value);
	WLD_API float GetVoiceAudioParameter(uint32_t voiceHandle, const std::string& name);
	WLD_API void ClearAudioParameters();
}

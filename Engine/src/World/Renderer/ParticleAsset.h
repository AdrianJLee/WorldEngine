#pragma once

#include "World/Core/Export.h"
#include "World/Renderer/ParticleSystem.h"
#include "World/Renderer/VfxModule.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace World
{
	enum class VfxEmissionMode : uint8_t
	{
		Continuous = 0, // 持续发射 (Continuous Rate: 火焰/浓烟/火花/喷泉)
		Burst = 1,      // 单次/循环瞬时爆发 (Instant Burst: 冲击波/闪光)
		Single = 2      // 单体常驻/非粒子 (Single Mesh/Decal/Shield: 法阵/护盾/光环网格，零持续粒子发射)
	};

	struct WLD_API ParticleEmitterLayer
	{
		std::string Name = "Emitter";
		uint32_t MaxParticles = 500;
		float Lifetime = 2.0f;
		float EmissionRate = 50.0f;
		VfxEmissionMode EmissionMode = VfxEmissionMode::Continuous;
		uint32_t SingleCount = 1; // 单体常驻或瞬时爆发时的数量 (默认 1)
		ParticleEmitterType EmitterType = ParticleEmitterType::Point;
		bool Enabled = true;
		ParticleRenderMode FacingMode = ParticleRenderMode::Billboard;
		bool LockToXY = false;
		std::string MeshPath;

		ParticleMaterialSettings Material;
		ParticleCollisionSettings Collision;

		glm::vec3 Gravity { 0.0f, -9.8f, 0.0f };
		float LinearDrag = 0.0f;
		float VortexStrength = 0.0f;
		glm::vec3 VortexAxis { 0.0f, 1.0f, 0.0f };
		float TurbulenceStrength = 0.0f;
		float TurbulenceFrequency = 1.0f;
		float Speed = 5.0f;
		float SpeedVariance = 1.0f;
		float SizeStart = 0.5f;
		float SizeEnd = 0.1f;
		glm::vec4 ColorStart { 1.0f };
		glm::vec4 ColorEnd { 1.0f, 1.0f, 1.0f, 0.0f };

		std::vector<ParticleBurst> Bursts;
		std::vector<SubEmitterConfig> SubEmitters;

		// 工业级 Slang 模块化着色器堆栈与自定义代码 (Slang Modular Pipeline)
		std::vector<VfxModule> Modules;
		std::string CustomSlangShaderOverride;
		bool UseCustomShaderOverride = false;

		void EnsureDefaultModules();
	};

	struct WLD_API ParticleAsset
	{
		uint64_t Guid = 0;
		std::string Name = "UnnamedVFX";
		std::string AssetPath;
		uint32_t Seed = 1337;
		float Duration = 0.0f;
		bool Looping = true;
		float WarmupTime = 0.0f;
		float CullDistance = 60.0f;
		bool PauseWhenOffscreen = true;

		std::unordered_map<std::string, float> UserParamsFloat;
		std::unordered_map<std::string, glm::vec4> UserParamsVec4;

		std::vector<ParticleEmitterLayer> Emitters;
	};

	class WLD_API ParticleAssetSerializer
	{
	public:
		static bool Serialize(const ParticleAsset& asset, const std::string& filepath);
		static bool Deserialize(const std::string& filepath, ParticleAsset& outAsset);
		static std::string SerializeToString(const ParticleAsset& asset);
		static bool DeserializeFromString(const std::string& yamlString, ParticleAsset& outAsset);
	};

	// 解析 VFX 资产在磁盘上的绝对路径(优先项目内容根，次之引擎全局内容库 Engine/content/vfx/)
	WLD_API std::filesystem::path ResolveVfxDiskPath(const std::string& path);

	class WLD_API ParticleAssetRegistry
	{
	public:
		static ParticleAssetRegistry& Get();

		uint64_t RegisterAsset(const std::string& filepath);
		uint64_t RegisterAssetFromMemory(const std::string& name, const ParticleAsset& asset);
		const ParticleAsset* GetAsset(uint64_t guid) const;
		ParticleAsset* GetAssetMut(uint64_t guid);
		const ParticleAsset* GetAssetByPath(const std::string& path) const;
		void Clear();
		size_t Count() const { return m_Assets.size(); }

	private:
		ParticleAssetRegistry() = default;
		std::unordered_map<uint64_t, ParticleAsset> m_Assets;
		std::unordered_map<std::string, uint64_t> m_PathToGuid;
	};
}

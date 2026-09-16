#pragma once

#include "World/Core/Export.h"
#include "World/Core/Core.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace World
{
	// D3:材质混合模式。Opaque = 深度写 + 背面剔除;Transparent = 混合 + 不写深度。
	enum class MaterialBlendMode : uint8_t
	{
		Opaque = 0,
		Transparent = 1,
	};

	// .wmat 的纯数据描述(可序列化、可比较、无 GPU 资源)。
	//
	// 色彩约定(工业级管线,与 plan §3.3 一致):
	//  - BaseColor / Emissive:编辑与存档都是 **sRGB 空间**取值,着色器解到线性后参与光照;
	//  - AlbedoTexture 以 R8G8B8A8_SRGB 创建(硬件解码到线性);
	//  - NormalTexture 是线性数据(UNORM,不做 sRGB 解码)。
	struct WLD_API MaterialDesc
	{
		std::string Name = "Material";
		glm::vec4 BaseColor { 1.0f };
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		glm::vec3 Emissive { 0.0f };
		// 相对 Game/assets 的路径(与场景/网格引用一致);空字符串 = 不使用该贴图。
		std::string AlbedoTexture;
		std::string NormalTexture;
		MaterialBlendMode BlendMode = MaterialBlendMode::Opaque;
		bool DoubleSided = false;

		bool operator==(const MaterialDesc& other) const
		{
			return Name == other.Name
				&& BaseColor == other.BaseColor
				&& Metallic == other.Metallic
				&& Roughness == other.Roughness
				&& Emissive == other.Emissive
				&& AlbedoTexture == other.AlbedoTexture
				&& NormalTexture == other.NormalTexture
				&& BlendMode == other.BlendMode
				&& DoubleSided == other.DoubleSided;
		}
		bool operator!=(const MaterialDesc& other) const { return !(*this == other); }
	};

	struct WLD_API MaterialLoadResult;

	// 运行时材质实例。Desc 是**单一事实源**:编辑器直接改它(即时预览),
	// 渲染侧通过 Revision 感知变化(失效 GPU 描述符/参数缓存)。
	// 实例本身归 MaterialLibrary 所有,外部只持有 Ref;不要在别处 new。
	class WLD_API Material
	{
	public:
		const MaterialDesc& GetDesc() const { return m_Desc; }
		// 非 const 访问器:编辑器就地修改后必须调用 InvalidateDesc() 才会触发渲染侧重建。
		MaterialDesc& GetMutableDesc() { return m_Desc; }
		const std::string& GetPath() const { return m_Path; }
		uint32_t GetRevision() const { return m_Revision; }

		// 参数写入统一走这里:Revision 自增 → 渲染侧缓存失效。
		void SetDesc(const MaterialDesc& desc)
		{
			if (m_Desc == desc)
				return;
			m_Desc = desc;
			BumpRevision();
		}
		void SetBaseColor(const glm::vec4& color) { if (m_Desc.BaseColor != color) { m_Desc.BaseColor = color; BumpRevision(); } }
		void SetMetallic(float value) { if (m_Desc.Metallic != value) { m_Desc.Metallic = value; BumpRevision(); } }
		void SetRoughness(float value) { if (m_Desc.Roughness != value) { m_Desc.Roughness = value; BumpRevision(); } }
		void SetEmissive(const glm::vec3& emissive) { if (m_Desc.Emissive != emissive) { m_Desc.Emissive = emissive; BumpRevision(); } }
		void SetAlbedoTexture(const std::string& path) { if (m_Desc.AlbedoTexture != path) { m_Desc.AlbedoTexture = path; BumpRevision(); } }
		void SetNormalTexture(const std::string& path) { if (m_Desc.NormalTexture != path) { m_Desc.NormalTexture = path; BumpRevision(); } }
		void SetBlendMode(MaterialBlendMode mode) { if (m_Desc.BlendMode != mode) { m_Desc.BlendMode = mode; BumpRevision(); } }
		void SetDoubleSided(bool value) { if (m_Desc.DoubleSided != value) { m_Desc.DoubleSided = value; BumpRevision(); } }

		// 贴图文件在磁盘/VFS 上被替换后调用:同样失效 GPU 侧贴图缓存。
		void InvalidateTextures() { BumpRevision(); }

		// 脏标记(编辑器用):已修改未落盘。保存成功后由库清掉。
		bool IsDirty() const { return m_Dirty; }
		void MarkDirty(bool dirty = true) { m_Dirty = dirty; }

	private:
		friend class MaterialLibrary;
		friend struct MaterialLoadResult;

		explicit Material(MaterialDesc desc, std::string path)
			: m_Desc(std::move(desc)), m_Path(std::move(path)) {}

		void BumpRevision() { ++m_Revision; }
		void SetPath(std::string path) { m_Path = std::move(path); }

		MaterialDesc m_Desc;
		std::string m_Path;          // 规范化(可阅读)路径;内存态可能为空 = 未落盘的新材质
		uint32_t m_Revision = 1;     // 0 保留给"从未上传"
		bool m_Dirty = false;
		// 热重载用的磁盘时间戳(仅 MaterialLibrary 维护)。
		std::filesystem::file_time_type m_FileTime = std::filesystem::file_time_type::min();
	};

	// 解析结果:错误信息给编辑器面板显示,素材照常回退默认值(坏文件不崩)。
	struct WLD_API MaterialLoadResult
	{
		bool Success = false;
		std::string Error;      // 人类可读;空 = 无错误
	};

	// .wmat 读写(纯逻辑,可单测;不依赖 RHI 设备)。
	namespace MaterialIO
	{
		// 支持版本。读到更高版本直接失败(不猜、不降级)。
		constexpr uint32_t kFormatVersion = 1;

		// 解析 .wmat 文本。失败时填充 out 的默认值并把原因写进 error。
		WLD_API MaterialLoadResult Parse(const std::string& text, MaterialDesc& out, std::string* error);
		// 序列化为 .wmat 文本(与 Parse 往返一致)。
		WLD_API std::string Serialize(const MaterialDesc& desc);

		// 从 VFS 优先、磁盘回退读取文件内容;找不到返回 false。
		WLD_API bool ReadFileText(const std::string& path, std::string& out);
		// 写文件(先写临时文件再替换,避免半截文件)。
		WLD_API bool WriteFileText(const std::string& path, const std::string& text, std::string* error);
	}
}

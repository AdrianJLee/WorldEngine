#pragma once

#include "World/Core/Export.h"
#include "World/Core/Core.h"
#include "World/Renderer/MaterialParams.h"

#include <glm/glm.hpp>

#include <cstddef>
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
		// 相对项目内容根(WLD_ASSETPATH)的路径(与场景/网格引用一致);空字符串 = 不使用该贴图。
		std::string AlbedoTexture;
		std::string NormalTexture;
		MaterialBlendMode BlendMode = MaterialBlendMode::Opaque;
		bool DoubleSided = false;
		// M4-S2:材质着色器资产(`.hlsl`,相对内容根;如 "shaders/glass.hlsl")。
		// 空 = 不做表面函数着色(M3 及以前的参数化路径)。
		std::string ShaderPath;

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
				&& DoubleSided == other.DoubleSided
				&& ShaderPath == other.ShaderPath;
		}
		bool operator!=(const MaterialDesc& other) const { return !(*this == other); }
	};

	// ---- M3:材质实例(.wmat = 覆盖字段 + 父级引用)----
	//
	// 格式版本:
	//  - 1 = M3 之前的老写法:逐字段全写;缺字段 = 引擎内置默认(逐字段行为与老代码一致);
	//  - 2 = 材质实例:Parent + 只写覆盖字段(缺字段 = 从父级继承;没有父级 = 引擎内置默认)。
	//        M4-S2 起 v2 还承载 `Shader:`(表面函数引用)与 `Params:`(参数覆盖);
	//        v1 没有这两个键,所以带它们的文件一律按 v2 写出。
	// 读接受 1..2,读到更高版本直接失败(不猜、不降级);写出用哪个版本见
	// MaterialIO::DocumentFormatVersion(全字段 + 无父级仍然写 1,保证老文件逐字节不变)。
	constexpr uint32_t kMaterialFormatVersionLegacy = 1;
	constexpr uint32_t kMaterialFormatVersionMax = 2;

	// 材质的可继承字段。顺序 = .wmat 的书写顺序(与 MaterialDesc 的成员顺序一致)。
	// Name 也是可继承字段:未覆盖时跟随父级(资产身份始终是文件路径,不是 Name)。
	enum class MaterialField : uint8_t
	{
		Name = 0,
		BaseColor,
		Metallic,
		Roughness,
		Emissive,
		AlbedoTexture,
		NormalTexture,
		BlendMode,
		DoubleSided,
		Count,
	};

	constexpr size_t kMaterialFieldCount = static_cast<size_t>(MaterialField::Count);

	constexpr uint16_t MaterialFieldBit(MaterialField field)
	{
		return static_cast<uint16_t>(1u << static_cast<uint8_t>(field));
	}

	// 字段集合(位掩码)。材质实例用它记"本文件显式写出的字段" = 覆盖集。
	struct WLD_API MaterialFieldSet
	{
		uint16_t Bits = 0;

		bool Has(MaterialField field) const { return (Bits & MaterialFieldBit(field)) != 0; }
		void Set(MaterialField field) { Bits |= MaterialFieldBit(field); }
		void Clear(MaterialField field) { Bits &= static_cast<uint16_t>(~MaterialFieldBit(field)); }
		bool Empty() const { return Bits == 0; }
		// 全部字段(老写法的判据之一)。
		bool All() const { return Bits == Everything().Bits; }
		int Count() const;
		static MaterialFieldSet Everything()
		{
			MaterialFieldSet set;
			set.Bits = static_cast<uint16_t>((1u << kMaterialFieldCount) - 1u);
			return set;
		}
		bool operator==(const MaterialFieldSet& other) const { return Bits == other.Bits; }
		bool operator!=(const MaterialFieldSet& other) const { return Bits != other.Bits; }
	};

	// .wmat 文档(材质实例):本文件自己写了什么 + 它指向哪个父级。
	// 解析后与父级合并才得到 MaterialDesc(渲染用的解析结果),见 MaterialIO::MergeDocument。
	//  - Values 只在 Overridden 命中的字段上有效;
	//  - ParentPath 相对内容根(与材质路径同一约定),空 = 没有父级 = 引擎内置默认;
	//  - 老文件(v1、逐字段全写)解析出来的 Overridden = 全部字段,合并结果与 M3 前逐字段一致。
	struct WLD_API MaterialDocument
	{
		uint32_t FormatVersion = kMaterialFormatVersionMax;
		std::string ParentPath;
		MaterialDesc Values;
		MaterialFieldSet Overridden;
		// M4-S2:本文件写了 `Shader:` 行(空字符串 = 显式置空,不继承父级)。
		bool HasShader = false;
		// M4-S2:本文件写的参数覆盖(文件顺序);值文本见 MaterialParams.h。
		std::vector<MaterialParamOverride> Params;
	};

	struct WLD_API MaterialLoadResult;

	// 运行时材质实例。Desc 是**单一事实源**:编辑器直接改它(即时预览),
	// 渲染侧通过 Revision 感知变化(失效 GPU 描述符/参数缓存)。
	// 实例本身归 MaterialLibrary 所有,外部只持有 Ref;不要在别处 new。
	//
	// M3:实例 = 覆盖字段 + 父级(父级也是实例,链尾是引擎内置默认)。
	// GetDesc() 始终是**解析后**的结果(渲染只看它);HasOverride/RevertField 是编辑器的继承视图。
	class WLD_API Material
	{
	public:
		const MaterialDesc& GetDesc() const { return m_Desc; }
		// 非 const 访问器:直接改**不记覆盖位、不前进 Revision**(渲染侧看不到、保存也不会写)。
		// M3 起编辑器请走 Set* 系列或 SetDesc(它们会记覆盖 + 前进 Revision)。
		MaterialDesc& GetMutableDesc() { return m_Desc; }
		const std::string& GetPath() const { return m_Path; }
		uint32_t GetRevision() const { return m_Revision; }

		// 参数写入统一走这里:Revision 自增 → 渲染侧缓存失效。
		// 逐字段比较:变了的值按"用户显式写入"记进覆盖集(M3),保存时才会写进文件。
		void SetDesc(const MaterialDesc& desc);
		void SetBaseColor(const glm::vec4& color) { if (m_Desc.BaseColor != color) { m_Desc.BaseColor = color; m_Overrides.Set(MaterialField::BaseColor); BumpRevision(); } }
		void SetMetallic(float value) { if (m_Desc.Metallic != value) { m_Desc.Metallic = value; m_Overrides.Set(MaterialField::Metallic); BumpRevision(); } }
		void SetRoughness(float value) { if (m_Desc.Roughness != value) { m_Desc.Roughness = value; m_Overrides.Set(MaterialField::Roughness); BumpRevision(); } }
		void SetEmissive(const glm::vec3& emissive) { if (m_Desc.Emissive != emissive) { m_Desc.Emissive = emissive; m_Overrides.Set(MaterialField::Emissive); BumpRevision(); } }
		void SetAlbedoTexture(const std::string& path) { if (m_Desc.AlbedoTexture != path) { m_Desc.AlbedoTexture = path; m_Overrides.Set(MaterialField::AlbedoTexture); BumpRevision(); } }
		void SetNormalTexture(const std::string& path) { if (m_Desc.NormalTexture != path) { m_Desc.NormalTexture = path; m_Overrides.Set(MaterialField::NormalTexture); BumpRevision(); } }
		void SetBlendMode(MaterialBlendMode mode) { if (m_Desc.BlendMode != mode) { m_Desc.BlendMode = mode; m_Overrides.Set(MaterialField::BlendMode); BumpRevision(); } }
		void SetDoubleSided(bool value) { if (m_Desc.DoubleSided != value) { m_Desc.DoubleSided = value; m_Overrides.Set(MaterialField::DoubleSided); BumpRevision(); } }

		// ---- M3:继承 / 覆盖(编辑器用)----

		// 本文件是否显式写了该字段(覆盖)。未覆盖 = 跟随父级(没有父级时 = 引擎内置默认)。
		bool HasOverride(MaterialField field) const { return m_Overrides.Has(field); }
		const MaterialFieldSet& Overrides() const { return m_Overrides; }
		int OverrideCount() const { return m_Overrides.Count(); }

		// 回退到父级(没有父级 = 引擎内置默认)的值:清掉覆盖位、Revision 自增、标记未保存。
		// 本来就是继承态 → no-op(不动 Revision、不动脏标记)。
		void RevertField(MaterialField field);

		// 文件里写的父级逻辑路径(规范化);空 = 没有父级 = 引擎内置默认。
		const std::string& ParentPath() const { return m_ParentPath; }
		// 解析到的父级实例;nullptr = 引擎内置默认,或父级不可用(退化,见 IsParentMissing)。
		const Ref<Material>& ResolvedParent() const { return m_Parent; }
		// 写了父级但读不到/坏/被拒 → 已经退化成引擎默认,子材质仍可用。
		bool IsParentMissing() const { return !m_ParentPath.empty() && !m_Parent; }
		// 父级不可用时的可读原因(空 = 没有退化)。
		const std::string& ParentWarning() const { return m_ParentWarning; }

		// 本实例当前写出会用哪个 .wmat 版本(1 = 老的全字段写法;2 = 覆盖字段 + Parent)。
		uint32_t GetFormatVersion() const;

		// ---- M4-S2:shader 引用 + 注解参数 ----
		//
		// 语义(与 M3 一致的最少惊讶口径):
		//  - Shader 是可继承字段:本文件没写 `Shader:` 时跟随父级(没有父级 = 不做表面函数);
		//  - 参数默认值来自 shader 的 `//! param` 注解;`Params:` 只写覆盖项;
		//    生效值 = 本文件覆盖 > 父级覆盖 > shader 默认;
		//  - shader 里没有声明的参数:文件里保留、运行期忽略,并在 ParamWarnings() 里给可读警告。

		// 生效的 shader 路径(可能继承父级;空 = 不做表面函数)。
		const std::string& ShaderPath() const { return m_Desc.ShaderPath; }
		// ---- M4-S3:表面管线键 ----
		// 键 = 表面管线在 MaterialSurfaceRuntime 里的身份(不透明字符串):
		//  - 默认 = 规范化后的 ShaderPath(),例如 "shaders/glass.hlsl" —— 已保存的材质
		//    在场景与预览里共用同一份已发布管线;
		//  - 编辑器里**未保存**的实时改动把键覆盖成 `<路径>#preview`(SetSurfaceKeyOverride),
		//    只有该面板的预览材质用它 —— 主场景因此永远看不到未保存的编辑(D2 的键分离);
		//  - 空串 = 该材质不参与表面管线(没有 shader 引用)。
		std::string SurfaceKey() const;
		void SetSurfaceKeyOverride(std::string key) { m_SurfaceKeyOverride = std::move(key); }
		const std::string& SurfaceKeyOverride() const { return m_SurfaceKeyOverride; }
		// M4-S3:引用的 `.hlsl` 内容变化(热重载)后调用:Revision 自增 → 渲染侧按 Revision
		// 重建该材质的参数 UBO / 表面描述符集(与 InvalidateTextures 同款语义)。
		void InvalidateShader() { BumpRevision(); }
		// 本文件是否显式写了 `Shader:`。
		bool HasShaderOverride() const { return m_HasShaderOverride; }
		// shader 读不到 / 注解解析失败的可读原因(空 = 没问题)。
		const std::string& ShaderWarning() const { return m_ShaderWarning; }
		// shader 注解声明的参数表(空 = 没有 shader / 读不到 / 注解为空)。
		const std::vector<MaterialParamDecl>& Params() const { return m_ParamDecls; }
		// 本文件写的覆盖(文件顺序)。
		const std::vector<MaterialParamOverride>& ParamOverrides() const { return m_ParamOverrides; }
		// 未声明参数 / 值类型不符等可读警告(加载时算好,编辑器直接显示)。
		const std::vector<std::string>& ParamWarnings() const { return m_ParamWarnings; }

		bool HasParamOverride(const std::string& name) const;
		const std::string* FindParamOverride(const std::string& name) const;
		// 生效值来自哪里:本文件覆盖 / 父级覆盖 / shader 默认(编辑器三态用)。
		MaterialParamSource ParamSource(const std::string& name) const;
		// shader 注解里的默认值(空 = 该参数没被声明)。
		std::string ParamDefaultValue(const std::string& name) const;
		// 生效值:本文件覆盖 > 父级覆盖 > shader 默认(都没有 = 空串)。
		std::string ResolvedParamValue(const std::string& name) const;
		// 本文件覆盖的值是否等于 shader 默认值(编辑器"与默认相同"态;没有覆盖 = false)。
		bool ParamMatchesDefault(const std::string& name) const;

		// 写 shader 覆盖(记覆盖 + Revision + 脏标记);参数表由 MaterialLibrary 立刻刷新。
		void SetShaderPath(const std::string& path);
		// 清掉 shader 覆盖(回退父级;没有父级 = 不做表面函数)。
		void RevertShader();
		// 写参数覆盖(值文本;NormalizeParamValue 的方言)。参数名不在 shader 声明里也照写
		// (文件里保留 + 警告),这样"编辑器里改错了还能改回来"。
		void SetParamOverride(const std::string& name, const std::string& value);
		// 清掉本文件的参数覆盖(生效值回到父级 / shader 默认)。本来就没有 → no-op。
		void RevertParam(const std::string& name);

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
		std::string m_ParentPath;    // M3:声明的父级(规范化);空 = 引擎内置默认
		MaterialFieldSet m_Overrides; // M3:本文件显式写出的字段(覆盖集)
		bool m_HasShaderOverride = false;              // M4-S2:本文件写了 Shader:
		std::vector<MaterialParamOverride> m_ParamOverrides;  // M4-S2:本文件的参数覆盖
		std::vector<MaterialParamDecl> m_ParamDecls;   // M4-S2:shader 注解参数表(可能继承父级)
		std::vector<std::string> m_ParamWarnings;      // M4-S2:未声明参数 / 值类型不符
		std::string m_ShaderWarning;                   // M4-S2:shader 读不到 / 注解解析失败
		Ref<Material> m_Parent;      // M3:解析到的父级实例(共享所有权;nullptr = 引擎默认/退化)
		std::string m_ParentWarning; // M3:父级不可用的可读原因
		std::string m_SurfaceKeyOverride;  // M4-S3:表面管线键覆盖(不序列化;预览材质用)
		uint32_t m_Revision = 1;     // 0 保留给"从未上传"
		bool m_Dirty = false;
		void RecomputeParamWarnings();  // 覆盖集 → ParamWarnings()
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
		// 支持的最高版本(读到更高版本直接失败,不猜、不降级)。老名字保留给既有调用点。
		constexpr uint32_t kFormatVersion = kMaterialFormatVersionMax;

		// 引擎内置默认材质(没有父级时的合并基底;= MaterialDesc 的默认构造值)。
		WLD_API const MaterialDesc& DefaultMaterialDesc();

		// 逻辑路径规范化:统一分隔符为 '/'、去掉前导 "./"、合并重复斜杠。
		// (MaterialLibrary::NormalizePath 是它的同义入口,老调用点不变。)
		WLD_API std::string NormalizePath(const std::string& path);

		// 字段名(与 .wmat 的键同名)与人类可读值(编辑器"继承自 <父>: <值>"、诊断输出用)。
		WLD_API const char* FieldKey(MaterialField field);
		WLD_API std::string FormatFieldValue(const MaterialDesc& desc, MaterialField field);

		// 解析 .wmat 文本为**文档**(只含本文件写出的字段 + 父级引用;不读盘、不解析父级链)。
		// 失败时 out 复位并把原因写进 error。
		WLD_API MaterialLoadResult ParseDocument(const std::string& text, MaterialDocument& out, std::string* error);

		// 合并:覆盖字段 → 父级解析结果(parentDesc == nullptr 时用引擎内置默认)。
		WLD_API MaterialDesc MergeDocument(const MaterialDocument& document, const MaterialDesc* parentDesc);

		// 文档写出的版本:没有父级 + 全部字段都有覆盖 = 1(与 M3 前的文件逐字节一致);否则 2。
		WLD_API uint32_t DocumentFormatVersion(const MaterialDocument& document);

		// 解析 .wmat 文本为**单文件视角**的 MaterialDesc(覆盖字段叠在引擎默认上)。
		// 注意:这里不读盘、不解析父级链 —— 带 Parent 的文件要走 MaterialLibrary::Load /
		// CreateInstance 才能得到继承语义;此处的 error 会带一条说明。
		WLD_API MaterialLoadResult Parse(const std::string& text, MaterialDesc& out, std::string* error);

		// 序列化为 .wmat 文本:
		//  - SerializeDocument:只写覆盖字段 + Parent + Shader + Params(逐字节确定;
		//    paramDecls 给参数值的 YAML 形态:已知类型 → 数字/序列/字符串,
		//    未知类型(shader 读不到)→ 按标量文本写,值文本原样保留);
		//  - Serialize(desc):老写法的全字段文本,与 M3 前逐字节一致(导入器/新建模板继续用它)。
		WLD_API std::string SerializeDocument(const MaterialDocument& document,
			const std::vector<MaterialParamDecl>* paramDecls = nullptr);
		WLD_API std::string Serialize(const MaterialDesc& desc);
		// U23:保存回读校验的比较口径(旧行为是 `verify != desc` 逐位相等)。
		//  - 浮点字段(BaseColor / Metallic / Roughness / Emissive)按 |a-b| <= tolerance 比较:
		//    写出文本的十进制表示不再把"拖一下滑杆"的正常值判成写入校验失败;
		//  - 非浮点字段(Name / 两个贴图路径 / BlendMode / DoubleSided)仍然**严格相等**,
		//    类型/字符串/枚举写坏必须被拒;
		//  - NaN 不参与"近似相等"(返回 false),与逐位比较的语义一致。
		WLD_API bool EquivalentForSave(const MaterialDesc& actual, const MaterialDesc& expected,
			float tolerance = 1e-6f);

		// 从 VFS 优先、磁盘回退读取文件内容;找不到返回 false。
		WLD_API bool ReadFileText(const std::string& path, std::string& out);
		// 写文件(先写临时文件再替换,避免半截文件)。
		WLD_API bool WriteFileText(const std::string& path, const std::string& text, std::string* error);
	}
}

#pragma once

#include "World/Core/Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace World
{
	// M4-S2:材质着色器(Slang 源 `.slang`;legacy `.hlsl` 同列)里的**注解参数表**。
	//
	// 注解是参数的事实源(编辑器据此生成 DragBar / 取色器 / 资产下拉 / 复选框 / 步进,
	// 运行时据此生成 cbuffer 布局):
	//
	//   //! param <type> <name> = <default> [min,max] unit("") group("") label("")
	//
	// 语法细则(逐行,`//!` 之后第一条指令必须是 param):
	//   - <type>    :Float / Vec2 / Vec3 / Vec4 / Color / Int / Bool / Texture2D(大小写敏感);
	//   - <name>    :HLSL 标识符;以 `u_` 开头或与引擎模板标识符冲突 → 报错(见 .cpp 的保留名表);
	//   - <default> :**必填**。数值型是逗号分隔的字面量(`0.25` / `1, 1, 1`);Bool 是
	//                 `true`/`false`;Texture2D 是相对内容根的路径(`""` = 无贴图,可带引号);
	//   - [min,max] :只对 Float / Int 有效(其余类型报错),min <= max,默认值必须落在区间内;
	//   - unit/group/label:双引号字符串,可空;每条注解里每个字段最多出现一次。
	// 失败时 error 的格式固定为 `<行>:<列>: <原因>`(1 基,列号指向出错的 token),
	// 供编辑器直接显示 / 定位。
	//
	// 值文本(Default / .wmat Params / MaterialParamOverride::Value)是**同一套方言**:
	//   - Float/Int : `0.25` / `3`
	//   - Vec2..4   : `1, 0.5`(逗号分隔,规范化时写成 `1, 0.5`)
	//   - Color     :3 或 4 个分量(写 3 个时 alpha = 1),规范化后固定 4 个分量
	//   - Bool      :`true` / `false`
	//   - Texture2D :路径文本(`textures/icon.png`;空串 = 无贴图),不带引号
	// 归一化入口 = NormalizeParamValue(校验 + 规范化,失败给可读原因)。
	enum class ParamType : uint8_t
	{
		Float = 0,
		Vec2 = 1,
		Vec3 = 2,
		Vec4 = 3,
		Color = 4,
		Int = 5,
		Bool = 6,
		Texture2D = 7,
	};

	// 注解里写的类型名(与 ParamTypeName 同一份文案)。
	WLD_API const char* ParamTypeName(ParamType type);
	WLD_API bool ParseParamTypeName(const std::string& text, ParamType* out);
	// 该类型是不是贴图(贴图是描述符绑定,不进 cbuffer)。
	WLD_API bool IsTextureParamType(ParamType type);
	// 参数名是否可用:合法 HLSL 标识符,且不与引擎模板/uniform 保留名冲突
	// (以 `u_` 开头、或与 MaterialParams / PSMain / input / surface 这类模板标识符同名)。
	WLD_API bool IsUsableParamName(const std::string& name);

	struct WLD_API MaterialParamDecl
	{
		std::string Name;
		ParamType Type = ParamType::Float;   // Float/Vec2/Vec3/Vec4/Color/Int/Bool/Texture2D
		std::string Default;                 // 值文本(见头注释)
		float Min = 0.0f;                    // 未写 [min,max] 时 = 0/1(与 Surface 标量语义范围一致)
		float Max = 1.0f;
		std::string Unit;
		std::string Group;
		std::string Label;
	};

	// 冻结 API(M4-S2 派工):注解解析。
	//  - 成功 = out 按文件出现顺序填好,error 清空;
	//  - 失败 = out 清空,error = `<行>:<列>: <原因>`;未知类型 / 未知字段 / 缺默认值 /
	//    重复参数名 / 语法错都给行列号。
	WLD_API bool ParseMaterialParams(const std::string& hlslSource,
		std::vector<MaterialParamDecl>* out, std::string* error);

	// 冻结 API(M4-S2 派工;Slang-T3 起编译/反射走 slangc + `-reflection-json`)。
	//  1) 按 table 生成参数块(注解是事实源)编译包装源码 → 拿 SPIR-V + 反射 JSON;
	//  2) 从反射 JSON 读参数块成员(名称/类型/偏移)与贴图槽(set/binding);
	//  3) 三态判定:
	//     - 合规:声明、类型、绑定一致,且成员真的被读 → true,无警告;
	//     - 声明未用 → true + warnings("参数 'X' 声明了但着色器没读它");
	//     - 用了未声明(参数块里有注解没有的成员)/ 类型不符 / 贴图绑定不符 → false + error;
	//  编译失败(用户源码错误)时 false + error(带 Slang 原始诊断)。
	// 反射数据来自 Slang 自己的 `-reflection-json`,不依赖编译器头文件/链接 —— 详见 .cpp。
	WLD_API bool ValidateParamsWithReflection(const std::string& hlslSource,
		const std::vector<MaterialParamDecl>& table,
		std::vector<std::string>* warnings, std::string* error);

	// ---- 值文本(Default / .wmat Params 共用) ----

	// 最短往返的浮点文本(与 Material.cpp 的 FormatFloat 同一口径,保证"覆盖 == 默认"比较稳定)。
	WLD_API std::string FormatParamFloatText(float value);
	WLD_API bool ParseParamFloat(const std::string& text, float* out);
	WLD_API bool ParseParamInt(const std::string& text, int* out);
	WLD_API bool ParseParamBool(const std::string& text, bool* out);
	// count = 2/3/4;Color 允许写 3 个分量(第 4 个补 1)。
	WLD_API bool ParseParamFloatComponents(const std::string& text, ParamType type,
		float* out, int count);
	// 校验 + 规范化值文本;失败时 error 给可读原因(normalized 不变)。
	WLD_API bool NormalizeParamValue(ParamType type, const std::string& text,
		std::string* normalized, std::string* error);
	// 类型与值文本是否相容(编辑器写覆盖前的轻量校验,不产生规范化结果)。
	WLD_API bool IsParamValueCompatible(ParamType type, const std::string& text);
	// 反射类型能否满足注解类型(Float←float、Vec2←v2float、Bool←uint(HLSL bool 在 SPIR-V 里
	// 是 uint32)、Color/Vec4←v4float …)。ValidateParamsWithReflection 的"类型不符"判据。
	WLD_API bool IsReflectedTypeCompatible(ParamType type, const std::string& reflectedType);
	// 注解 → 文本(不带 `//! ` 前缀)。编辑器改写材质着色器里的注解行时用它,保证
	// "写出的行"能被 ParseMaterialParams 原样读回来(同一套值文本 / 引号 / 范围口径)。
	// 范围只在 (Min,Max) != (0,1) 时写出 —— 保持"没写范围"与"[0,1]"在解析结果上等价。
	WLD_API std::string FormatMaterialParamAnnotation(const MaterialParamDecl& decl);

	// ---- 反射布局(由 Slang 的反射 JSON 读出,不手写结构体) ----

	struct WLD_API MaterialParamLayoutField
	{
		std::string Name;           // 参数名(= 参数块成员名)
		ParamType Type = ParamType::Float;
		std::string ReflectedType;  // SPIR-V 侧的标量/向量类型(如 float / v3float / uint)
		uint32_t Offset = 0;        // 反射偏移(字节)
		uint32_t Size = 0;          // 反射大小(字节)
	};

	struct WLD_API MaterialParamTextureSlot
	{
		std::string Name;
		std::string ReflectedType;  // 如 type.2d.image
		uint32_t Set = 0;
		uint32_t Binding = 0;
	};

	struct WLD_API MaterialParamLayout
	{
		uint32_t CbufferSize = 0;      // 16 字节对齐(>= 反射到的成员末端)
		// M4-S3(D1):参数块固定 set 1 / binding 4(与引擎 ObjectUniforms 同 set)。
		// 必须是 4 而不是 2:OpenGL 后端的 UBO 绑定单元 = binding(忽略 set),
		// set0 的灯光 UBO 已经占了单元 2(与 2026-09-19 骨骼单元 2→3 同类坑)。
		// 占用表:0=相机、1=物体、2=灯光、3=骨骼、4=材质参数。
		uint32_t CbufferSet = 1;
		uint32_t CbufferBinding = 4;
		std::vector<MaterialParamLayoutField> Fields;
		std::vector<MaterialParamTextureSlot> Textures;
		// 反射到的"真的被读"的成员名(升序)。声明了但不在这里面 = 声明未用 → 警告。
		std::vector<std::string> UsedMembers;
	};

	// 参数块 / 贴图槽的寄存器约定(生成器、反射、运行时上传共用一份)。
	WLD_API const char* ParamCbufferName();
	WLD_API uint32_t ParamCbufferSet();
	WLD_API uint32_t ParamCbufferBinding();
	WLD_API uint32_t ParamTextureBaseBinding();

	// M4-S3:参数块里贴图参数的数量上限。贴图槽 = ParamTextureBaseBinding() 起**连续**占用
	// (t4..t11),与运行时/表面材质的固定描述符槽位一一对应;超过上限 = 结构化错误
	// (编译与反射校验都不允许静默丢参数)。
	inline constexpr uint32_t kMaxMaterialTextureSlots = 8;

	// Slang-T3:从 Slang 的 `-reflection-json` + 同一次编译的 SPIR-V 二进制反射参数布局。
	//  - JSON 是布局的事实源:参数块 (set,binding)、成员名/类型/偏移/大小、块大小、贴图槽 set/binding;
	//  - SPIR-V 只用来回答"成员真的被读"(OpAccessChain 的首下标)—— 输入是二进制模块,
	//    不依赖任何外部反汇编工具。
	// 纯函数:不调用编译器,可无工具单测。
	WLD_API bool ReflectParamLayoutFromReflectionJson(const std::string& reflectionJson,
		const std::vector<uint8_t>& spirv, MaterialParamLayout* out, std::string* error);

	// 端到端:按 table 编译包装源码(slangc)并反射出布局。工具缺失/源码错误 → false + error。
	WLD_API bool BuildParamLayout(const std::string& hlslSource,
		const std::vector<MaterialParamDecl>& table,
		MaterialParamLayout* out, std::string* error);

	// 字段偏移表的可读文本(报告 / 日志 / 编辑器诊断用)。
	WLD_API std::string FormatParamLayout(const MaterialParamLayout& layout);

	// .wmat 里写的参数覆盖(值文本;文件顺序 = vector 顺序)。
	struct WLD_API MaterialParamOverride
	{
		std::string Name;
		std::string Value;

		bool operator==(const MaterialParamOverride& other) const
		{
			return Name == other.Name && Value == other.Value;
		}
		bool operator!=(const MaterialParamOverride& other) const { return !(*this == other); }
	};

	// 参数值 → 参数块字节(CPU 侧打包,布局来自反射)。Texture2D 是绑定,不写进字节;
	// out 大小 = layout.CbufferSize。值文本按声明类型解析,失败给可读原因。
	WLD_API bool PackParamValues(const MaterialParamLayout& layout,
		const std::vector<MaterialParamDecl>& table,
		const std::vector<MaterialParamOverride>& overrides,
		std::vector<uint8_t>* out, std::string* error);

	// 参数生效值 / 来源判定 + 警告(材质加载后由 Material 持有;shaderPath 只用于文案)。
	enum class MaterialParamSource : uint8_t
	{
		ShaderDefault = 0,   // 注解默认值
		Parent = 1,          // 父级材质覆盖
		Local = 2,           // 本文件覆盖
	};

	// 覆盖集对注解表的检查:未声明 / 值类型不符 → 可读警告(顺序 = overrides 顺序)。
	WLD_API std::vector<std::string> BuildParamWarnings(
		const std::vector<MaterialParamDecl>& table,
		const std::vector<MaterialParamOverride>& overrides,
		const std::string& shaderPath);

	// 保存回读校验用的参数比较:名字/顺序严格一致,值按声明类型**归一后**比较
	// (序列空白、Color 写 3 个分量补 alpha 这类差异不算写坏);未声明参数按原文本比较。
	// 类型不符的值也按原文本比较(不作静默改写)。
	WLD_API bool ParamOverridesEquivalent(const std::vector<MaterialParamOverride>& actual,
		const std::vector<MaterialParamOverride>& expected,
		const std::vector<MaterialParamDecl>& table);

	// 注解表里找参数(找不到返回 nullptr)。
	WLD_API const MaterialParamDecl* FindParamDecl(const std::vector<MaterialParamDecl>& table,
		const std::string& name);
}

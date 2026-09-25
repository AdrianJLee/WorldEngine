#pragma once

// MAT-INTEL2(用户 2026-09-24:「interface 这种关键字没有提示」「代码部分好多还是白色的」):
// Slang 关键字 / 内建 / 引擎契约符号的**唯一事实源** —— 高亮(`SlangHighlight.h`)与补全 + 悬停
// (`SlangCompletion.h`)都从这一张表取名字,不再各写一份。旧版正是两张表漂移的例子:高亮认得
// `interface`/`enum`/`where`…,补全的关键字表却只有 11 条,于是输入 `int` 弹不出 `interface`。
//
// 表内容(合并,不新增第二份):
//   - `SlangHighlight.h` 旧 `IsKeyword` / `IsIntrinsic` 的全部词(Slang 语言层 + HLSL 子集 + 内建函数);
//   - `SlangCompletion.h` 旧 `TypeTable` / `BuiltinTable` / `EngineHelperTable` / `KeywordTable` /
//     `AnnotationTable`(文档一并搬过来,口径不变);
//   - 引擎契约类型 `Surface` / `MaterialInputs` 与包装函数(`MakeDefaultSurface` / `WeLinearizeColor` /
//     `WeDefaultNormal` / `WeIdentityMatrix` / `Evaluate`)。
//   **字段名不在这张表里** —— 它们的事实源是 `World/Renderer/MaterialSurfaceContract.hlsli` 的
//   X-macro(补全的 `input.`/`surface.` 成员表与高亮的 `IsContractField` 读同一份)。
//
// 两个维度分开(每条都显式写出,不靠推导):
//   - **Class**(补全语义 / 候选池):决定补全 Kind、排序档位与插入形态;
//       Keyword / Type / EngineType / AnnotationType  → 补全 Kind=Keyword
//       AnnotationKey / AnnotationSyntax              → 注解行候选 Kind=Field
//       AnnotationAttr                                → 注解行候选 Kind=Method(插入 `group("")` 形态)
//       Intrinsic / EngineFunction                    → 补全 Kind=Method(插入补 `()`)
//   - **TokenKind Highlight**(高亮类别,MAT-INTEL4;用户 2026-09-24「类型标识颜色应该和名字分开」):
//       Keyword    控制流 / 声明关键字
//       Type       Slang 标量·向量·矩阵 + 资源·采样器类型 + 引擎契约结构(含 `void`)
//       Function   Slang 内建 + 引擎包装函数
//       Field      字段 / 点号成员(表里暂无条目 —— 契约字段与 `x.y` 成员由高亮器按规则给)
//       Annotation `//!` 注解关键字(param / 类型 / group·label·unit / [min,max])
//       Default    **名字条目**(`input` = Evaluate 的契约形参名):不着色,补全仍有条目
// 上下文(Context)决定词进哪个候选池;高亮**不看**上下文(一个词出现在代码里就按 Highlight 着色,
// 注解词出现在代码里也无害 —— 少一层状态,注解行与代码行共用同一条 token 化路径)。

#include "World/Renderer/MaterialSurfaceContract.hlsli"
#include "World/WUI/WuiCodeEditor.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace World
{
	// 大小写不敏感的比较工具(补全的过滤/排序与注解解析共用一份实现)。
	namespace SlangText
	{
		inline char LowerAscii(char c)
		{
			return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}

		inline bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix)
		{
			if (prefix.size() > text.size())
				return false;
			for (std::size_t i = 0; i < prefix.size(); ++i)
				if (LowerAscii(text[i]) != LowerAscii(prefix[i]))
					return false;
			return true;
		}

		inline bool ContainsIgnoreCase(std::string_view text, std::string_view needle)
		{
			if (needle.empty())
				return true;
			if (needle.size() > text.size())
				return false;
			for (std::size_t start = 0; start + needle.size() <= text.size(); ++start)
				if (StartsWithIgnoreCase(text.substr(start), needle))
					return true;
			return false;
		}

		// 大小写不敏感的名字比较(同档内的排序口径;严格弱序)。
		inline bool NameLess(std::string_view a, std::string_view b)
		{
			const std::size_t shared = std::min(a.size(), b.size());
			for (std::size_t i = 0; i < shared; ++i)
			{
				const char left = LowerAscii(a[i]);
				const char right = LowerAscii(b[i]);
				if (left != right)
					return left < right;
			}
			return a.size() < b.size();
		}
	}

	namespace SlangSymbols
	{
		enum class Class : unsigned char
		{
			Keyword,          // 语言关键字 / 字面量 / 资源类型名
			Type,             // Slang 标量 / 向量 / 矩阵类型(补全里有更详细的文档)
			EngineType,       // 引擎契约结构(Surface / MaterialInputs)
			Intrinsic,        // Slang 内建函数
			EngineFunction,   // 引擎包装层函数(评估入口 + 辅助)
			AnnotationKey,    // `//! param`
			AnnotationType,   // `//!` 里的参数类型(Float / Int / Color / Vec3 / Texture2D …)
			AnnotationAttr,   // `//!` 里的属性(group() / label() / unit())
			AnnotationSyntax, // `//!` 里的语法片段(如 [min,max])
		};

		enum class Context : unsigned char
		{
			Code,       // 代码上下文候选(顶层标识符)
			Annotation, // `//!` 注解行候选
			Both,       // 两边都进(如 Texture2D)
		};

		// 高亮类别 = 引擎 token kind 本身(MAT-INTEL4):表里**每条**显式写自己的类别,
		// 高亮器只做"取类别 → 上色",不再从 Class 二次推导(避免两套口径漂移)。
		// 新增词时**必须**写出 TokenKind::… —— 漏写会退化成 Default(不着色),
		// 探针 mat-intel4-color-probe.py 有一条静态断言守着这件事。
		using TokenKind = Wui::WuiCodeTokenKind;

		struct Symbol
		{
			std::string_view Name;
			std::string_view Category; // 补全的 "类型" 列
			std::string_view Doc;
			Class Kind = Class::Keyword;            // 补全语义(候选池 / Kind / 插入形态)
			TokenKind Highlight = TokenKind::Default; // MAT-INTEL4:高亮类别(每条显式写)
			Context Where = Context::Code;
			std::string_view Insert;                // 补全插入文本(空 = 用 Name;`group` → `group("")`)
		};

		// 唯一表。加词只改这里:高亮与补全同时生效(名字必须唯一 —— 见下方 Index() 的说明)。
		inline constexpr Symbol kSymbols[] = {
			// ---- Slang / HLSL 标量与向量类型 ----
			{ "bool", "scalar", "Boolean scalar (true / false).", Class::Type, TokenKind::Type },
			{ "int", "scalar", "32-bit signed integer scalar.", Class::Type, TokenKind::Type },
			{ "uint", "scalar", "32-bit unsigned integer scalar.", Class::Type, TokenKind::Type },
			{ "dword", "scalar", "32-bit unsigned integer alias (HLSL).", Class::Type, TokenKind::Type },
			{ "half", "scalar", "16-bit float scalar (no implicit conversions).", Class::Type, TokenKind::Type },
			{ "min16float", "scalar", "16-bit minimum-precision float (HLSL).", Class::Type, TokenKind::Type },
			{ "float", "scalar", "32-bit float scalar. Slang does not convert widths implicitly.", Class::Type, TokenKind::Type },
			{ "double", "scalar", "64-bit float scalar (no implicit narrowing to float).", Class::Type, TokenKind::Type },
			{ "float2", "vector", "2-component float vector. Construct explicitly: float2(a, b).", Class::Type, TokenKind::Type },
			{ "float3", "vector", "3-component float vector. Truncate with .xyz/.rgb — implicit width conversion is an error.", Class::Type, TokenKind::Type },
			{ "float4", "vector", "4-component float vector (colour literals are usually float4: r, g, b, a).", Class::Type, TokenKind::Type },
			{ "int2", "vector", "2-component signed integer vector.", Class::Type, TokenKind::Type },
			{ "int3", "vector", "3-component signed integer vector.", Class::Type, TokenKind::Type },
			{ "int4", "vector", "4-component signed integer vector.", Class::Type, TokenKind::Type },
			{ "uint2", "vector", "2-component unsigned integer vector.", Class::Type, TokenKind::Type },
			{ "uint3", "vector", "3-component unsigned integer vector.", Class::Type, TokenKind::Type },
			{ "uint4", "vector", "4-component unsigned integer vector.", Class::Type, TokenKind::Type },
			{ "matrix", "matrix", "Generic matrix type.", Class::Type, TokenKind::Type },
			{ "float2x2", "matrix", "2x2 float matrix.", Class::Type, TokenKind::Type },
			{ "float3x3", "matrix", "3x3 float matrix.", Class::Type, TokenKind::Type },
			{ "float4x4", "matrix", "4x4 float matrix (skin/bone transforms).", Class::Type, TokenKind::Type },
			{ "void", "keyword", "No-return type (helpers that only write their out parameters).", Class::Keyword, TokenKind::Type },

			// ---- 资源 / 采样器类型(严格子集里贴图参数生成 Sampler2D)----
			{ "Texture1D", "resource", "1D texture resource type.", Class::Keyword, TokenKind::Type },
			{ "Texture2D", "resource",
				"Texture resource. As a `//! param` type it means a content-root relative path (\"\" = white 1x1 "
				"fallback); slots t4..t11 by annotation order, max 8, sampled as `name.Sample(uv)`.", Class::Keyword, TokenKind::Type,
				Context::Both },
			{ "Texture3D", "resource", "3D texture resource type.", Class::Keyword, TokenKind::Type },
			{ "TextureCube", "resource", "Cube texture resource type.", Class::Keyword, TokenKind::Type },
			{ "Texture2DArray", "resource", "Arrayed 2D texture resource type.", Class::Keyword, TokenKind::Type },
			{ "TextureCubeArray", "resource", "Arrayed cube texture resource type.", Class::Keyword, TokenKind::Type },
			{ "RWTexture2D", "resource", "Read-write 2D texture resource type (compute).", Class::Keyword, TokenKind::Type },
			{ "ByteAddressBuffer", "resource", "Raw byte-address buffer resource type.", Class::Keyword, TokenKind::Type },
			{ "SamplerState", "resource", "Sampler state resource type.", Class::Keyword, TokenKind::Type },
			{ "SamplerComparisonState", "resource", "Comparison sampler state resource type.", Class::Keyword, TokenKind::Type },
			{ "Sampler1D", "resource", "Combined 1D sampler (engine binds this for Texture1D parameters).", Class::Keyword, TokenKind::Type },
			{ "Sampler2D", "resource", "Combined 2D sampler (engine binds this for Texture2D parameters).", Class::Keyword, TokenKind::Type },
			{ "Sampler3D", "resource", "Combined 3D sampler.", Class::Keyword, TokenKind::Type },
			{ "SamplerCube", "resource", "Combined cube sampler.", Class::Keyword, TokenKind::Type },
			{ "Sampler2DArray", "resource", "Combined arrayed 2D sampler.", Class::Keyword, TokenKind::Type },
			{ "SamplerCubeArray", "resource", "Combined arrayed cube sampler.", Class::Keyword, TokenKind::Type },

			// ---- 声明 / 存储 / 绑定修饰 ----
			{ "static", "keyword", "File-scope static declaration.", Class::Keyword, TokenKind::Keyword },
			{ "const", "keyword", "Read-only value (usual for helper constants).", Class::Keyword, TokenKind::Keyword },
			{ "inline", "keyword", "Inline function (helpers are inlined by the compiler anyway).", Class::Keyword, TokenKind::Keyword },
			{ "uniform", "keyword", "Uniform value (read-only per draw).", Class::Keyword, TokenKind::Keyword },
			{ "in", "keyword", "Input parameter / input-stage qualifier.", Class::Keyword, TokenKind::Keyword },
			{ "out", "keyword", "Output parameter / output-stage qualifier.", Class::Keyword, TokenKind::Keyword },
			{ "inout", "keyword", "Read-write parameter.", Class::Keyword, TokenKind::Keyword },
			{ "volatile", "keyword", "Volatile value (no optimisation across accesses).", Class::Keyword, TokenKind::Keyword },
			{ "cbuffer", "keyword", "Constant buffer block (engine generates the real ones from annotations).", Class::Keyword, TokenKind::Keyword },
			{ "tbuffer", "keyword", "Texture/sampler buffer block.", Class::Keyword, TokenKind::Keyword },
			{ "register", "keyword", "Explicit register binding — engine binds resources from annotations, so user sources do not write it.", Class::Keyword, TokenKind::Keyword },
			{ "packoffset", "keyword", "Packing offset inside a constant buffer.", Class::Keyword, TokenKind::Keyword },
			{ "row_major", "keyword", "Row-major matrix layout.", Class::Keyword, TokenKind::Keyword },
			{ "column_major", "keyword", "Column-major matrix layout (Slang default).", Class::Keyword, TokenKind::Keyword },
			{ "groupshared", "keyword", "Compute group-shared storage.", Class::Keyword, TokenKind::Keyword },
			{ "nointerpolation", "keyword", "Do not interpolate this varying.", Class::Keyword, TokenKind::Keyword },
			{ "noperspective", "keyword", "Interpolate without perspective correction.", Class::Keyword, TokenKind::Keyword },
			{ "linear", "keyword", "Linear interpolation modifier.", Class::Keyword, TokenKind::Keyword },
			{ "centroid", "keyword", "Centroid sampling modifier.", Class::Keyword, TokenKind::Keyword },
			{ "sample", "keyword", "Sample-frequency interpolation modifier.", Class::Keyword, TokenKind::Keyword },
			{ "snorm", "keyword", "Signed normalised value qualifier.", Class::Keyword, TokenKind::Keyword },
			{ "unorm", "keyword", "Unsigned normalised value qualifier.", Class::Keyword, TokenKind::Keyword },

			// ---- 语句 / 表达式关键字与字面量 ----
			{ "if", "keyword", "Conditional branch — parameter-driven shading lives here.", Class::Keyword, TokenKind::Keyword },
			{ "else", "keyword", "Alternative branch.", Class::Keyword, TokenKind::Keyword },
			{ "for", "keyword", "Loop (keep it bounded: this runs per pixel).", Class::Keyword, TokenKind::Keyword },
			{ "while", "keyword", "Loop with a condition.", Class::Keyword, TokenKind::Keyword },
			{ "do", "keyword", "Do/while loop body.", Class::Keyword, TokenKind::Keyword },
			{ "switch", "keyword", "Switch statement (must be exhaustive for scalars).", Class::Keyword, TokenKind::Keyword },
			{ "case", "keyword", "Switch case label.", Class::Keyword, TokenKind::Keyword },
			{ "default", "keyword", "Default branch / default switch label.", Class::Keyword, TokenKind::Keyword },
			{ "break", "keyword", "Break out of the innermost loop or switch.", Class::Keyword, TokenKind::Keyword },
			{ "continue", "keyword", "Continue the innermost loop.", Class::Keyword, TokenKind::Keyword },
			{ "return", "keyword", "Return the Surface from Evaluate.", Class::Keyword, TokenKind::Keyword },
			{ "discard", "keyword", "Discard the fragment (alpha-cutout style effects).", Class::Keyword, TokenKind::Keyword },
			{ "true", "literal", "Boolean true.", Class::Keyword, TokenKind::Keyword },
			{ "false", "literal", "Boolean false.", Class::Keyword, TokenKind::Keyword },
			{ "this", "keyword", "Current instance inside a method.", Class::Keyword, TokenKind::Keyword },
			{ "new", "keyword", "Allocate a reference-type instance.", Class::Keyword, TokenKind::Keyword },

			// ---- Slang 语言层(模块 / 泛型 / 接口)—— 高亮它们让"这是 Slang 源"读得出来 ----
			{ "struct", "keyword", "Struct definition (helper data for Evaluate).", Class::Keyword, TokenKind::Keyword },
			{ "class", "keyword", "Reference type with methods (Slang).", Class::Keyword, TokenKind::Keyword },
			{ "enum", "keyword", "Enumeration definition (Slang).", Class::Keyword, TokenKind::Keyword },
			{ "namespace", "keyword", "Namespace declaration (Slang module scoping).", Class::Keyword, TokenKind::Keyword },
			{ "module", "keyword", "Slang module (compiled separately and imported).", Class::Keyword, TokenKind::Keyword },
			{ "import", "keyword", "Import a Slang module into this file.", Class::Keyword, TokenKind::Keyword },
			{ "using", "keyword", "Type alias / using declaration.", Class::Keyword, TokenKind::Keyword },
			{ "typedef", "keyword", "C-style type alias.", Class::Keyword, TokenKind::Keyword },
			{ "interface", "keyword",
				"Slang interface: a set of required members that a concrete type implements (generic constraint `T : I`).",
				Class::Keyword, TokenKind::Keyword },
			{ "associatedtype", "keyword", "Interface member type that implementers bind.", Class::Keyword, TokenKind::Keyword },
			{ "extension", "keyword", "Extend an existing type with new members (Slang).", Class::Keyword, TokenKind::Keyword },
			{ "property", "keyword", "Property accessor block (Slang).", Class::Keyword, TokenKind::Keyword },
			{ "get", "keyword", "Property getter.", Class::Keyword, TokenKind::Keyword },
			{ "set", "keyword", "Property setter.", Class::Keyword, TokenKind::Keyword },
			{ "public", "keyword", "Public visibility.", Class::Keyword, TokenKind::Keyword },
			{ "internal", "keyword", "Module-internal visibility.", Class::Keyword, TokenKind::Keyword },
			{ "private", "keyword", "Private visibility.", Class::Keyword, TokenKind::Keyword },
			{ "extern", "keyword", "Externally defined declaration.", Class::Keyword, TokenKind::Keyword },
			{ "where", "keyword", "Generic constraint clause (`<T> where T : IUVTransform`).", Class::Keyword, TokenKind::Keyword },
			{ "each", "keyword", "Slang `each` generic expansion.", Class::Keyword, TokenKind::Keyword },
			{ "expand", "keyword", "Slang `expand` generic expansion.", Class::Keyword, TokenKind::Keyword },
			{ "func", "keyword", "Function type declarator (Slang).", Class::Keyword, TokenKind::Keyword },
			{ "let", "keyword", "Immutable local binding.", Class::Keyword, TokenKind::Keyword },
			{ "var", "keyword", "Mutable local binding.", Class::Keyword, TokenKind::Keyword },
			{ "__subscript", "keyword", "Slang subscript operator member.", Class::Keyword, TokenKind::Keyword },
			{ "__init", "keyword", "Slang initialiser member.", Class::Keyword, TokenKind::Keyword },
			{ "__generic", "keyword", "Slang generic type parameter list.", Class::Keyword, TokenKind::Keyword },

			// ---- 引擎契约结构(高亮 Type 色;补全里和类型同档)----
			{ "MaterialInputs", "struct",
				"Per-pixel inputs handed to Evaluate: input.UV / input.WorldPosition / input.WorldNormal / "
				"input.WorldTangent / input.WorldBitangent (contract §2).", Class::EngineType, TokenKind::Type },
			{ "Surface", "struct",
				"What Evaluate returns: surface.BaseColor / Metallic / Roughness / Emissive / Normal / Opacity / "
				"AmbientOcclusion (contract §2).", Class::EngineType, TokenKind::Type },
			{ "input", "MaterialInputs",
				"The MaterialInputs parameter of Evaluate — the engine fixes its name "
				"(Surface Evaluate(MaterialInputs input)), so it is always in scope (contract §2).",
				Class::EngineType, TokenKind::Default },

			// ---- 引擎包装层函数(docs/dev/shader-contract.md §2/§4 与包装模板一致)----
			{ "MakeDefaultSurface", "Surface",
				"Engine default surface. Take it first, then set only the fields this material needs.",
				Class::EngineFunction, TokenKind::Function },
			{ "WeLinearizeColor", "float3(float3)",
				"Convert an sRGB colour to linear RGB. Surface colours are linear; display encoding is handled by the engine.",
				Class::EngineFunction, TokenKind::Function },
			{ "WeDefaultNormal", "float3",
				"Default tangent-space normal (0,0,1) — keeps the normal the engine sampled.", Class::EngineFunction, TokenKind::Function },
			{ "WeIdentityMatrix", "float4x4",
				"Identity matrix helper from the wrapper (skinning fallback).", Class::EngineFunction, TokenKind::Function },
			{ "Evaluate", "Surface(MaterialInputs)",
				"The entry point the engine wraps. Signature is fixed: Surface Evaluate(MaterialInputs input).",
				Class::EngineFunction, TokenKind::Function },

			// ---- Slang 内建 / 纹理方法(插入时补 `()`)----
			{ "abs", "x", "Absolute value.", Class::Intrinsic, TokenKind::Function },
			{ "acos", "x", "Arc cosine (radians).", Class::Intrinsic, TokenKind::Function },
			{ "all", "x", "True when every component is non-zero.", Class::Intrinsic, TokenKind::Function },
			{ "any", "x", "True when any component is non-zero.", Class::Intrinsic, TokenKind::Function },
			{ "asin", "x", "Arc sine (radians).", Class::Intrinsic, TokenKind::Function },
			{ "atan", "x", "Arc tangent (radians).", Class::Intrinsic, TokenKind::Function },
			{ "atan2", "y, x", "Four-quadrant arc tangent (radians).", Class::Intrinsic, TokenKind::Function },
			{ "ceil", "x", "Round up to the nearest integer value.", Class::Intrinsic, TokenKind::Function },
			{ "clamp", "x, min, max", "Clamp x into [min, max].", Class::Intrinsic, TokenKind::Function },
			{ "cos", "x", "Cosine (radians).", Class::Intrinsic, TokenKind::Function },
			{ "cosh", "x", "Hyperbolic cosine.", Class::Intrinsic, TokenKind::Function },
			{ "cross", "a, b", "Cross product of two float3 vectors.", Class::Intrinsic, TokenKind::Function },
			{ "ddx", "x", "Screen-space derivative along x (per 2x2 quad).", Class::Intrinsic, TokenKind::Function },
			{ "ddy", "x", "Screen-space derivative along y (per 2x2 quad).", Class::Intrinsic, TokenKind::Function },
			{ "ddx_coarse", "x", "Coarse screen-space derivative along x.", Class::Intrinsic, TokenKind::Function },
			{ "ddy_coarse", "x", "Coarse screen-space derivative along y.", Class::Intrinsic, TokenKind::Function },
			{ "degrees", "x", "Radians to degrees.", Class::Intrinsic, TokenKind::Function },
			{ "determinant", "m", "Matrix determinant.", Class::Intrinsic, TokenKind::Function },
			{ "distance", "a, b", "Distance between two points.", Class::Intrinsic, TokenKind::Function },
			{ "dot", "a, b", "Dot product.", Class::Intrinsic, TokenKind::Function },
			{ "exp", "x", "e^x.", Class::Intrinsic, TokenKind::Function },
			{ "exp2", "x", "2^x.", Class::Intrinsic, TokenKind::Function },
			{ "faceforward", "n, i, ng", "Flip n so that it faces away from i.", Class::Intrinsic, TokenKind::Function },
			{ "floor", "x", "Round down to the nearest integer value.", Class::Intrinsic, TokenKind::Function },
			{ "fmod", "x, y", "Floating-point remainder of x / y.", Class::Intrinsic, TokenKind::Function },
			{ "frac", "x", "Fractional part.", Class::Intrinsic, TokenKind::Function },
			{ "length", "v", "Vector length.", Class::Intrinsic, TokenKind::Function },
			{ "lerp", "a, b, t", "Linear interpolation: a + (b - a) * t.", Class::Intrinsic, TokenKind::Function },
			{ "log", "x", "Natural logarithm.", Class::Intrinsic, TokenKind::Function },
			{ "log2", "x", "Base-2 logarithm.", Class::Intrinsic, TokenKind::Function },
			{ "mad", "a, b, c", "a * b + c.", Class::Intrinsic, TokenKind::Function },
			{ "max", "a, b", "Component-wise maximum.", Class::Intrinsic, TokenKind::Function },
			{ "min", "a, b", "Component-wise minimum.", Class::Intrinsic, TokenKind::Function },
			{ "modf", "x, out ip", "Split x into fractional and integer parts.", Class::Intrinsic, TokenKind::Function },
			{ "mul", "a, b", "Matrix/vector multiply.", Class::Intrinsic, TokenKind::Function },
			{ "normalize", "v", "Unit vector (zero-length input stays zero).", Class::Intrinsic, TokenKind::Function },
			{ "pow", "x, y", "x^y. Wrap negative bases in saturate() yourself.", Class::Intrinsic, TokenKind::Function },
			{ "radians", "x", "Degrees to radians.", Class::Intrinsic, TokenKind::Function },
			{ "reflect", "i, n", "Reflect vector i about normal n.", Class::Intrinsic, TokenKind::Function },
			{ "refract", "i, n, eta", "Refract vector i through normal n.", Class::Intrinsic, TokenKind::Function },
			{ "round", "x", "Round to the nearest integer value.", Class::Intrinsic, TokenKind::Function },
			{ "rsqrt", "x", "1 / sqrt(x).", Class::Intrinsic, TokenKind::Function },
			{ "saturate", "x", "Clamp to [0,1] (works on scalars and vectors).", Class::Intrinsic, TokenKind::Function },
			{ "sign", "x", "-1, 0 or 1.", Class::Intrinsic, TokenKind::Function },
			{ "sin", "x", "Sine (radians).", Class::Intrinsic, TokenKind::Function },
			{ "sincos", "x, out s, out c", "Sine and cosine in one call.", Class::Intrinsic, TokenKind::Function },
			{ "sinh", "x", "Hyperbolic sine.", Class::Intrinsic, TokenKind::Function },
			{ "smoothstep", "min, max, x", "Smooth Hermite interpolation between min and max.", Class::Intrinsic, TokenKind::Function },
			{ "sqrt", "x", "Square root.", Class::Intrinsic, TokenKind::Function },
			{ "step", "edge, x", "0 when x < edge, 1 otherwise.", Class::Intrinsic, TokenKind::Function },
			{ "tan", "x", "Tangent (radians).", Class::Intrinsic, TokenKind::Function },
			{ "tanh", "x", "Hyperbolic tangent.", Class::Intrinsic, TokenKind::Function },
			{ "transpose", "m", "Matrix transpose.", Class::Intrinsic, TokenKind::Function },
			{ "trunc", "x", "Truncate toward zero.", Class::Intrinsic, TokenKind::Function },
			{ "Sample", "uv", "Sample the combined sampler at uv (no separate sampler parameter).", Class::Intrinsic, TokenKind::Function },
			{ "SampleLevel", "uv, lod", "Sample at an explicit mip level.", Class::Intrinsic, TokenKind::Function },
			{ "SampleCmp", "uv, compare", "Comparison sample (shadow-style lookups).", Class::Intrinsic, TokenKind::Function },
			{ "GetDimensions", "out w, out h", "Query the resource size.", Class::Intrinsic, TokenKind::Function },

			// ---- `//!` 注解行(docs/dev/shader-contract.md §6)----
			{ "param", "annotation",
				"//! param <type> <name> = <default> [min,max] unit(\"\") group(\"\") label(\"\") — the annotation is "
				"the single source of truth for editor rows, cbuffer layout and slots.",
				Class::AnnotationKey, TokenKind::Annotation, Context::Annotation },
			{ "group", "\"Group\"", "group(\"Appearance\") — row group in the material editor (empty = Parameters).",
				Class::AnnotationAttr, TokenKind::Annotation, Context::Annotation, "group(\"\")" },
			{ "label", "\"Label\"", "label(\"Tint\") — display name for the row (empty = the parameter name).",
				Class::AnnotationAttr, TokenKind::Annotation, Context::Annotation, "label(\"\")" },
			{ "unit", "\"%\"", "unit(\"%\") — unit suffix shown next to the value.",
				Class::AnnotationAttr, TokenKind::Annotation, Context::Annotation, "unit(\"\")" },
			{ "[min,max]", "range", "[0,1] — value range; Float/Int only, default [0,1], the default must fall inside.",
				Class::AnnotationSyntax, TokenKind::Annotation, Context::Annotation },
			{ "Float", "annotation type", "Float — number; [min,max] applies.", Class::AnnotationType, TokenKind::Annotation, Context::Annotation },
			{ "Int", "annotation type", "Int — integer; [min,max] applies.", Class::AnnotationType, TokenKind::Annotation, Context::Annotation },
			{ "Bool", "annotation type", "Bool — true / false.", Class::AnnotationType, TokenKind::Annotation, Context::Annotation },
			{ "Vec2", "annotation type", "Vec2 — two comma-separated literals (no [min,max]).",
				Class::AnnotationType, TokenKind::Annotation, Context::Annotation },
			{ "Vec3", "annotation type", "Vec3 — three comma-separated literals (no [min,max]).",
				Class::AnnotationType, TokenKind::Annotation, Context::Annotation },
			{ "Vec4", "annotation type", "Vec4 — four comma-separated literals (no [min,max]).",
				Class::AnnotationType, TokenKind::Annotation, Context::Annotation },
			{ "Color", "annotation type", "Color — r, g, b, a literals (linear; no [min,max]).",
				Class::AnnotationType, TokenKind::Annotation, Context::Annotation },
		};

		inline constexpr std::size_t Count() { return sizeof(kSymbols) / sizeof(kSymbols[0]); }

		// 名字 → 分类。表是编译期常量,索引只在首次调用时建一次(名字重复时后者覆盖前者 ——
		// 探针 mat-intel2-highlight-probe.py 有一条静态断言:表里不允许重复名字)。
		inline const std::unordered_map<std::string_view, const Symbol*>& Index()
		{
			static const std::unordered_map<std::string_view, const Symbol*> index = []()
			{
				std::unordered_map<std::string_view, const Symbol*> built;
				built.reserve(Count() * 2);
				for (std::size_t i = 0; i < Count(); ++i)
					built.emplace(kSymbols[i].Name, &kSymbols[i]);
				return built;
			}();
			return index;
		}

		inline const Symbol* Find(std::string_view word)
		{
			const auto found = Index().find(word);
			return found == Index().end() ? nullptr : found->second;
		}

		// 高亮分类:词 → 颜色(不在表里、或表里标了 Default 的**名字**条目 → 返回 false,
		// 由调用方决定:契约字段 / 点号成员 → Field,其余保持默认色)。
		inline bool HighlightKind(std::string_view word, Wui::WuiCodeTokenKind& out)
		{
			const Symbol* symbol = Find(word);
			if (symbol == nullptr || symbol->Highlight == Wui::WuiCodeTokenKind::Default)
				return false;
			out = symbol->Highlight;
			return true;
		}

		// 引擎契约字段名(MaterialInputs + Surface)= MaterialSurfaceContract.hlsli 的 X-macro 并集。
		// 补全的 `input.` / `surface.` 成员表与高亮读同一份,字段增删只改 .hlsli。
		inline bool IsContractField(std::string_view word)
		{
			for (const MaterialSurfaceContract::FieldInfo& field :
				MaterialSurfaceContract::MaterialInputFields)
				if (word == field.Name)
					return true;
			for (const MaterialSurfaceContract::FieldInfo& field : MaterialSurfaceContract::SurfaceFields)
				if (word == field.Name)
					return true;
			return false;
		}
	}

	// ---- `//!` 注解行(补全闸门 + 参数声明的共同口径)----
	// MAT-INTEL2:`param` 声明的**扫描/解析**从 SlangCompletion.h 挪到这里 —— 它同时被
	// 补全索引(候选 + 文档)与高亮(参数名着色)使用,只有一份实现。
	// `HighlightLineWithAnnotations`(注解体着色)留在 SlangHighlight.h:它依赖高亮器。
	namespace SlangAnnotations
	{
		// 一条 `//! param <type> <name> = <default> …` 声明。
		struct ParamDecl
		{
			std::string Name;
			std::string Type;
			std::string Default;
		};

		inline bool IsSpace(char c)
		{
			return c == ' ' || c == '\t' || c == '\r';
		}

		// 行首(允许前置空白)是不是 `//!` 注解行。
		inline bool IsAnnotationLine(std::string_view line)
		{
			std::size_t index = 0;
			while (index < line.size() && IsSpace(line[index]))
				++index;
			if (line.size() - index < 3)
				return false;
			return line[index] == '/' && line[index + 1] == '/' && line[index + 2] == '!';
		}

		// 注解体起点 = 前导空白 + `//!` + 一个可选空格;不是注解行返回 0。
		inline std::size_t BodyStart(std::string_view line)
		{
			if (!IsAnnotationLine(line))
				return 0;
			std::size_t index = 0;
			while (index < line.size() && IsSpace(line[index]))
				++index;
			index += 3;
			if (index < line.size() && line[index] == ' ')
				++index;
			return index;
		}

		// `//! param <type> <name> = <default> …` → 名字 / 类型 / 默认值(容错:缺空格、`//!param` 也认)。
		// 默认值取到 `[` 或 group(/label(/unit( 之前的文本(注解里这几项互不嵌套)。
		inline bool ParseParamDecl(std::string_view line, ParamDecl& out)
		{
			if (!IsAnnotationLine(line))
				return false;
			const std::string_view body = line.substr(BodyStart(line));
			if (body.size() < 5 || !SlangText::StartsWithIgnoreCase(body, "param"))
				return false;
			std::size_t index = 5;
			auto skipSpace = [&]()
			{
				while (index < body.size() && (body[index] == ' ' || body[index] == '\t'))
					++index;
			};
			auto readWord = [&]() -> std::string_view
			{
				const std::size_t start = index;
				while (index < body.size())
				{
					const char c = body[index];
					const bool ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
						|| (c >= '0' && c <= '9') || c == '_';
					if (!ident)
						break;
					++index;
				}
				return body.substr(start, index - start);
			};
			skipSpace();
			const std::string_view type = readWord();
			skipSpace();
			const std::string_view name = readWord();
			if (type.empty() || name.empty())
				return false;
			out.Type.assign(type);
			out.Name.assign(name);
			out.Default.clear();
			skipSpace();
			if (index >= body.size() || body[index] != '=')
				return true;
			++index;
			while (index < body.size() && (body[index] == ' ' || body[index] == '\t'))
				++index;
			std::size_t end = body.size();
			for (std::size_t probe = index; probe < body.size(); ++probe)
			{
				const bool attribute = body[probe] == '['
					|| (body[probe] == ' ' && (SlangText::StartsWithIgnoreCase(body.substr(probe + 1), "group(")
						|| SlangText::StartsWithIgnoreCase(body.substr(probe + 1), "label(")
						|| SlangText::StartsWithIgnoreCase(body.substr(probe + 1), "unit(")));
				if (attribute)
				{
					end = probe;
					break;
				}
			}
			while (end > index && (body[end - 1] == ' ' || body[end - 1] == '\t'))
				--end;
			out.Default.assign(body.substr(index, end - index));
			return true;
		}

		// 逐行扫描整份源码的 `//! param` 声明(顺序 = 出现顺序;重复名字保留,口径由调用方定)。
		inline void ScanParamDecls(std::string_view fileText, std::vector<ParamDecl>& out)
		{
			out.clear();
			std::size_t start = 0;
			for (;;)
			{
				const std::size_t end = fileText.find('\n', start);
				const std::size_t lineEnd = end == std::string_view::npos ? fileText.size() : end;
				std::string_view line = fileText.substr(start, lineEnd - start);
				if (!line.empty() && line.back() == '\r')
					line.remove_suffix(1);
				ParamDecl decl;
				if (ParseParamDecl(line, decl))
					out.push_back(std::move(decl));
				if (end == std::string_view::npos)
					break;
				start = end + 1;
			}
		}

		// 声明里的参数名(高亮用;保持出现顺序,重复名字去重)。
		inline void CollectDeclaredNames(std::string_view fileText, std::vector<std::string>& out)
		{
			std::vector<ParamDecl> decls;
			ScanParamDecls(fileText, decls);
			out.clear();
			for (const ParamDecl& decl : decls)
			{
				bool seen = false;
				for (const std::string& name : out)
					if (name == decl.Name)
					{
						seen = true;
						break;
					}
				if (!seen)
					out.push_back(decl.Name);
			}
		}
	}
}

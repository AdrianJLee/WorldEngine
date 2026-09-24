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
// 分类(Class)决定"怎么着色 / 进哪个候选池":
//   Keyword / Type                                   → 高亮 Keyword 色;补全 Kind=Keyword
//   EngineType                                       → 高亮 Global 色;补全 Kind=Keyword
//   Intrinsic / EngineFunction                       → 高亮 Global 色;补全 Kind=Method(插入补 `()`)
//   AnnotationKey / AnnotationSyntax                 → 注解行候选 Kind=Field
//   AnnotationAttr                                   → 注解行候选 Kind=Method(插入 `group("")` 形态)
//   AnnotationType                                   → 注解行候选 Kind=Keyword
// 上下文(Context)决定词进哪个候选池;高亮**不看**上下文(一个词出现在代码里就按 Class 着色,
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

		struct Symbol
		{
			std::string_view Name;
			std::string_view Category; // 补全的 "类型" 列
			std::string_view Doc;
			Class Kind = Class::Keyword;
			Context Where = Context::Code;
			std::string_view Insert;   // 补全插入文本(空 = 用 Name;`group` → `group("")`)
		};

		// 唯一表。加词只改这里:高亮与补全同时生效(名字必须唯一 —— 见下方 Index() 的说明)。
		inline constexpr Symbol kSymbols[] = {
			// ---- Slang / HLSL 标量与向量类型 ----
			{ "bool", "scalar", "Boolean scalar (true / false).", Class::Type },
			{ "int", "scalar", "32-bit signed integer scalar.", Class::Type },
			{ "uint", "scalar", "32-bit unsigned integer scalar.", Class::Type },
			{ "dword", "scalar", "32-bit unsigned integer alias (HLSL).", Class::Type },
			{ "half", "scalar", "16-bit float scalar (no implicit conversions).", Class::Type },
			{ "min16float", "scalar", "16-bit minimum-precision float (HLSL).", Class::Type },
			{ "float", "scalar", "32-bit float scalar. Slang does not convert widths implicitly.", Class::Type },
			{ "double", "scalar", "64-bit float scalar (no implicit narrowing to float).", Class::Type },
			{ "float2", "vector", "2-component float vector. Construct explicitly: float2(a, b).", Class::Type },
			{ "float3", "vector", "3-component float vector. Truncate with .xyz/.rgb — implicit width conversion is an error.", Class::Type },
			{ "float4", "vector", "4-component float vector (colour literals are usually float4: r, g, b, a).", Class::Type },
			{ "int2", "vector", "2-component signed integer vector.", Class::Type },
			{ "int3", "vector", "3-component signed integer vector.", Class::Type },
			{ "int4", "vector", "4-component signed integer vector.", Class::Type },
			{ "uint2", "vector", "2-component unsigned integer vector.", Class::Type },
			{ "uint3", "vector", "3-component unsigned integer vector.", Class::Type },
			{ "uint4", "vector", "4-component unsigned integer vector.", Class::Type },
			{ "matrix", "matrix", "Generic matrix type.", Class::Type },
			{ "float2x2", "matrix", "2x2 float matrix.", Class::Type },
			{ "float3x3", "matrix", "3x3 float matrix.", Class::Type },
			{ "float4x4", "matrix", "4x4 float matrix (skin/bone transforms).", Class::Type },
			{ "void", "keyword", "No-return type (helpers that only write their out parameters).", Class::Keyword },

			// ---- 资源 / 采样器类型(严格子集里贴图参数生成 Sampler2D)----
			{ "Texture1D", "resource", "1D texture resource type.", Class::Keyword },
			{ "Texture2D", "resource",
				"Texture resource. As a `//! param` type it means a content-root relative path (\"\" = white 1x1 "
				"fallback); slots t4..t11 by annotation order, max 8, sampled as `name.Sample(uv)`.", Class::Keyword,
				Context::Both },
			{ "Texture3D", "resource", "3D texture resource type.", Class::Keyword },
			{ "TextureCube", "resource", "Cube texture resource type.", Class::Keyword },
			{ "Texture2DArray", "resource", "Arrayed 2D texture resource type.", Class::Keyword },
			{ "TextureCubeArray", "resource", "Arrayed cube texture resource type.", Class::Keyword },
			{ "RWTexture2D", "resource", "Read-write 2D texture resource type (compute).", Class::Keyword },
			{ "ByteAddressBuffer", "resource", "Raw byte-address buffer resource type.", Class::Keyword },
			{ "SamplerState", "resource", "Sampler state resource type.", Class::Keyword },
			{ "SamplerComparisonState", "resource", "Comparison sampler state resource type.", Class::Keyword },
			{ "Sampler1D", "resource", "Combined 1D sampler (engine binds this for Texture1D parameters).", Class::Keyword },
			{ "Sampler2D", "resource", "Combined 2D sampler (engine binds this for Texture2D parameters).", Class::Keyword },
			{ "Sampler3D", "resource", "Combined 3D sampler.", Class::Keyword },
			{ "SamplerCube", "resource", "Combined cube sampler.", Class::Keyword },
			{ "Sampler2DArray", "resource", "Combined arrayed 2D sampler.", Class::Keyword },
			{ "SamplerCubeArray", "resource", "Combined arrayed cube sampler.", Class::Keyword },

			// ---- 声明 / 存储 / 绑定修饰 ----
			{ "static", "keyword", "File-scope static declaration.", Class::Keyword },
			{ "const", "keyword", "Read-only value (usual for helper constants).", Class::Keyword },
			{ "inline", "keyword", "Inline function (helpers are inlined by the compiler anyway).", Class::Keyword },
			{ "uniform", "keyword", "Uniform value (read-only per draw).", Class::Keyword },
			{ "in", "keyword", "Input parameter / input-stage qualifier.", Class::Keyword },
			{ "out", "keyword", "Output parameter / output-stage qualifier.", Class::Keyword },
			{ "inout", "keyword", "Read-write parameter.", Class::Keyword },
			{ "volatile", "keyword", "Volatile value (no optimisation across accesses).", Class::Keyword },
			{ "cbuffer", "keyword", "Constant buffer block (engine generates the real ones from annotations).", Class::Keyword },
			{ "tbuffer", "keyword", "Texture/sampler buffer block.", Class::Keyword },
			{ "register", "keyword", "Explicit register binding — engine binds resources from annotations, so user sources do not write it.", Class::Keyword },
			{ "packoffset", "keyword", "Packing offset inside a constant buffer.", Class::Keyword },
			{ "row_major", "keyword", "Row-major matrix layout.", Class::Keyword },
			{ "column_major", "keyword", "Column-major matrix layout (Slang default).", Class::Keyword },
			{ "groupshared", "keyword", "Compute group-shared storage.", Class::Keyword },
			{ "nointerpolation", "keyword", "Do not interpolate this varying.", Class::Keyword },
			{ "noperspective", "keyword", "Interpolate without perspective correction.", Class::Keyword },
			{ "linear", "keyword", "Linear interpolation modifier.", Class::Keyword },
			{ "centroid", "keyword", "Centroid sampling modifier.", Class::Keyword },
			{ "sample", "keyword", "Sample-frequency interpolation modifier.", Class::Keyword },
			{ "snorm", "keyword", "Signed normalised value qualifier.", Class::Keyword },
			{ "unorm", "keyword", "Unsigned normalised value qualifier.", Class::Keyword },

			// ---- 语句 / 表达式关键字与字面量 ----
			{ "if", "keyword", "Conditional branch — parameter-driven shading lives here.", Class::Keyword },
			{ "else", "keyword", "Alternative branch.", Class::Keyword },
			{ "for", "keyword", "Loop (keep it bounded: this runs per pixel).", Class::Keyword },
			{ "while", "keyword", "Loop with a condition.", Class::Keyword },
			{ "do", "keyword", "Do/while loop body.", Class::Keyword },
			{ "switch", "keyword", "Switch statement (must be exhaustive for scalars).", Class::Keyword },
			{ "case", "keyword", "Switch case label.", Class::Keyword },
			{ "default", "keyword", "Default branch / default switch label.", Class::Keyword },
			{ "break", "keyword", "Break out of the innermost loop or switch.", Class::Keyword },
			{ "continue", "keyword", "Continue the innermost loop.", Class::Keyword },
			{ "return", "keyword", "Return the Surface from Evaluate.", Class::Keyword },
			{ "discard", "keyword", "Discard the fragment (alpha-cutout style effects).", Class::Keyword },
			{ "true", "literal", "Boolean true.", Class::Keyword },
			{ "false", "literal", "Boolean false.", Class::Keyword },
			{ "this", "keyword", "Current instance inside a method.", Class::Keyword },
			{ "new", "keyword", "Allocate a reference-type instance.", Class::Keyword },

			// ---- Slang 语言层(模块 / 泛型 / 接口)—— 高亮它们让"这是 Slang 源"读得出来 ----
			{ "struct", "keyword", "Struct definition (helper data for Evaluate).", Class::Keyword },
			{ "class", "keyword", "Reference type with methods (Slang).", Class::Keyword },
			{ "enum", "keyword", "Enumeration definition (Slang).", Class::Keyword },
			{ "namespace", "keyword", "Namespace declaration (Slang module scoping).", Class::Keyword },
			{ "module", "keyword", "Slang module (compiled separately and imported).", Class::Keyword },
			{ "import", "keyword", "Import a Slang module into this file.", Class::Keyword },
			{ "using", "keyword", "Type alias / using declaration.", Class::Keyword },
			{ "typedef", "keyword", "C-style type alias.", Class::Keyword },
			{ "interface", "keyword",
				"Slang interface: a set of required members that a concrete type implements (generic constraint `T : I`).",
				Class::Keyword },
			{ "associatedtype", "keyword", "Interface member type that implementers bind.", Class::Keyword },
			{ "extension", "keyword", "Extend an existing type with new members (Slang).", Class::Keyword },
			{ "property", "keyword", "Property accessor block (Slang).", Class::Keyword },
			{ "get", "keyword", "Property getter.", Class::Keyword },
			{ "set", "keyword", "Property setter.", Class::Keyword },
			{ "public", "keyword", "Public visibility.", Class::Keyword },
			{ "internal", "keyword", "Module-internal visibility.", Class::Keyword },
			{ "private", "keyword", "Private visibility.", Class::Keyword },
			{ "extern", "keyword", "Externally defined declaration.", Class::Keyword },
			{ "where", "keyword", "Generic constraint clause (`<T> where T : IUVTransform`).", Class::Keyword },
			{ "each", "keyword", "Slang `each` generic expansion.", Class::Keyword },
			{ "expand", "keyword", "Slang `expand` generic expansion.", Class::Keyword },
			{ "func", "keyword", "Function type declarator (Slang).", Class::Keyword },
			{ "let", "keyword", "Immutable local binding.", Class::Keyword },
			{ "var", "keyword", "Mutable local binding.", Class::Keyword },
			{ "__subscript", "keyword", "Slang subscript operator member.", Class::Keyword },
			{ "__init", "keyword", "Slang initialiser member.", Class::Keyword },
			{ "__generic", "keyword", "Slang generic type parameter list.", Class::Keyword },

			// ---- 引擎契约结构(高亮 Global 色;补全里和类型同档)----
			{ "MaterialInputs", "struct",
				"Per-pixel inputs handed to Evaluate: input.UV / input.WorldPosition / input.WorldNormal / "
				"input.WorldTangent / input.WorldBitangent (contract §2).", Class::EngineType },
			{ "Surface", "struct",
				"What Evaluate returns: surface.BaseColor / Metallic / Roughness / Emissive / Normal / Opacity / "
				"AmbientOcclusion (contract §2).", Class::EngineType },
			{ "input", "MaterialInputs",
				"The MaterialInputs parameter of Evaluate — the engine fixes its name "
				"(Surface Evaluate(MaterialInputs input)), so it is always in scope (contract §2).",
				Class::EngineType },

			// ---- 引擎包装层函数(docs/dev/shader-contract.md §2/§4 与包装模板一致)----
			{ "MakeDefaultSurface", "Surface",
				"Engine default surface. Take it first, then set only the fields this material needs.",
				Class::EngineFunction },
			{ "WeLinearizeColor", "float3(float3)",
				"Convert an sRGB colour to linear RGB. Surface colours are linear; display encoding is handled by the engine.",
				Class::EngineFunction },
			{ "WeDefaultNormal", "float3",
				"Default tangent-space normal (0,0,1) — keeps the normal the engine sampled.", Class::EngineFunction },
			{ "WeIdentityMatrix", "float4x4",
				"Identity matrix helper from the wrapper (skinning fallback).", Class::EngineFunction },
			{ "Evaluate", "Surface(MaterialInputs)",
				"The entry point the engine wraps. Signature is fixed: Surface Evaluate(MaterialInputs input).",
				Class::EngineFunction },

			// ---- Slang 内建 / 纹理方法(插入时补 `()`)----
			{ "abs", "x", "Absolute value.", Class::Intrinsic },
			{ "acos", "x", "Arc cosine (radians).", Class::Intrinsic },
			{ "all", "x", "True when every component is non-zero.", Class::Intrinsic },
			{ "any", "x", "True when any component is non-zero.", Class::Intrinsic },
			{ "asin", "x", "Arc sine (radians).", Class::Intrinsic },
			{ "atan", "x", "Arc tangent (radians).", Class::Intrinsic },
			{ "atan2", "y, x", "Four-quadrant arc tangent (radians).", Class::Intrinsic },
			{ "ceil", "x", "Round up to the nearest integer value.", Class::Intrinsic },
			{ "clamp", "x, min, max", "Clamp x into [min, max].", Class::Intrinsic },
			{ "cos", "x", "Cosine (radians).", Class::Intrinsic },
			{ "cosh", "x", "Hyperbolic cosine.", Class::Intrinsic },
			{ "cross", "a, b", "Cross product of two float3 vectors.", Class::Intrinsic },
			{ "ddx", "x", "Screen-space derivative along x (per 2x2 quad).", Class::Intrinsic },
			{ "ddy", "x", "Screen-space derivative along y (per 2x2 quad).", Class::Intrinsic },
			{ "ddx_coarse", "x", "Coarse screen-space derivative along x.", Class::Intrinsic },
			{ "ddy_coarse", "x", "Coarse screen-space derivative along y.", Class::Intrinsic },
			{ "degrees", "x", "Radians to degrees.", Class::Intrinsic },
			{ "determinant", "m", "Matrix determinant.", Class::Intrinsic },
			{ "distance", "a, b", "Distance between two points.", Class::Intrinsic },
			{ "dot", "a, b", "Dot product.", Class::Intrinsic },
			{ "exp", "x", "e^x.", Class::Intrinsic },
			{ "exp2", "x", "2^x.", Class::Intrinsic },
			{ "faceforward", "n, i, ng", "Flip n so that it faces away from i.", Class::Intrinsic },
			{ "floor", "x", "Round down to the nearest integer value.", Class::Intrinsic },
			{ "fmod", "x, y", "Floating-point remainder of x / y.", Class::Intrinsic },
			{ "frac", "x", "Fractional part.", Class::Intrinsic },
			{ "length", "v", "Vector length.", Class::Intrinsic },
			{ "lerp", "a, b, t", "Linear interpolation: a + (b - a) * t.", Class::Intrinsic },
			{ "log", "x", "Natural logarithm.", Class::Intrinsic },
			{ "log2", "x", "Base-2 logarithm.", Class::Intrinsic },
			{ "mad", "a, b, c", "a * b + c.", Class::Intrinsic },
			{ "max", "a, b", "Component-wise maximum.", Class::Intrinsic },
			{ "min", "a, b", "Component-wise minimum.", Class::Intrinsic },
			{ "modf", "x, out ip", "Split x into fractional and integer parts.", Class::Intrinsic },
			{ "mul", "a, b", "Matrix/vector multiply.", Class::Intrinsic },
			{ "normalize", "v", "Unit vector (zero-length input stays zero).", Class::Intrinsic },
			{ "pow", "x, y", "x^y. Wrap negative bases in saturate() yourself.", Class::Intrinsic },
			{ "radians", "x", "Degrees to radians.", Class::Intrinsic },
			{ "reflect", "i, n", "Reflect vector i about normal n.", Class::Intrinsic },
			{ "refract", "i, n, eta", "Refract vector i through normal n.", Class::Intrinsic },
			{ "round", "x", "Round to the nearest integer value.", Class::Intrinsic },
			{ "rsqrt", "x", "1 / sqrt(x).", Class::Intrinsic },
			{ "saturate", "x", "Clamp to [0,1] (works on scalars and vectors).", Class::Intrinsic },
			{ "sign", "x", "-1, 0 or 1.", Class::Intrinsic },
			{ "sin", "x", "Sine (radians).", Class::Intrinsic },
			{ "sincos", "x, out s, out c", "Sine and cosine in one call.", Class::Intrinsic },
			{ "sinh", "x", "Hyperbolic sine.", Class::Intrinsic },
			{ "smoothstep", "min, max, x", "Smooth Hermite interpolation between min and max.", Class::Intrinsic },
			{ "sqrt", "x", "Square root.", Class::Intrinsic },
			{ "step", "edge, x", "0 when x < edge, 1 otherwise.", Class::Intrinsic },
			{ "tan", "x", "Tangent (radians).", Class::Intrinsic },
			{ "tanh", "x", "Hyperbolic tangent.", Class::Intrinsic },
			{ "transpose", "m", "Matrix transpose.", Class::Intrinsic },
			{ "trunc", "x", "Truncate toward zero.", Class::Intrinsic },
			{ "Sample", "uv", "Sample the combined sampler at uv (no separate sampler parameter).", Class::Intrinsic },
			{ "SampleLevel", "uv, lod", "Sample at an explicit mip level.", Class::Intrinsic },
			{ "SampleCmp", "uv, compare", "Comparison sample (shadow-style lookups).", Class::Intrinsic },
			{ "GetDimensions", "out w, out h", "Query the resource size.", Class::Intrinsic },

			// ---- `//!` 注解行(docs/dev/shader-contract.md §6)----
			{ "param", "annotation",
				"//! param <type> <name> = <default> [min,max] unit(\"\") group(\"\") label(\"\") — the annotation is "
				"the single source of truth for editor rows, cbuffer layout and slots.",
				Class::AnnotationKey, Context::Annotation },
			{ "group", "\"Group\"", "group(\"Appearance\") — row group in the material editor (empty = Parameters).",
				Class::AnnotationAttr, Context::Annotation, "group(\"\")" },
			{ "label", "\"Label\"", "label(\"Tint\") — display name for the row (empty = the parameter name).",
				Class::AnnotationAttr, Context::Annotation, "label(\"\")" },
			{ "unit", "\"%\"", "unit(\"%\") — unit suffix shown next to the value.",
				Class::AnnotationAttr, Context::Annotation, "unit(\"\")" },
			{ "[min,max]", "range", "[0,1] — value range; Float/Int only, default [0,1], the default must fall inside.",
				Class::AnnotationSyntax, Context::Annotation },
			{ "Float", "annotation type", "Float — number; [min,max] applies.", Class::AnnotationType, Context::Annotation },
			{ "Int", "annotation type", "Int — integer; [min,max] applies.", Class::AnnotationType, Context::Annotation },
			{ "Bool", "annotation type", "Bool — true / false.", Class::AnnotationType, Context::Annotation },
			{ "Vec2", "annotation type", "Vec2 — two comma-separated literals (no [min,max]).",
				Class::AnnotationType, Context::Annotation },
			{ "Vec3", "annotation type", "Vec3 — three comma-separated literals (no [min,max]).",
				Class::AnnotationType, Context::Annotation },
			{ "Vec4", "annotation type", "Vec4 — four comma-separated literals (no [min,max]).",
				Class::AnnotationType, Context::Annotation },
			{ "Color", "annotation type", "Color — r, g, b, a literals (linear; no [min,max]).",
				Class::AnnotationType, Context::Annotation },
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

		// 高亮分类:词 → 颜色(不在表里返回 false,由调用方决定(点号成员/默认色))。
		// 与补全 Kind 对齐:候选 Kind=Keyword/Field(关键字/类型/`param`)→ Keyword 色;
		// Kind=Method/Global(内建/引擎函数/`group(...)` 这类属性)→ Global 色。
		inline bool HighlightKind(std::string_view word, Wui::WuiCodeTokenKind& out)
		{
			const Symbol* symbol = Find(word);
			if (symbol == nullptr)
				return false;
			switch (symbol->Kind)
			{
				case Class::Keyword:
				case Class::Type:
				case Class::AnnotationKey:    // Kind=Field
				case Class::AnnotationType:   // Kind=Keyword
				case Class::AnnotationSyntax: // Kind=Field(语法片段,一般不是标识符)
					out = Wui::WuiCodeTokenKind::Keyword;
					return true;
				case Class::EngineType:
				case Class::Intrinsic:
				case Class::EngineFunction:
				case Class::AnnotationAttr:   // Kind=Method:group() / label() / unit()
					out = Wui::WuiCodeTokenKind::Global;
					return true;
				default:
					return false;
			}
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

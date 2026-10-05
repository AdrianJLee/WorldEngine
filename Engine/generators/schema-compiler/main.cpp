// WorldEngine schema-compiler
// 解析受限的 WE_SCHEMA_BODY / WE_ENUM_SCHEMA 注解块,生成模块注册 TU:
//   - GeneratedAccess<T> / GeneratedEnum<E> 显式特化(类型化 per-field 访问器)
//   - Register<Module>SchemaModule / Unregister<Module>SchemaModule
// 生成物确定性、提交进仓库,构建机不依赖本工具。

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	struct SourcePos
	{
		int Line = 1;
		int Column = 1;
	};

	struct Token
	{
		enum class Type { Identifier, Number, String, Punct, End };
		Type type = Type::End;
		std::string Text;
		SourcePos Pos;
	};

	[[noreturn]] void Fail(const std::string& file, const SourcePos& pos, const std::string& message)
	{
		throw std::runtime_error(file + "(" + std::to_string(pos.Line) + "): " + message);
	}

	std::string Unquote(const std::string& text)
	{
		if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
			return text.substr(1, text.size() - 2);
		return text;
	}

	// WE_SCHEMA_META 的文本会被原样写进生成物的 C++ 字符串字面量:拒绝转义/引号/控制字符,
	// 保证生成物总是合法 C++(不做转义处理,保持生成器行为可预测)。
	void ValidateMetaText(const std::string& file, const SourcePos& pos, const std::string& attr, const std::string& value)
	{
		for (const char c : value)
		{
			if (c == '\\' || c == '"')
				Fail(file, pos, "WE_SCHEMA_META " + attr + " must not contain quotes or backslashes (it is emitted verbatim into a C++ string literal)");
			if (c == '\n' || c == '\r' || c == '\t')
				Fail(file, pos, "WE_SCHEMA_META " + attr + " must be a single line without control characters");
		}
	}

	class Tokenizer
	{
	public:
		explicit Tokenizer(const std::string& source, const std::string& file)
			: m_Source(source), m_File(file)
		{
		}

		std::vector<Token> Run()
		{
			std::vector<Token> tokens;
			while (true)
			{
				SkipTrivia();
				if (AtEnd())
					break;
				const SourcePos start = m_Pos;
				const char c = Peek();
				if (c == '"')
				{
					Token token;
					token.type = Token::Type::String;
					token.Pos = start;
					token.Text = ReadString();
					tokens.push_back(std::move(token));
				}
				else if (IsNameStart(c))
				{
					Token token;
					token.type = Token::Type::Identifier;
					token.Pos = start;
					token.Text = ReadName();
					tokens.push_back(std::move(token));
				}
				else if (c >= '0' && c <= '9')
				{
					Token token;
					token.type = Token::Type::Number;
					token.Pos = start;
					token.Text = ReadNumber();
					tokens.push_back(std::move(token));
				}
				else
				{
					Token token;
					token.type = Token::Type::Punct;
					token.Pos = start;
					token.Text = std::string(1, Take());
					tokens.push_back(std::move(token));
				}
			}
			Token end;
			end.type = Token::Type::End;
			end.Pos = m_Pos;
			tokens.push_back(std::move(end));
			return tokens;
		}

	private:
		bool AtEnd() const { return m_Index >= m_Source.size(); }
		char Peek() const { return m_Source[m_Index]; }
		char Take()
		{
			const char c = m_Source[m_Index++];
			if (c == '\n') { ++m_Pos.Line; m_Pos.Column = 1; }
			else ++m_Pos.Column;
			return c;
		}

		static bool IsNameStart(char c)
		{
			return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
		}
		static bool IsNameChar(char c) { return IsNameStart(c) || (c >= '0' && c <= '9'); }

		void SkipTrivia()
		{
			while (!AtEnd())
			{
				const char c = Peek();
				if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { Take(); continue; }
				if (c == '/' && m_Index + 1 < m_Source.size() && m_Source[m_Index + 1] == '/')
				{
					while (!AtEnd() && Peek() != '\n') Take();
					continue;
				}
				if (c == '/' && m_Index + 1 < m_Source.size() && m_Source[m_Index + 1] == '*')
				{
					Take(); Take();
					while (!AtEnd())
					{
						if (Peek() == '*' && m_Index + 1 < m_Source.size() && m_Source[m_Index + 1] == '/') { Take(); Take(); break; }
						Take();
					}
					continue;
				}
				break;
			}
		}

		std::string ReadString()
		{
			Take(); // 起始引号
			std::string out = "\"";
			while (!AtEnd())
			{
				const char c = Take();
				out.push_back(c);
				if (c == '\\' && !AtEnd())
				{
					out.push_back(Take());
					continue;
				}
				if (c == '"')
					break;
				if (c == '\n')
					Fail(m_File, m_Pos, "unterminated string literal");
			}
			return out;
		}

		std::string ReadName()
		{
			std::string out;
			while (!AtEnd() && IsNameChar(Peek()))
				out.push_back(Take());
			return out;
		}

		std::string ReadNumber()
		{
			std::string out;
			while (!AtEnd())
			{
				const char c = Peek();
				if (IsNameChar(c) || c == '.')
				{
					out.push_back(Take());
					continue;
				}
				break;
			}
			return out;
		}

		const std::string& m_Source;
		const std::string& m_File;
		size_t m_Index = 0;
		SourcePos m_Pos;
	};

	struct Attr
	{
		std::string Name;
		std::vector<std::vector<Token>> Args; // 顶层逗号分隔的参数组
		SourcePos Pos;
	};

	struct FieldDecl
	{
		std::string Name;
		std::string Kind;
		std::vector<Attr> Attrs;
		SourcePos Pos;
	};

	struct StructDecl
	{
		std::string Module;
		std::string Type;
		std::string Category;
		// WE_SCHEMA_META(Category(...), Doc(...)):类型级描述元数据(可只写其一;空 = 未填)。
		std::string MetaCategory;
		std::string MetaDoc;
		// WE_SCHEMA_META(..., Core()):核心组件(编辑器不允许移除)。
		bool MetaCore = false;
		bool HasMeta = false;
		SourcePos MetaPos;
		std::vector<FieldDecl> Fields;
		SourcePos Pos;
		std::string File;
	};

	struct EnumDecl
	{
		std::string Module;
		std::string Type;
		std::string Underlying;
		std::vector<std::pair<std::string, SourcePos>> Values;
		SourcePos Pos;
		std::string File;
	};

	class Parser
	{
	public:
		explicit Parser(const std::vector<Token>& tokens, const std::string& file)
			: m_Tokens(tokens), m_File(file)
		{
		}

		void Run(std::vector<StructDecl>& structs, std::vector<EnumDecl>& enums)
		{
			while (!AtEnd())
			{
				if (PeekIs(Token::Type::Identifier, "WE_SCHEMA_BODY"))
					structs.push_back(ParseStruct());
				else if (PeekIs(Token::Type::Identifier, "WE_ENUM_SCHEMA"))
					enums.push_back(ParseEnum());
				else
					Next();
			}
		}

	private:
		bool AtEnd() const { return m_Index >= m_Tokens.size(); }
		const Token& Peek() const { return m_Tokens[m_Index]; }
		const Token& Next() { return m_Tokens[m_Index++]; }

		bool PeekIs(Token::Type type, const std::string& text) const
		{
			return !AtEnd() && Peek().type == type && Peek().Text == text;
		}

		void Expect(Token::Type type, const char* what)
		{
			if (AtEnd() || Peek().type != type)
				Fail(m_File, Peek().Pos, std::string("expected ") + what);
			Next();
		}

		const Token& ExpectIdentifier(const char* what)
		{
			if (AtEnd() || Peek().type != Token::Type::Identifier)
				Fail(m_File, Peek().Pos, std::string("expected ") + what);
			return Next();
		}

		StructDecl ParseStruct()
		{
			StructDecl decl;
			decl.Pos = Next().Pos; // WE_SCHEMA_BODY
			decl.File = m_File;
			Expect(Token::Type::Punct, "'('");
			decl.Module = ExpectIdentifier("module name").Text;
			Expect(Token::Type::Punct, "','");
			decl.Type = ExpectIdentifier("type name").Text;
			Expect(Token::Type::Punct, "','");
			decl.Category = ExpectIdentifier("category").Text;
			Expect(Token::Type::Punct, "')'");

			while (!AtEnd() && !PeekIs(Token::Type::Identifier, "WE_SCHEMA_END"))
			{
				if (PeekIs(Token::Type::Punct, ";"))
				{
					Next();
					continue;
				}
				if (PeekIs(Token::Type::Identifier, "WE_SCHEMA_META"))
				{
					ParseStructMeta(decl);
					continue;
				}
				if (!PeekIs(Token::Type::Identifier, "WE_FIELD"))
					Fail(m_File, Peek().Pos, "only WE_SCHEMA_META and WE_FIELD declarations are allowed inside a WE_SCHEMA_BODY block");
				decl.Fields.push_back(ParseField());
			}
			ExpectIdentifier("WE_SCHEMA_END");
			if (!AtEnd() && PeekIs(Token::Type::Punct, ";"))
				Next();
			return decl;
		}

		// 类型级描述元数据:WE_SCHEMA_META(Category("Rendering/Light"), Doc("一句话说明"), Core())
		// 接受 Category / Doc(单个字符串字面量)与 Core(无参),每个最多一次,顺序任意。
		void ParseStructMeta(StructDecl& decl)
		{
			const SourcePos pos = Next().Pos; // WE_SCHEMA_META
			if (decl.HasMeta)
				Fail(m_File, pos, "duplicate WE_SCHEMA_META for '" + decl.Type + "' (only one per type)");
			decl.HasMeta = true;
			decl.MetaPos = pos;
			Expect(Token::Type::Punct, "'('");
			if (!PeekIs(Token::Type::Punct, ")"))
			{
				while (true)
				{
					const Attr attr = ParseAttr();
					if (attr.Name != "Category" && attr.Name != "Doc" && attr.Name != "Core")
						Fail(m_File, attr.Pos, "unknown WE_SCHEMA_META attribute '" + attr.Name + "' (expected Category/Doc/Core)");
					if (attr.Name == "Core")
					{
						if (!(attr.Args.empty() || (attr.Args.size() == 1 && attr.Args[0].empty())))
							Fail(m_File, attr.Pos, "WE_SCHEMA_META attribute 'Core' takes no arguments");
						if (decl.MetaCore)
							Fail(m_File, attr.Pos, "duplicate WE_SCHEMA_META attribute 'Core'");
						decl.MetaCore = true;
					}
					else
					{
						if (attr.Args.size() != 1 || attr.Args[0].size() != 1 || attr.Args[0][0].type != Token::Type::String)
							Fail(m_File, attr.Pos, "WE_SCHEMA_META attribute '" + attr.Name + "' expects a single string literal");
						const std::string value = Unquote(attr.Args[0][0].Text);
						ValidateMetaText(m_File, attr.Pos, attr.Name, value);
						std::string& target = attr.Name == "Category" ? decl.MetaCategory : decl.MetaDoc;
						if (!target.empty())
							Fail(m_File, attr.Pos, "duplicate WE_SCHEMA_META attribute '" + attr.Name + "'");
						target = value;
					}
					if (!AtEnd() && PeekIs(Token::Type::Punct, ","))
					{
						Next();
						continue;
					}
					break;
				}
			}
			Expect(Token::Type::Punct, "')'");
			if (!AtEnd() && PeekIs(Token::Type::Punct, ";"))
				Next();

			// 分类是分层路径:派生本地化键 schema.category.<路径,把 / 换成 .>,所以禁止空段。
			if (!decl.MetaCategory.empty()
				&& (decl.MetaCategory.front() == '/' || decl.MetaCategory.back() == '/'
					|| decl.MetaCategory.find("//") != std::string::npos))
				Fail(m_File, decl.MetaPos, "WE_SCHEMA_META Category must be a slash-separated path without empty segments (e.g. \"Rendering/Light\")");
		}

		// 属性词法:WE_FIELD 的 Attr(...) 与 WE_SCHEMA_META 的 Category(...)/Doc(...) 共用。
		// 调用前逗号已消费,当前 token 是属性名。
		Attr ParseAttr()
		{
			Attr attr;
			attr.Pos = ExpectIdentifier("attribute name").Pos;
			attr.Name = m_Tokens[m_Index - 1].Text;
			if (!AtEnd() && PeekIs(Token::Type::Punct, "("))
			{
				Next();
				std::vector<Token> raw;
				int depth = 1;
				while (!AtEnd() && depth > 0)
				{
					const Token token = Next();
					if (token.type == Token::Type::Punct)
					{
						if (token.Text == "(") ++depth;
						else if (token.Text == ")") --depth;
					}
					if (depth > 0)
						raw.push_back(token);
				}
				if (depth != 0)
					Fail(m_File, attr.Pos, "unbalanced parentheses in attribute '" + attr.Name + "'");
				std::vector<Token> group;
				for (const Token& token : raw)
				{
					if (token.type == Token::Type::Punct && token.Text == ",")
					{
						attr.Args.push_back(std::move(group));
						group.clear();
						continue;
					}
					group.push_back(token);
				}
				attr.Args.push_back(std::move(group));
			}
			return attr;
		}

		FieldDecl ParseField()
		{
			FieldDecl field;
			field.Pos = Next().Pos; // WE_FIELD
			Expect(Token::Type::Punct, "'('");
			field.Name = ExpectIdentifier("field name").Text;
			Expect(Token::Type::Punct, "','");
			field.Kind = ExpectIdentifier("field kind").Text;
			while (!AtEnd() && PeekIs(Token::Type::Punct, ","))
			{
				Next();
				field.Attrs.push_back(ParseAttr());
			}
			Expect(Token::Type::Punct, "')'");
			Expect(Token::Type::Punct, "';'");
			return field;
		}

		EnumDecl ParseEnum()
		{
			EnumDecl decl;
			decl.Pos = Next().Pos; // WE_ENUM_SCHEMA
			decl.File = m_File;
			Expect(Token::Type::Punct, "'('");
			decl.Module = ExpectIdentifier("module name").Text;
			Expect(Token::Type::Punct, "','");
			decl.Type = ExpectIdentifier("enum type name").Text;
			Expect(Token::Type::Punct, "','");
			decl.Underlying = ExpectIdentifier("underlying kind").Text;
			Expect(Token::Type::Punct, "')'");

			while (!AtEnd() && !PeekIs(Token::Type::Identifier, "WE_ENUM_END"))
			{
				if (PeekIs(Token::Type::Punct, ";"))
				{
					Next();
					continue;
				}
				if (!PeekIs(Token::Type::Identifier, "WE_ENUM_VALUE"))
					Fail(m_File, Peek().Pos, "only WE_ENUM_VALUE declarations are allowed inside a WE_ENUM_SCHEMA block");
				Next();
				Expect(Token::Type::Punct, "'('");
				const Token value = ExpectIdentifier("enumerator name");
				Expect(Token::Type::Punct, "')'");
				if (!AtEnd() && PeekIs(Token::Type::Punct, ";"))
					Next();
				decl.Values.push_back({ value.Text, value.Pos });
			}
			ExpectIdentifier("WE_ENUM_END");
			if (!AtEnd() && PeekIs(Token::Type::Punct, ";"))
				Next();
			return decl;
		}

		const std::vector<Token>& m_Tokens;
		const std::string& m_File;
		size_t m_Index = 0;
	};

	uint64_t Fnv1a64(const std::string& text)
	{
		uint64_t hash = 14695981039346656037ull;
		for (unsigned char c : text)
		{
			hash ^= c;
			hash *= 1099511628211ull;
		}
		return hash;
	}

	const std::set<std::string>& StructKinds()
	{
		static const std::set<std::string> kinds = {
			"Bool", "Int8", "Int16", "Int32", "Int64", "UInt8", "UInt16", "UInt32", "UInt64",
			"Float", "Double", "Vec2", "Vec3", "Vec4", "IVec2", "IVec3", "IVec4",
			"UVec2", "UVec3", "UVec4", "Quat", "Mat3", "Mat4", "String", "Name", "Text", "Enum", "Asset", "Object"
		};
		return kinds;
	}

	const std::set<std::string>& EnumUnderlyings()
	{
		static const std::set<std::string> kinds = { "Int8", "Int16", "Int32", "Int64", "UInt8", "UInt16", "UInt32", "UInt64" };
		return kinds;
	}

	const std::set<std::string>& NumericKinds()
	{
		static const std::set<std::string> kinds = {
			"Int8", "Int16", "Int32", "Int64", "UInt8", "UInt16", "UInt32", "UInt64", "Float", "Double"
		};
		return kinds;
	}

	const std::string& CppType(const std::string& kind)
	{
		static const std::map<std::string, std::string> types = {
			{ "Bool", "bool" }, { "Int8", "int8_t" }, { "Int16", "int16_t" }, { "Int32", "int32_t" }, { "Int64", "int64_t" },
			{ "UInt8", "uint8_t" }, { "UInt16", "uint16_t" }, { "UInt32", "uint32_t" }, { "UInt64", "uint64_t" },
			{ "Float", "float" }, { "Double", "double" },
			{ "Vec2", "glm::vec2" }, { "Vec3", "glm::vec3" }, { "Vec4", "glm::vec4" },
			{ "IVec2", "glm::ivec2" }, { "IVec3", "glm::ivec3" }, { "IVec4", "glm::ivec4" },
			{ "UVec2", "glm::uvec2" }, { "UVec3", "glm::uvec3" }, { "UVec4", "glm::uvec4" },
			{ "Quat", "glm::quat" }, { "Mat3", "glm::mat3" }, { "Mat4", "glm::mat4" },
			{ "String", "std::string" },
			{ "Name", "World::NameId" },
		};
		return types.at(kind);
	}

	// CPPT-6-FIX2:类型零值必须是**这个 kind 自己那一支** variant 备选(与编辑器的
	// DefaultPropertyValue / ComponentPropertyModel::ValueMatchesKind 同一份口径)。
	// 旧实现把 Float 写成 `Value(0.0)`(double)、整数族写成 `Value(0)`(int):非 Script 类别
	// (Struct/Component 的字段)的声明默认值因此类型不符 —— 嵌套 struct 的 Float 子字段在检视器里
	// 判"值与声明类型不符",只画 `—` 且不可编辑(Stats.Health 实测)。
	std::string DefaultValue(const std::string& kind)
	{
		if (kind == "Bool") return "Value(false)";
		if (kind == "Int8") return "Value(static_cast<int8_t>(0))";
		if (kind == "Int16") return "Value(static_cast<int16_t>(0))";
		if (kind == "Int32") return "Value(static_cast<int32_t>(0))";
		if (kind == "Int64") return "Value(static_cast<int64_t>(0))";
		if (kind == "UInt8") return "Value(static_cast<uint8_t>(0))";
		if (kind == "UInt16") return "Value(static_cast<uint16_t>(0))";
		if (kind == "UInt32") return "Value(static_cast<uint32_t>(0))";
		if (kind == "UInt64") return "Value(static_cast<uint64_t>(0))";
		if (kind == "Float") return "Value(0.0f)";
		if (kind == "Double") return "Value(0.0)";
		if (kind == "String") return "Value(std::string())";
		if (kind == "Text") return "Value(std::string())";
		if (kind == "Vec2") return "Value(glm::vec2(0.0f))";
		if (kind == "Vec3") return "Value(glm::vec3(0.0f))";
		if (kind == "Vec4") return "Value(glm::vec4(0.0f))";
		if (kind == "IVec2") return "Value(glm::ivec2(0))";
		if (kind == "IVec3") return "Value(glm::ivec3(0))";
		if (kind == "IVec4") return "Value(glm::ivec4(0))";
		if (kind == "UVec2") return "Value(glm::uvec2(0u))";
		if (kind == "UVec3") return "Value(glm::uvec3(0u))";
		if (kind == "UVec4") return "Value(glm::uvec4(0u))";
		if (kind == "Quat") return "Value(glm::quat(1.0f, 0.0f, 0.0f, 0.0f))";
		if (kind == "Mat3") return "Value(glm::mat3(1.0f))";
		if (kind == "Mat4") return "Value(glm::mat4(1.0f))";
		// Enum/Object/Asset 有各自的默认值分支(见 EmitStruct 里的 def 组装),不经过这里;
		// 其余 kind 在解析期已被 StructKinds() 白名单拒绝 —— 返回未设值而不是 0,避免静默造出错误变体。
		return "Value()";
	}

	std::string JoinTokens(const std::vector<Token>& tokens)
	{
		std::string out;
		for (const Token& token : tokens)
		{
			if (!out.empty())
				out.push_back(' ');
			out += token.Text;
		}
		return out;
	}

	std::string JoinCompact(const std::vector<Token>& tokens)
	{
		std::string out;
		for (const Token& token : tokens)
			out += token.Text;
		return out;
	}

	struct EnumInfo
	{
		bool IsSigned = true;
		uint8_t Size = 4;
	};

	EnumInfo UnderlyingInfo(const std::string& underlying)
	{
		EnumInfo info;
		info.IsSigned = underlying.rfind('U', 0) != 0;
		if (underlying.find("64") != std::string::npos)
			info.Size = 8;
		else if (underlying.find("16") != std::string::npos)
			info.Size = 2;
		else if (underlying.find("8") != std::string::npos)
			info.Size = 1;
		else
			info.Size = 4;
		return info;
	}

	struct FieldOptions
	{
		std::string DisplayName;
		std::string Group;
		std::optional<float> Min;
		std::optional<float> Max;
		bool ReadOnly = false;
		bool Transient = false;
		// Entity32:UInt64 字段在 C++ 侧是 entt::entity(32 位句柄)。
		// 生成的访问器做 uint32<->entity 打包,NULL 用 entt::null 表示;默认值也是 entt::null。
		bool Entity32 = false;
		std::optional<uint64_t> Id;
		std::optional<std::string> DefaultExpr;
		std::optional<std::string> Of;
		// Of(...) 的实参是不是字符串字面量(Asset 的资产类型名写法:`Of("Material")`)。
		// 容器元素类型解析要靠它区分"资产类型字符串"与"命名 struct/enum"。
		bool OfIsString = false;
		// ---- P4-U9:编辑期字段语义(见 Schema.h 的 FieldMetadata) ----
		std::string Doc;                        // 一句话说明(空 = 未填)
		bool IsColor = false;                   // Color() → Vec3/Vec4 用取色器
		std::string AssetType;                  // Asset("Material") → 字符串字段是资产路径
		std::vector<std::string> Choices;       // Choices("cube","plane") → 字符串字段固定集合
		// ---- CPPT-2:计量单位与拖拽步长(编辑期提示;进 FieldMetadata 尾部) ----
		std::string Unit;                       // Unit("m") → 行后缀
		std::optional<float> Step;              // Step(0.1) → 数值拖拽步长
		// ---- CPPT-6:容器形状(WE_FIELD(Name, Array|Map, Of(...))) ----
		// 元素类型在 ElementTypeOf 里解析:叶子 kind / 已注册的命名 struct / 命名 enum /
		// 资产类型字符串(Of("Material"))。Map 键固定 std::string。
	};

	FieldOptions ParseOptions(const FieldDecl& field, const std::string& file)
	{
		FieldOptions options;
		// 无参属性既接受 `ReadOnly` 也接受 `ReadOnly()`(后者语法上是一个空分组)。
		const auto noArgs = [](const Attr& attr)
		{
			return attr.Args.empty() || (attr.Args.size() == 1 && attr.Args[0].empty());
		};
		for (const Attr& attr : field.Attrs)
		{
			if (attr.Name == "Group" || attr.Name == "DisplayName")
			{
				if (attr.Args.size() != 1 || attr.Args[0].size() != 1 || attr.Args[0][0].type != Token::Type::String)
					Fail(file, attr.Pos, "attribute '" + attr.Name + "' expects a single string literal");
				const std::string value = Unquote(attr.Args[0][0].Text);
				if (attr.Name == "Group") options.Group = value;
				else options.DisplayName = value;
			}
			else if (attr.Name == "Range")
			{
				if (attr.Args.size() != 2)
					Fail(file, attr.Pos, "attribute 'Range' expects two numbers");
				options.Min = std::stof(JoinTokens(attr.Args[0]));
				options.Max = std::stof(JoinTokens(attr.Args[1]));
			}
			else if (attr.Name == "ReadOnly")
			{
				if (!noArgs(attr)) Fail(file, attr.Pos, "attribute 'ReadOnly' takes no arguments");
				options.ReadOnly = true;
			}
			else if (attr.Name == "Transient")
			{
				if (!noArgs(attr)) Fail(file, attr.Pos, "attribute 'Transient' takes no arguments");
				options.Transient = true;
			}
			else if (attr.Name == "Entity32")
			{
				if (!noArgs(attr)) Fail(file, attr.Pos, "attribute 'Entity32' takes no arguments");
				options.Entity32 = true;
			}
			else if (attr.Name == "Id")
			{
				if (attr.Args.size() != 1 || attr.Args[0].size() != 1 || attr.Args[0][0].type != Token::Type::Number)
					Fail(file, attr.Pos, "attribute 'Id' expects one number");
				options.Id = std::stoull(attr.Args[0][0].Text, nullptr, 0);
			}
			else if (attr.Name == "Default")
			{
				if (attr.Args.size() != 1 || attr.Args[0].empty())
					Fail(file, attr.Pos, "attribute 'Default' needs a single expression");
				// 紧凑拼接:表达式里会出现 "entt::null" 这类带 :: 的限定名,插空格会拼出非法 C++。
				options.DefaultExpr = JoinCompact(attr.Args[0]);
			}
			else if (attr.Name == "Of")
			{
				if (attr.Args.size() != 1 || attr.Args[0].empty())
					Fail(file, attr.Pos, "attribute 'Of' expects a type name");
				const bool isString = attr.Args[0].size() == 1 && attr.Args[0][0].type == Token::Type::String;
				options.Of = isString ? Unquote(attr.Args[0][0].Text) : JoinCompact(attr.Args[0]);
				options.OfIsString = isString;
			}
			else if (attr.Name == "Doc")
			{
				if (attr.Args.size() != 1 || attr.Args[0].size() != 1 || attr.Args[0][0].type != Token::Type::String)
					Fail(file, attr.Pos, "attribute 'Doc' expects a single string literal");
				const std::string value = Unquote(attr.Args[0][0].Text);
				ValidateMetaText(file, attr.Pos, attr.Name, value);
				if (!options.Doc.empty())
					Fail(file, attr.Pos, "duplicate attribute 'Doc'");
				options.Doc = value;
			}
			else if (attr.Name == "Color")
			{
				if (!noArgs(attr))
					Fail(file, attr.Pos, "attribute 'Color' takes no arguments");
				options.IsColor = true;
			}
			else if (attr.Name == "Asset")
			{
				if (attr.Args.size() != 1 || attr.Args[0].size() != 1 || attr.Args[0][0].type != Token::Type::String)
					Fail(file, attr.Pos, "attribute 'Asset' expects a single string literal (asset type name)");
				options.AssetType = Unquote(attr.Args[0][0].Text);
			}
			else if (attr.Name == "Choices")
			{
				if (attr.Args.empty())
					Fail(file, attr.Pos, "attribute 'Choices' expects at least one string literal");
				for (const std::vector<Token>& group : attr.Args)
				{
					if (group.size() != 1 || group[0].type != Token::Type::String)
						Fail(file, attr.Pos, "attribute 'Choices' expects string literals only");
					options.Choices.push_back(Unquote(group[0].Text));
				}
			}
			else if (attr.Name == "Unit")
			{
				if (attr.Args.size() != 1 || attr.Args[0].size() != 1 || attr.Args[0][0].type != Token::Type::String)
					Fail(file, attr.Pos, "attribute 'Unit' expects a single string literal");
				if (!options.Unit.empty())
					Fail(file, attr.Pos, "duplicate attribute 'Unit'");
				options.Unit = Unquote(attr.Args[0][0].Text);
			}
			else if (attr.Name == "Step")
			{
				if (attr.Args.size() != 1 || attr.Args[0].empty())
					Fail(file, attr.Pos, "attribute 'Step' expects one number");
				options.Step = std::stof(JoinTokens(attr.Args[0]));
				if (!(options.Step.value() > 0.0f))
					Fail(file, attr.Pos, "attribute 'Step' must be a positive number");
			}
			else
			{
				Fail(file, attr.Pos, "unknown attribute '" + attr.Name + "'");
			}
		}
		return options;
	}

	std::string OptionalText(std::optional<float> value)
	{
		if (!value.has_value())
			return "std::nullopt";
		std::ostringstream out;
		out << "std::optional<float>(" << std::to_string(value.value()) << "f)";
		return out.str();
	}

	std::string ShortName(const std::string& qualified)
	{
		const size_t colon = qualified.rfind("::");
		return colon == std::string::npos ? qualified : qualified.substr(colon + 2);
	}

	// 依据 manifest 的限定名解析 Of(短名) 的 C++ 限定名。P0 约束:引用目标与本模块同模块。
	struct QualifiedIndex
	{
		std::map<std::string, std::string> Structs;
		std::map<std::string, std::string> Enums;

		void Add(const std::string& kind, const std::string& qualified)
		{
			if (kind == "struct") Structs[ShortName(qualified)] = qualified;
			else Enums[ShortName(qualified)] = qualified;
		}

		std::string Struct(const std::string& shortName, const std::string& file, const SourcePos& pos) const
		{
			const auto it = Structs.find(shortName);
			if (it == Structs.end())
				Fail(file, pos, "cannot qualify struct '" + shortName + "' (not in manifest)");
			return it->second;
		}

		std::string Enum(const std::string& shortName, const std::string& file, const SourcePos& pos) const
		{
			const auto it = Enums.find(shortName);
			if (it == Enums.end())
				Fail(file, pos, "cannot qualify enum '" + shortName + "' (not in manifest)");
			return it->second;
		}
	};

	// CPPT-6:容器元素类型。Of(x) 的 x 可以是:
	//   · 叶子 kind 名(Bool / Int32 / Float / Vec3 / … / String);
	//   · 已注册的命名 struct(→ Object,走 GetElementNested 递归读写);
	//   · 已注册的命名 enum(→ Enum,整数读写 + GetEnum 判有符号);
	//   · 资产类型字符串(Of("Material") 或等价的 Of(Asset("Material")) → Asset)。
	// 更深的匿名嵌套(Of(Array(...)))不支持 —— 用命名 struct 再套容器表达。
	struct ElementType
	{
		std::string Kind;       // 叶 kind 名,或 Object / Enum / Asset
		std::string Struct;     // Kind==Object:限定名(如 Game::StatEntry)
		std::string Enum;       // Kind==Enum:枚举短名
		std::string AssetType;  // Kind==Asset:资产类型名
	};

	ElementType ResolveElementType(const FieldDecl& field, const FieldOptions& options, const StructDecl& decl,
		const QualifiedIndex& index, const std::map<std::string, EnumDecl>& enumsByName)
	{
		if (!options.Of.has_value())
			Fail(decl.File, field.Pos, "field '" + field.Name + "' of shape " + field.Kind +
				" requires Of(<element kind>) (e.g. Of(Float) / Of(Vec3) / Of(MyStruct))");
		const std::string& of = options.Of.value();
		if (of == "Array" || of == "Map" || of.rfind("Array(", 0) == 0 || of.rfind("Map(", 0) == 0)
			Fail(decl.File, field.Pos, "field '" + field.Name + "' of shape " + field.Kind +
				": nested containers are not supported — wrap the inner container in a named struct and use Of(ThatStruct)");
		ElementType element;
		if (options.OfIsString)
		{
			element.Kind = "Asset";
			element.AssetType = of;
			if (element.AssetType.empty())
				Fail(decl.File, field.Pos, "field '" + field.Name + "': Of(\"\") must name an asset type (e.g. Of(\"Material\"))");
			return element;
		}
		if (of.rfind("Asset(", 0) == 0 && of.back() == ')')
		{
			element.Kind = "Asset";
			element.AssetType = Unquote(of.substr(6, of.size() - 7));
			if (element.AssetType.empty())
				Fail(decl.File, field.Pos, "field '" + field.Name + "': Of(Asset(\"\")) must name an asset type (e.g. Of(\"Material\"))");
			return element;
		}
		if (of == "Object" || of == "Enum" || of == "Asset" || of == "None")
			Fail(decl.File, field.Pos, "field '" + field.Name + "': Of(" + of + ") needs a named type — " +
				"use Of(MyStruct) / Of(MyEnum) / Of(\"AssetType\")");
		if (StructKinds().count(of))
		{
			element.Kind = of;
			return element;
		}
		if (enumsByName.count(of))
		{
			element.Kind = "Enum";
			element.Enum = of;
			return element;
		}
		const auto structIt = index.Structs.find(of);
		if (structIt != index.Structs.end())
		{
			element.Kind = "Object";
			element.Struct = structIt->second;
			return element;
		}
		Fail(decl.File, field.Pos, "field '" + field.Name + "': unknown container element type '" + of +
			"' (expected a leaf kind like Float/Vec3/String, a registered struct/enum name, or an asset type string like Of(\"Material\"))");
		return element;
	}

	// FieldMetadata 的生成文本(普通字段与容器字段共用一份,避免两处漂移)。
	void EmitFieldMetadata(std::ostringstream& out, const FieldOptions& options)
	{
		out << "            FieldMetadata{ \"" << options.DisplayName << "\", \"" << options.Group << "\", "
			<< OptionalText(options.Min) << ", " << OptionalText(options.Max) << ", "
			<< (options.ReadOnly ? "true" : "false") << ", " << (options.Transient ? "true" : "false") << ", "
			<< "\"" << options.Doc << "\", " << (options.IsColor ? "true" : "false") << ", "
			<< "\"" << options.AssetType << "\", { ";
		for (size_t i = 0; i < options.Choices.size(); ++i)
			out << (i ? ", " : "") << "\"" << options.Choices[i] << "\"";
		out << " }, \"" << options.Unit << "\", " << OptionalText(options.Step) << " },\n";
	}

	// CPPT-6:容器字段的 Get/Set(整个容器 <-> ValueList / ValueMap)。
	// Set 对"未设(monostate)/类型不符"的元素回落到类型零值 —— 场景里 `~` 的行、手改坏值
	// 都不会在 Play 应用时抛 bad_variant_access。元素是命名 struct 时用 Read/WriteStructValue
	// 递归(值 = 字段名 → Value 的 ValueMap)。
	void EmitContainerAccessors(std::ostringstream& out, const FieldDecl& field, const std::string& qualified,
		const FieldOptions& options, const StructDecl& decl, const QualifiedIndex& index,
		const std::map<std::string, EnumDecl>& enumsByName)
	{
		const ElementType element = ResolveElementType(field, options, decl, index, enumsByName);
		const bool mapShape = field.Kind == "Map";
		const std::string packName = mapShape ? "PackMap" : "PackSequence";
		const std::string unpackName = mapShape ? "UnpackMap" : "UnpackSequence";

		std::string packParameter;
		std::string packBody;
		std::string unpackReturn;
		std::string unpackBody;
		if (element.Kind == "Object")
		{
			const std::string& nested = element.Struct;   // 已由 ResolveElementType 解析成限定名
			packParameter = "const " + nested + "&";
			packBody = "return ReadStructValue(WeSchemaOf_" + ShortName(nested) + "(), &item);";
			unpackReturn = nested;
			unpackBody = "                " + nested + " out {};\n"
				"                WriteStructValue(WeSchemaOf_" + ShortName(nested) + "(), &out, item);\n"
				"                return out;";
		}
		else if (element.Kind == "Enum")
		{
			const auto enumIt = enumsByName.find(element.Enum);
			const EnumInfo info = UnderlyingInfo(enumIt->second.Underlying);
			const std::string signedType = info.IsSigned ? "int64_t" : "uint64_t";
			const std::string enumType = index.Enum(element.Enum, decl.File, field.Pos);
			packParameter = "const " + enumType + "&";
			packBody = "return Value(static_cast<" + signedType + ">(item));";
			unpackReturn = enumType;
			unpackBody = "                if (const " + signedType + "* raw = std::get_if<" + signedType + ">(&item))\n"
				"                    return static_cast<" + enumType + ">(*raw);\n"
				"                return static_cast<" + enumType + ">(0);";
		}
		else if (element.Kind == "Asset")
		{
			packParameter = "const auto&";
			packBody = "return Value(AssetOps<std::remove_cv_t<std::remove_reference_t<decltype(item)>>>::GetPath(item));";
			unpackReturn = "Element";
			unpackBody = "                Element element {};\n"
				"                if (const std::string* path = std::get_if<std::string>(&item))\n"
				"                    AssetOps<Element>::SetPath(element, *path);\n"
				"                return element;";
		}
		else
		{
			const std::string& cpp = CppType(element.Kind);
			packParameter = "const " + cpp + "&";
			packBody = "return Value(item);";
			unpackReturn = cpp;
			unpackBody = "                const " + cpp + "* typed = std::get_if<" + cpp + ">(&item);\n"
				"                return typed ? *typed : " + cpp + " {};";
		}

		out << "    static Value Get_" << field.Name << "(const void* instance)\n";
		out << "    {\n        const " << qualified << "* self = static_cast<const " << qualified << "*>(instance);\n";
		out << "        return " << packName << "(self->" << field.Name << ",\n";
		out << "            [](" << packParameter << " item) { " << packBody << " });\n    }\n";
		out << "    static void Set_" << field.Name << "(void* instance, const Value& value)\n";
		out << "    {\n        " << qualified << "* self = static_cast<" << qualified << "*>(instance);\n";
		if (element.Kind == "Asset")
			out << "        using Element = std::remove_cv_t<std::remove_reference_t<decltype(self->" << field.Name
				<< ")>>::" << (mapShape ? "mapped_type" : "value_type") << ";\n";
		out << "        " << unpackName << "(value, &self->" << field.Name << ",\n";
		out << "            [](const Value& item) -> " << unpackReturn << "\n            {\n"
			<< unpackBody << "\n            });\n    }\n";
		if (element.Kind == "Enum")
			out << "    static const EnumSchema* GetEnum_" << field.Name << "()\n    {\n        return &WeEnumSchemaOf_"
				<< ShortName(index.Enum(element.Enum, decl.File, field.Pos)) << "();\n    }\n";
		if (element.Kind == "Object")
			out << "    static const TypeSchema* GetElementNested_" << field.Name << "()\n    {\n        return &WeSchemaOf_"
				<< ShortName(element.Struct) << "();\n    }\n";
	}

	// CPPT-6:容器字段的 FieldSchema(Collection/ElementKind/KeyKind/ElementTypeName/GetElementNested)。
	// K 恒为 Object(与脚本属性模型一致):容器不是叶子,元素/键类型在形状描述里。
	void EmitContainerFieldSchema(std::ostringstream& out, const FieldDecl& field, const FieldOptions& options,
		const StructDecl& decl, const QualifiedIndex& index, const std::map<std::string, EnumDecl>& enumsByName)
	{
		const ElementType element = ResolveElementType(field, options, decl, index, enumsByName);
		const uint64_t fieldId = options.Id.value_or(Fnv1a64(decl.Module + "::" + decl.Type + "." + field.Name));
		std::ostringstream hex;
		hex << "0x" << std::uppercase << std::hex << fieldId;

		out << "    static const FieldSchema& Field_" << field.Name << "()\n";
		out << "    {\n        static const FieldSchema schema = {\n";
		out << "            FieldId{ " << hex.str() << "ull },\n";
		out << "            \"" << field.Name << "\",\n";
		out << "            Kind::Object,\n";
		out << "            &Get_" << field.Name << ",\n";
		out << "            &Set_" << field.Name << ",\n";
		out << "            nullptr,\n            nullptr,\n            nullptr,\n";
		if (element.Kind == "Enum")
			out << "            &GetEnum_" << field.Name << ",\n";
		else
			out << "            nullptr,\n";
		if (element.Kind == "Asset")
			out << "            \"" << element.AssetType << "\",\n";
		else
			out << "            nullptr,\n";
		EmitFieldMetadata(out, options);
		out << "            Value(),\n";
		out << "            CollectionKind::" << field.Kind << ",\n";
		out << "            Kind::" << element.Kind << ",\n";
		out << "            " << (field.Kind == "Map" ? "Kind::String" : "Kind::None") << ",\n";
		if (element.Kind == "Object")
			// 元素类型名与 TypeSchema::Id.Name 同形(Module::Type)—— ComponentPropertyModel 用它
			// 填 PropertyNode::TypeName,与 GetElementNested() 的 Id.Name 保持一致。
			out << "            \"" << decl.Module << "::" << ShortName(element.Struct) << "\",\n";
		else if (element.Kind == "Enum")
			out << "            \"" << element.Enum << "\",\n";
		else
			out << "            nullptr,\n";
		if (element.Kind == "Object")
			out << "            &GetElementNested_" << field.Name << ",\n";
		else
			out << "            nullptr,\n";
		out << "        };\n        return schema;\n    }\n";
	}

	void EmitStructAccessor(std::ostringstream& out, const StructDecl& decl, const std::string& qualified,
		const QualifiedIndex& index, const std::map<std::string, EnumDecl>& enumsByName)
	{
		out << "template <>\nstruct GeneratedAccess<" << qualified << ">\n{\n";
		for (const FieldDecl& field : decl.Fields)
		{
			const FieldOptions options = ParseOptions(field, decl.File);
			if (field.Kind == "Array" || field.Kind == "Map")
			{
				EmitContainerAccessors(out, field, qualified, options, decl, index, enumsByName);
			}
			else if (field.Kind == "Enum")
			{
				const auto enumIt = enumsByName.find(options.Of.value());
				const EnumInfo info = UnderlyingInfo(enumIt->second.Underlying);
				const std::string signedType = info.IsSigned ? "int64_t" : "uint64_t";
				const std::string enumType = index.Enum(options.Of.value(), decl.File, field.Pos);
				out << "    static Value Get_" << field.Name << "(const void* instance)\n";
				out << "    {\n        const " << qualified << "* self = static_cast<const " << qualified << "*>(instance);\n";
				out << "        return Value(static_cast<" << signedType << ">(self->" << field.Name << "));\n    }\n";
				out << "    static void Set_" << field.Name << "(void* instance, const Value& value)\n";
				out << "    {\n        " << qualified << "* self = static_cast<" << qualified << "*>(instance);\n";
				out << "        self->" << field.Name << " = static_cast<" << enumType << ">(std::get<" << signedType << ">(value));\n    }\n";
			out << "    static const EnumSchema* GetEnum_" << field.Name << "()\n";
			out << "    {\n        return &WeEnumSchemaOf_" << ShortName(enumType) << "();\n    }\n";
			}
			else if (field.Kind == "Object")
			{
				const FieldOptions options = ParseOptions(field, decl.File);
				const std::string nested = index.Struct(options.Of.value(), decl.File, field.Pos);
				out << "    static void* GetPtr_" << field.Name << "(void* instance)\n";
				out << "    {\n        return &(static_cast<" << qualified << "*>(instance)->" << field.Name << ");\n    }\n";
				out << "    static const void* GetPtrConst_" << field.Name << "(const void* instance)\n";
				out << "    {\n        return &(static_cast<const " << qualified << "*>(instance)->" << field.Name << ");\n    }\n";
			out << "    static const TypeSchema* GetNested_" << field.Name << "()\n";
			out << "    {\n        return &WeSchemaOf_" << ShortName(nested) << "();\n    }\n";
			}
			else if (field.Kind == "Text")
			{
				// 有界文本:边界字符串 ↔ InlineString<N>(TextOps)。与 Name 同形,但**允许 Default(...)**
				// (Text 是真正的文本内容,给一个初值是有意义的;Name 是标识符,默认值没意义)。
				out << "    static Value Get_" << field.Name << "(const void* instance)\n";
				out << "    {\n        const " << qualified << "* self = static_cast<const " << qualified << "*>(instance);\n";
				out << "        return Value(TextOps<std::remove_reference_t<decltype(self->" << field.Name << ")>>::GetText(self->" << field.Name << "));\n    }\n";
				out << "    static void Set_" << field.Name << "(void* instance, const Value& value)\n";
				out << "    {\n        " << qualified << "* self = static_cast<" << qualified << "*>(instance);\n";
				out << "        TextOps<std::remove_reference_t<decltype(self->" << field.Name << ")>>::SetText(self->" << field.Name << ", std::get<std::string>(value));\n    }\n";
			}
			else if (field.Kind == "Name")
			{
				// 名字字段:边界字符串 ↔ 驻留 NameId(NameOps)。与 Asset 同形,名字不是身份 ⇒ 无身份通道。
				out << "    static Value Get_" << field.Name << "(const void* instance)\n";
				out << "    {\n        const " << qualified << "* self = static_cast<const " << qualified << "*>(instance);\n";
				out << "        return Value(NameOps<std::remove_reference_t<decltype(self->" << field.Name << ")>>::GetName(self->" << field.Name << "));\n    }\n";
				out << "    static void Set_" << field.Name << "(void* instance, const Value& value)\n";
				out << "    {\n        " << qualified << "* self = static_cast<" << qualified << "*>(instance);\n";
				out << "        NameOps<std::remove_reference_t<decltype(self->" << field.Name << ")>>::SetName(self->" << field.Name << ", std::get<std::string>(value));\n    }\n";
			}
			else if (field.Kind == "Asset")
			{
				out << "    static Value Get_" << field.Name << "(const void* instance)\n";
				out << "    {\n        const " << qualified << "* self = static_cast<const " << qualified << "*>(instance);\n";
				out << "        return Value(AssetOps<std::remove_reference_t<decltype(self->" << field.Name << ")>>::GetPath(self->" << field.Name << "));\n    }\n";
				out << "    static void Set_" << field.Name << "(void* instance, const Value& value)\n";
				out << "    {\n        " << qualified << "* self = static_cast<" << qualified << "*>(instance);\n";
				out << "        AssetOps<std::remove_reference_t<decltype(self->" << field.Name << ")>>::SetPath(self->" << field.Name << ", std::get<std::string>(value));\n    }\n";
				// 资产稳定身份通道(追加在尾部):序列化器据此把 AssetId 与路径一起持久化。
				out << "    static uint64_t GetAssetIdentity_" << field.Name << "(const void* instance)\n";
				out << "    {\n        const " << qualified << "* self = static_cast<const " << qualified << "*>(instance);\n";
				out << "        return AssetOps<std::remove_reference_t<decltype(self->" << field.Name << ")>>::GetIdentity(self->" << field.Name << ");\n    }\n";
				out << "    static void SetAssetIdentity_" << field.Name << "(void* instance, uint64_t identity)\n";
				out << "    {\n        " << qualified << "* self = static_cast<" << qualified << "*>(instance);\n";
				out << "        AssetOps<std::remove_reference_t<decltype(self->" << field.Name << ")>>::SetIdentity(self->" << field.Name << ", identity);\n    }\n";
			}
			else
			{
				const std::string& cpp = CppType(field.Kind);
				out << "    static Value Get_" << field.Name << "(const void* instance)\n";
				out << "    {\n        const " << qualified << "* self = static_cast<const " << qualified << "*>(instance);\n";
				if (options.Entity32)
					out << "        return Value(static_cast<uint64_t>(static_cast<uint32_t>(self->"
						<< field.Name << ")));\n    }\n";
				else
					out << "        return Value(self->" << field.Name << ");\n    }\n";
				out << "    static void Set_" << field.Name << "(void* instance, const Value& value)\n";
				out << "    {\n        " << qualified << "* self = static_cast<" << qualified << "*>(instance);\n";
				if (options.Entity32)
				{
					out << "        const uint64_t raw = std::get<uint64_t>(value);\n";
					out << "        self->" << field.Name << " = raw == static_cast<uint64_t>(entt::null)\n";
					out << "            ? entt::null : static_cast<entt::entity>(static_cast<uint32_t>(raw));\n    }\n";
				}
				else
					out << "        self->" << field.Name << " = std::get<" << cpp << ">(value);\n    }\n";
			}
		}
		for (const FieldDecl& field : decl.Fields)
		{
			const FieldOptions options = ParseOptions(field, decl.File);
			if (field.Kind == "Array" || field.Kind == "Map")
			{
				EmitContainerFieldSchema(out, field, options, decl, index, enumsByName);
				continue;
			}
			const uint64_t fieldId = options.Id.value_or(Fnv1a64(decl.Module + "::" + decl.Type + "." + field.Name));
			std::ostringstream hex;
			hex << "0x" << std::uppercase << std::hex << fieldId;

			out << "    static const FieldSchema& Field_" << field.Name << "()\n";
			out << "    {\n        static const FieldSchema schema = {\n";
			out << "            FieldId{ " << hex.str() << "ull },\n";
			out << "            \"" << field.Name << "\",\n";
			out << "            Kind::" << field.Kind << ",\n";
			if (field.Kind == "Object")
			{
				out << "            nullptr,\n            nullptr,\n";
				out << "            &GetPtr_" << field.Name << ",\n";
				out << "            &GetPtrConst_" << field.Name << ",\n";
				out << "            &GetNested_" << field.Name << ",\n";
				out << "            nullptr,\n            nullptr,\n";
			}
			else if (field.Kind == "Enum")
			{
				out << "            &Get_" << field.Name << ",\n";
				out << "            &Set_" << field.Name << ",\n";
				out << "            nullptr,\n            nullptr,\n            nullptr,\n";
				out << "            &GetEnum_" << field.Name << ",\n            nullptr,\n";
			}
			else if (field.Kind == "Text")
			{
				out << "            &Get_" << field.Name << ",\n";
				out << "            &Set_" << field.Name << ",\n";
				out << "            nullptr,\n            nullptr,\n            nullptr,\n            nullptr,\n";
				out << "            nullptr,\n";
			}
			else if (field.Kind == "Name")
			{
				out << "            &Get_" << field.Name << ",\n";
				out << "            &Set_" << field.Name << ",\n";
				out << "            nullptr,\n            nullptr,\n            nullptr,\n            nullptr,\n";
				out << "            nullptr,\n";
			}
			else if (field.Kind == "Asset")
			{
				const std::string assetName = options.Of.value_or("");
				out << "            &Get_" << field.Name << ",\n";
				out << "            &Set_" << field.Name << ",\n";
				out << "            nullptr,\n            nullptr,\n            nullptr,\n            nullptr,\n";
				out << "            \"" << assetName << "\",\n";
			}
			else
			{
				out << "            &Get_" << field.Name << ",\n";
				out << "            &Set_" << field.Name << ",\n";
				out << "            nullptr,\n            nullptr,\n            nullptr,\n            nullptr,\n            nullptr,\n";
			}
			EmitFieldMetadata(out, options);
			std::string def;
			if (field.Kind == "Enum")
			{
				const auto enumIt = enumsByName.find(options.Of.value());
				const EnumInfo info = UnderlyingInfo(enumIt->second.Underlying);
				const std::string signedType = info.IsSigned ? "int64_t" : "uint64_t";
				def = options.DefaultExpr.has_value()
					? "Value(static_cast<" + signedType + ">(" + options.DefaultExpr.value() + "))"
					: "Value(static_cast<" + signedType + ">(0))";
			}
			else if (field.Kind == "Text")
			{
				// 显式包 std::string:裸 `Value("x")` 在 variant 里会被解析成 bool(指针→bool 是
				// 标准转换,优先于到 std::string 的用户定义转换)—— 那是静默错误。
				def = options.DefaultExpr.has_value()
					? "Value(std::string(" + options.DefaultExpr.value() + "))"
					: "Value(std::string())";
			}
			else if (field.Kind == "Object" || field.Kind == "Asset" || field.Kind == "Name")
			{
				if (options.DefaultExpr.has_value())
					Fail(decl.File, field.Pos, "Default is not supported on Object/Asset/Name fields");
				def = "Value()";
			}
			else
			{
				if (options.Entity32)
					def = options.DefaultExpr.has_value()
						? "Value(" + options.DefaultExpr.value() + ")"
						: "Value(static_cast<uint64_t>(static_cast<uint32_t>(entt::null)))";
				else
					def = options.DefaultExpr.has_value() ? "Value(" + options.DefaultExpr.value() + ")" : DefaultValue(field.Kind);
			}
			out << "            " << def << ",\n";
			// 容器尾部槽位:加上追加在 FieldSchema 尾部的两个身份函数指针(仅资产叶字段非空)。
			if (field.Kind == "Asset")
			{
				out << "            CollectionKind::None,\n";
				out << "            Kind::None,\n";
				out << "            Kind::String,\n";
				out << "            nullptr,\n            nullptr,\n";
				out << "            &GetAssetIdentity_" << field.Name << ",\n";
				out << "            &SetAssetIdentity_" << field.Name << ",\n";
			}
			out << "        };\n        return schema;\n    }\n";
		}
		if (decl.Category == "Component")
		{
			out << "    static const StorageBinding& StorageBindingOf()\n";
			out << "    {\n        static const StorageBinding binding = MakeComponentStorage<" << qualified << ">();\n";
			out << "        return binding;\n    }\n";
		}
		out << "    static const TypeSchema& WeSchema()\n";
		out << "    {\n        static const TypeSchema schema = {\n";
		out << "            TypeId{ \"" << decl.Module << "::" << decl.Type << "\" },\n";
		out << "            \"" << decl.Type << "\",\n";
		out << "            WE_SCHEMA_ABI_VERSION,\n";
		out << "            sizeof(" << qualified << "),\n";
		out << "            TypeCategory::" << decl.Category << ",\n";
		out << "            {\n";
		for (const FieldDecl& field : decl.Fields)
			out << "                Field_" << field.Name << "(),\n";
		out << "            },\n";
		if (decl.Category == "Component")
			out << "            &StorageBindingOf(),\n";
		else
			out << "            nullptr,\n";
		// 类型级描述元数据(TypeSchema::CategoryPath / TypeSchema::Doc;空字符串 = 未填)。
		// 见 Schema.h 里 TypeSchema 末尾的字段说明:位置固定在最后(CategoryPath/Doc/Core)。
		out << "            \"" << decl.MetaCategory << "\",\n";
		out << "            \"" << decl.MetaDoc << "\",\n";
		out << "            " << (decl.MetaCore ? "true" : "false") << ",\n";
		out << "        };\n        return schema;\n    }\n";
		out << "};\n\n";
	}

	void EmitEnumAccessor(std::ostringstream& out, const EnumDecl& decl, const std::string& qualified)
	{
		const EnumInfo info = UnderlyingInfo(decl.Underlying);
		out << "template <>\nstruct GeneratedEnum<" << qualified << ">\n{\n";
		out << "    static const EnumSchema& WeEnumSchema()\n";
		out << "    {\n        static const EnumSchema schema = {\n";
		out << "            \"" << decl.Type << "\",\n";
		out << "            " << (info.IsSigned ? "true" : "false") << ",\n";
		out << "            " << static_cast<int>(info.Size) << ",\n";
		out << "            {\n";
		for (const auto& [value, pos] : decl.Values)
			out << "                { \"" << value << "\", static_cast<int64_t>(" << qualified << "::" << value << ") },\n";
		out << "            },\n";
		out << "        };\n        return schema;\n    }\n";
		out << "};\n\n";
	}

	void EmitModule(const std::string& moduleName, const std::vector<StructDecl>& structs,
		const std::vector<EnumDecl>& enums, const std::vector<std::pair<std::string, std::string>>& manifestEntries,
		const std::vector<std::string>& regIncludes, std::string& header, std::string& source)
	{
		{
			std::ostringstream out;
			out << "#pragma once\n\n";
			out << "// Generated by WorldEngine schema-compiler. Do not edit.\n";
			out << "#include \"World/Schema/SchemaRegistry.h\"\n\n";
			out << "namespace World::Schema\n{\n";
			out << "\tbool Register" << moduleName << "SchemaModule(SchemaRegistry& registry);\n";
			out << "\tvoid Unregister" << moduleName << "SchemaModule(SchemaRegistry& registry);\n";
			out << "}\n";
			header = out.str();
		}

		QualifiedIndex index;
		for (const auto& [kind, name] : manifestEntries)
			index.Add(kind, name);
		std::map<std::string, StructDecl> structsByName;
		for (const StructDecl& decl : structs)
			structsByName[decl.Type] = decl;
		std::map<std::string, EnumDecl> enumsByName;
		for (const EnumDecl& decl : enums)
			enumsByName[decl.Type] = decl;

		std::ostringstream out;
		out << "// Generated by WorldEngine schema-compiler. Do not edit.\n";
		out << "#include \"World/Schema/Schema.h\"\n";
		out << "#include \"World/Schema/SchemaRegistry.h\"\n";
		// MakeComponentStorage 的声明在这里;缺了它生成物自己编不过。
		out << "#include \"World/Schema/ComponentSchemaBridge.h\"\n";
		for (const std::string& include : regIncludes)
			out << "#include \"" << include << "\"\n";
		out << "\nnamespace World::Schema\n{\n";

		// 前置声明自由函数:访问器之间可按任意顺序互引(嵌套 Object/Enum)。
		for (const auto& [kind, qualified] : manifestEntries)
		{
			if (kind == "struct")
				out << "const TypeSchema& WeSchemaOf_" << ShortName(qualified) << "();\n";
			else
				out << "const EnumSchema& WeEnumSchemaOf_" << ShortName(qualified) << "();\n";
		}
		out << "\n";

		out << "\tnamespace\n\t{\n";
		out << "\t\tconst ModuleId kModule { \"" << moduleName << "\", 1 };\n";
		out << "\t}\n\n";

		for (const auto& [kind, qualified] : manifestEntries)
		{
			if (kind == "struct")
				EmitStructAccessor(out, structsByName.at(ShortName(qualified)), qualified, index, enumsByName);
			else
				EmitEnumAccessor(out, enumsByName.at(ShortName(qualified)), qualified);
		}

		// 特化全部完整后,自由函数再映射到特化成员(不依赖声明顺序)。
		for (const auto& [kind, qualified] : manifestEntries)
		{
			if (kind == "struct")
				out << "const TypeSchema& WeSchemaOf_" << ShortName(qualified) << "() { return GeneratedAccess<" << qualified << ">::WeSchema(); }\n";
			else
				out << "const EnumSchema& WeEnumSchemaOf_" << ShortName(qualified) << "() { return GeneratedEnum<" << qualified << ">::WeEnumSchema(); }\n";
		}
		out << "\n";

		out << "\tbool Register" << moduleName << "SchemaModule(SchemaRegistry& registry)\n\t{\n";
		out << "\t\tbool ok = true;\n";
		for (const auto& [kind, qualified] : manifestEntries)
			if (kind == "enum")
				out << "\t\tif (registry.RegisterEnum(kModule, WeEnumSchemaOf_" << ShortName(qualified) << "()) != SchemaRegistry::Status::Ok) ok = false;\n";
		out << "\t\tconst std::vector<TypeSchema> schemas = {\n";
		for (const auto& [kind, qualified] : manifestEntries)
			if (kind == "struct")
				out << "\t\t\tWeSchemaOf_" << ShortName(qualified) << "(),\n";
		out << "\t\t};\n";
		out << "\t\tif (registry.RegisterModule(kModule, schemas) != SchemaRegistry::Status::Ok) ok = false;\n";
		out << "\t\tif (!ok)\n\t\t{\n\t\t\tregistry.UnregisterModule(kModule);\n\t\t\treturn false;\n\t\t}\n";
		out << "\t\treturn true;\n\t}\n\n";
		out << "\tvoid Unregister" << moduleName << "SchemaModule(SchemaRegistry& registry)\n\t{\n";
		out << "\t\tregistry.UnregisterModule(kModule);\n\t}\n";
		out << "}\n";
		source = out.str();
	}

	std::string ReadFile(const std::string& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
			throw std::runtime_error("cannot open input file: " + path);
		std::ostringstream buffer;
		buffer << stream.rdbuf();
		if (stream.bad())
			throw std::runtime_error("failed reading file: " + path);
		return buffer.str();
	}

	std::string ReadFileIfExists(const std::string& path, bool* exists)
	{
		std::ifstream stream(path, std::ios::binary);
		*exists = static_cast<bool>(stream);
		if (!*exists)
			return {};
		std::ostringstream buffer;
		buffer << stream.rdbuf();
		return buffer.str();
	}

	void WriteFile(const std::string& path, const std::string& content)
	{
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		if (!stream)
			throw std::runtime_error("cannot open output file: " + path);
		stream << content;
		if (!stream)
			throw std::runtime_error("failed writing file: " + path);
	}

	struct Cli
	{
		std::vector<std::string> Inputs;
		std::string Module;
		std::string Output;
		std::string Manifest;
		std::vector<std::string> RegIncludes;
		bool Check = false;
	};

	Cli ParseCli(int argc, char** argv)
	{
		Cli cli;
		for (int i = 1; i < argc; ++i)
		{
			const std::string arg = argv[i];
			auto nextValue = [&](const char* flag) -> std::string
			{
				if (i + 1 >= argc)
					throw std::runtime_error(std::string("missing value for ") + flag);
				return argv[++i];
			};
			if (arg == "--input") cli.Inputs.push_back(nextValue("--input"));
			else if (arg == "--module") cli.Module = nextValue("--module");
			else if (arg == "--output") cli.Output = nextValue("--output");
			else if (arg == "--manifest") cli.Manifest = nextValue("--manifest");
			else if (arg == "--reg-include") cli.RegIncludes.push_back(nextValue("--reg-include"));
			else if (arg == "--check") cli.Check = true;
			else throw std::runtime_error("unknown argument: " + arg);
		}
		if (cli.Inputs.empty()) throw std::runtime_error("--input is required");
		if (cli.Module.empty()) throw std::runtime_error("--module is required");
		if (cli.Output.empty()) throw std::runtime_error("--output is required");
		if (cli.Manifest.empty()) throw std::runtime_error("--manifest is required");
		if (cli.RegIncludes.empty()) throw std::runtime_error("--reg-include is required at least once");
		return cli;
	}

	std::string PathJoin(const std::string& left, const std::string& right)
	{
		if (left.empty()) return right;
		if (left.back() == '/' || left.back() == '\\')
			return left + right;
		return left + "/" + right;
	}
}

int main(int argc, char** argv)
{
	try
	{
		const Cli cli = ParseCli(argc, argv);

		std::vector<StructDecl> structs;
		std::vector<EnumDecl> enums;
		std::map<std::string, std::string> structFile;
		std::map<std::string, std::string> enumFile;

		for (const std::string& input : cli.Inputs)
		{
			const std::string source = ReadFile(input);
			const std::vector<Token> tokens = Tokenizer(source, input).Run();
			Parser parser(tokens, input);
			std::vector<StructDecl> fileStructs;
			std::vector<EnumDecl> fileEnums;
			parser.Run(fileStructs, fileEnums);
			for (StructDecl& decl : fileStructs)
			{
				if (decl.Module != cli.Module)
					Fail(input, decl.Pos, "struct module '" + decl.Module + "' does not match --module " + cli.Module);
				if (structFile.count(decl.Type))
					Fail(input, decl.Pos, "duplicate struct '" + decl.Type + "' (also in " + structFile[decl.Type] + ")");
				structFile[decl.Type] = input;
				structs.push_back(std::move(decl));
			}
			for (EnumDecl& decl : fileEnums)
			{
				if (decl.Module != cli.Module)
					Fail(input, decl.Pos, "enum module '" + decl.Module + "' does not match --module " + cli.Module);
				if (enumFile.count(decl.Type))
					Fail(input, decl.Pos, "duplicate enum '" + decl.Type + "' (also in " + enumFile[decl.Type] + ")");
				enumFile[decl.Type] = input;
				enums.push_back(std::move(decl));
			}
		}

		std::vector<std::pair<std::string, std::string>> manifestEntries;
		{
			std::istringstream manifest(ReadFile(cli.Manifest));
			std::string line;
			int lineNumber = 0;
			while (std::getline(manifest, line))
			{
				++lineNumber;
				const size_t comment = line.find('#');
				if (comment != std::string::npos)
					line = line.substr(0, comment);
				std::istringstream words(line);
				std::string kind, name;
				if (!(words >> kind))
					continue;
				if (!(words >> name))
					Fail(cli.Manifest, SourcePos{ lineNumber, 1 }, "manifest entry needs a qualified type name");
				if (kind != "struct" && kind != "enum")
					Fail(cli.Manifest, SourcePos{ lineNumber, 1 }, "manifest kind must be 'struct' or 'enum'");
				const std::string shortName = ShortName(name);
				if (kind == "struct" && !structFile.count(shortName))
					Fail(cli.Manifest, SourcePos{ lineNumber, 1 }, "manifest lists unknown struct '" + shortName + "'");
				if (kind == "enum" && !enumFile.count(shortName))
					Fail(cli.Manifest, SourcePos{ lineNumber, 1 }, "manifest lists unknown enum '" + shortName + "'");
				manifestEntries.push_back({ kind, name });
			}
			for (const StructDecl& decl : structs)
			{
				bool listed = false;
				for (const auto& [kind, name] : manifestEntries)
					if (kind == "struct" && ShortName(name) == decl.Type)
						listed = true;
				if (!listed)
					Fail(decl.File, decl.Pos, "struct '" + decl.Type + "' is declared but missing from the manifest");
			}
			for (const EnumDecl& decl : enums)
			{
				bool listed = false;
				for (const auto& [kind, name] : manifestEntries)
					if (kind == "enum" && ShortName(name) == decl.Type)
						listed = true;
				if (!listed)
					Fail(decl.File, decl.Pos, "enum '" + decl.Type + "' is declared but missing from the manifest");
			}
		}

		static const std::set<std::string> categories = { "Struct", "Component" };
		std::map<std::string, EnumDecl> enumsByName;
		for (const EnumDecl& decl : enums)
			enumsByName[decl.Type] = decl;
		QualifiedIndex manifestIndex;
		for (const auto& [kind, name] : manifestEntries)
			manifestIndex.Add(kind, name);
		for (const StructDecl& decl : structs)
		{
			if (!categories.count(decl.Category))
				Fail(decl.File, decl.Pos, "invalid category '" + decl.Category + "'");
			std::set<std::string> names;
			for (const FieldDecl& field : decl.Fields)
			{
				if (!names.insert(field.Name).second)
					Fail(decl.File, field.Pos, "duplicate field '" + field.Name + "'");
				// CPPT-6:Array/Map 是**形状**(不是 Kind);元素类型在 ElementTypeOf 里解析。
				const bool containerShape = field.Kind == "Array" || field.Kind == "Map";
				if (!containerShape && !StructKinds().count(field.Kind))
					Fail(decl.File, field.Pos, "unknown field kind '" + field.Kind + "'");
				const FieldOptions options = ParseOptions(field, decl.File);
				if (containerShape)
				{
					const ElementType element = ResolveElementType(field, options, decl, manifestIndex, enumsByName);
					if (options.Min.has_value() && !NumericKinds().count(element.Kind))
						Fail(decl.File, field.Pos, "Range is only valid on numeric element types ('" + field.Name + "')");
					if (!options.Unit.empty() && !NumericKinds().count(element.Kind) && element.Kind != "String")
						Fail(decl.File, field.Pos, "Unit is only valid on numeric or String element types ('" + field.Name + "')");
					if (options.Step.has_value() && !NumericKinds().count(element.Kind))
						Fail(decl.File, field.Pos, "Step is only valid on numeric element types ('" + field.Name + "')");
					if (options.Entity32)
						Fail(decl.File, field.Pos, "Entity32 is not supported on container fields ('" + field.Name + "')");
					if (options.DefaultExpr.has_value())
						Fail(decl.File, field.Pos, "Default is not supported on container fields ('" + field.Name +
							"'); the script member initializer is the default");
					if (options.IsColor)
						Fail(decl.File, field.Pos, "Color() is not supported on container fields ('" + field.Name + "')");
					if (!options.Choices.empty())
						Fail(decl.File, field.Pos, "Choices(...) is not supported on container fields ('" + field.Name + "')");
					// Name / Text 没有容器通道(它们在元素位置会落到 CppType 泛型支路,
					// 生成出编译不过的代码)。真正要拦的是 **element.Kind** —— 之前这里检查的是
					// field.Kind,而该分支里 field.Kind 恒为 Array/Map ⇒ 那是一段死校验。
					if (element.Kind == "Name" || element.Kind == "Text")
						Fail(decl.File, field.Pos, "field '" + field.Name + "': Of(" + element.Kind +
							") is not supported as a container element/key (no container field uses it; "
							"declare it as a plain field)");
				}
				else if (field.Kind == "Enum" || field.Kind == "Object" || field.Kind == "Asset")
				{
					if (!options.Of.has_value())
						Fail(decl.File, field.Pos, "field '" + field.Name + "' of kind " + field.Kind + " requires Of(...)");
				}
				else if (options.Of.has_value())
				{
					Fail(decl.File, field.Pos, "field '" + field.Name + "' must not specify Of(...)");
				}
				if (!containerShape && field.Kind == "Enum" && !enumsByName.count(options.Of.value()))
					Fail(decl.File, field.Pos, "field '" + field.Name + "' references unknown enum '" + options.Of.value() + "'");
				if (!containerShape && options.Min.has_value() && !NumericKinds().count(field.Kind))
					Fail(decl.File, field.Pos, "Range is only valid on numeric fields");
				if (!containerShape && !options.Unit.empty() && !NumericKinds().count(field.Kind) && field.Kind != "String")
					Fail(decl.File, field.Pos, "Unit is only valid on numeric or String fields");
				if (!containerShape && options.Step.has_value() && !NumericKinds().count(field.Kind))
					Fail(decl.File, field.Pos, "Step is only valid on numeric fields");
				if (!containerShape && options.Entity32 && field.Kind != "UInt64")
					Fail(decl.File, field.Pos, "Entity32 is only valid on UInt64 fields (entt::entity packed into 64-bit storage)");
			}
		}
		for (const EnumDecl& decl : enums)
		{
			if (!EnumUnderlyings().count(decl.Underlying))
				Fail(decl.File, decl.Pos, "invalid enum underlying kind '" + decl.Underlying + "'");
			std::set<std::string> values;
			for (const auto& [value, pos] : decl.Values)
				if (!values.insert(value).second)
					Fail(decl.File, pos, "duplicate enumerator '" + value + "'");
		}

		std::string header, source;
		EmitModule(cli.Module, structs, enums, manifestEntries, cli.RegIncludes, header, source);

		std::map<std::string, std::string> outputs;
		outputs[PathJoin(cli.Module, cli.Module + "SchemaRegistration.h")] = header;
		outputs[PathJoin(cli.Module, cli.Module + "SchemaRegistration.cpp")] = source;

		if (cli.Check)
		{
			bool ok = true;
			for (const auto& [relative, content] : outputs)
			{
				const std::string path = PathJoin(cli.Output, relative);
				bool exists = false;
				const std::string existing = ReadFileIfExists(path, &exists);
				if (!exists || existing != content)
				{
					std::cerr << "schema-compiler: drift detected in " << path << "\n";
					ok = false;
				}
			}
			if (!ok)
			{
				std::cerr << "schema-compiler: generated files are out of date; run without --check to regenerate.\n";
				return 1;
			}
			std::cout << "schema-compiler: all generated files are up to date.\n";
			return 0;
		}

		std::filesystem::create_directories(cli.Output);
		for (const auto& [relative, content] : outputs)
		{
			const std::string path = PathJoin(cli.Output, relative);
			const size_t slash = path.find_last_of("/\\");
			if (slash != std::string::npos)
				std::filesystem::create_directories(path.substr(0, slash));
			WriteFile(path, content);
		}
		std::cout << "schema-compiler: wrote " << outputs.size() << " files for module '" << cli.Module << "'.\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "schema-compiler: " << error.what() << "\n";
		return 1;
	}
}

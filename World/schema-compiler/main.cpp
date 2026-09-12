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
				if (!PeekIs(Token::Type::Identifier, "WE_FIELD"))
					Fail(m_File, Peek().Pos, "only WE_FIELD declarations are allowed inside a WE_SCHEMA_BODY block");
				decl.Fields.push_back(ParseField());
			}
			ExpectIdentifier("WE_SCHEMA_END");
			if (!AtEnd() && PeekIs(Token::Type::Punct, ";"))
				Next();
			return decl;
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
				field.Attrs.push_back(std::move(attr));
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

	std::string Unquote(const std::string& text)
	{
		if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
			return text.substr(1, text.size() - 2);
		return text;
	}

	const std::set<std::string>& StructKinds()
	{
		static const std::set<std::string> kinds = {
			"Bool", "Int8", "Int16", "Int32", "Int64", "UInt8", "UInt16", "UInt32", "UInt64",
			"Float", "Double", "Vec2", "Vec3", "Vec4", "IVec2", "IVec3", "IVec4",
			"UVec2", "UVec3", "UVec4", "Quat", "Mat3", "Mat4", "String", "Enum", "Asset", "Object"
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
		};
		return types.at(kind);
	}

	std::string DefaultValue(const std::string& kind)
	{
		if (kind == "Bool") return "Value(false)";
		if (kind == "Float" || kind == "Double") return "Value(0.0)";
		if (kind == "String") return "Value(std::string())";
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
		if (kind == "Enum" || kind == "Object" || kind == "Asset") return "Value()";
		return "Value(0)"; // 整数族
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
		std::optional<uint64_t> Id;
		std::optional<std::string> DefaultExpr;
		std::optional<std::string> Of;
	};

	FieldOptions ParseOptions(const FieldDecl& field, const std::string& file)
	{
		FieldOptions options;
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
				if (!attr.Args.empty()) Fail(file, attr.Pos, "attribute 'ReadOnly' takes no arguments");
				options.ReadOnly = true;
			}
			else if (attr.Name == "Transient")
			{
				if (!attr.Args.empty()) Fail(file, attr.Pos, "attribute 'Transient' takes no arguments");
				options.Transient = true;
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
				options.DefaultExpr = JoinTokens(attr.Args[0]);
			}
			else if (attr.Name == "Of")
			{
				if (attr.Args.size() != 1 || attr.Args[0].empty())
					Fail(file, attr.Pos, "attribute 'Of' expects a type name");
				const bool isString = attr.Args[0].size() == 1 && attr.Args[0][0].type == Token::Type::String;
				options.Of = isString ? Unquote(attr.Args[0][0].Text) : JoinCompact(attr.Args[0]);
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

	void EmitStructAccessor(std::ostringstream& out, const StructDecl& decl, const std::string& qualified,
		const QualifiedIndex& index, const std::map<std::string, EnumDecl>& enumsByName)
	{
		out << "template <>\nstruct GeneratedAccess<" << qualified << ">\n{\n";
		for (const FieldDecl& field : decl.Fields)
		{
			if (field.Kind == "Enum")
			{
				const FieldOptions options = ParseOptions(field, decl.File);
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
				out << "    {\n        return &GeneratedEnum<" << enumType << ">::WeEnumSchema();\n    }\n";
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
				out << "    {\n        return &GeneratedAccess<" << nested << ">::WeSchema();\n    }\n";
			}
			else if (field.Kind == "Asset")
			{
				out << "    static Value Get_" << field.Name << "(const void* instance)\n";
				out << "    {\n        const " << qualified << "* self = static_cast<const " << qualified << "*>(instance);\n";
				out << "        return Value(AssetOps<decltype(self->" << field.Name << ")>::GetPath(self->" << field.Name << "));\n    }\n";
				out << "    static void Set_" << field.Name << "(void* instance, const Value& value)\n";
				out << "    {\n        " << qualified << "* self = static_cast<" << qualified << "*>(instance);\n";
				out << "        AssetOps<decltype(self->" << field.Name << ")>::SetPath(self->" << field.Name << ", std::get<std::string>(value));\n    }\n";
			}
			else
			{
				const std::string& cpp = CppType(field.Kind);
				out << "    static Value Get_" << field.Name << "(const void* instance)\n";
				out << "    {\n        const " << qualified << "* self = static_cast<const " << qualified << "*>(instance);\n";
				out << "        return Value(self->" << field.Name << ");\n    }\n";
				out << "    static void Set_" << field.Name << "(void* instance, const Value& value)\n";
				out << "    {\n        " << qualified << "* self = static_cast<" << qualified << "*>(instance);\n";
				out << "        self->" << field.Name << " = std::get<" << cpp << ">(value);\n    }\n";
			}
		}
		for (const FieldDecl& field : decl.Fields)
		{
			const FieldOptions options = ParseOptions(field, decl.File);
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
			out << "            FieldMetadata{ \"" << options.DisplayName << "\", \"" << options.Group << "\", "
				<< OptionalText(options.Min) << ", " << OptionalText(options.Max) << ", "
				<< (options.ReadOnly ? "true" : "false") << ", " << (options.Transient ? "true" : "false") << " },\n";
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
			else if (field.Kind == "Object" || field.Kind == "Asset")
			{
				if (options.DefaultExpr.has_value())
					Fail(decl.File, field.Pos, "Default is not supported on Object/Asset fields");
				def = "Value()";
			}
			else
			{
				def = options.DefaultExpr.has_value() ? "Value(" + options.DefaultExpr.value() + ")" : DefaultValue(field.Kind);
			}
			out << "            " << def << ",\n";
			out << "        };\n        return schema;\n    }\n";
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
		out << "            nullptr,\n            nullptr,\n";
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
		for (const std::string& include : regIncludes)
			out << "#include \"" << include << "\"\n";
		out << "\nnamespace World::Schema\n{\n";
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

		out << "\tbool Register" << moduleName << "SchemaModule(SchemaRegistry& registry)\n\t{\n";
		out << "\t\tbool ok = true;\n";
		for (const auto& [kind, qualified] : manifestEntries)
			if (kind == "enum")
				out << "\t\tif (registry.RegisterEnum(kModule, GeneratedEnum<" << qualified << ">::WeEnumSchema()) != SchemaRegistry::Status::Ok) ok = false;\n";
		out << "\t\tconst std::vector<TypeSchema> schemas = {\n";
		for (const auto& [kind, qualified] : manifestEntries)
			if (kind == "struct")
				out << "\t\t\tGeneratedAccess<" << qualified << ">::WeSchema(),\n";
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

		static const std::set<std::string> categories = { "Struct", "Component", "Script" };
		std::map<std::string, EnumDecl> enumsByName;
		for (const EnumDecl& decl : enums)
			enumsByName[decl.Type] = decl;
		for (const StructDecl& decl : structs)
		{
			if (!categories.count(decl.Category))
				Fail(decl.File, decl.Pos, "invalid category '" + decl.Category + "'");
			std::set<std::string> names;
			for (const FieldDecl& field : decl.Fields)
			{
				if (!names.insert(field.Name).second)
					Fail(decl.File, field.Pos, "duplicate field '" + field.Name + "'");
				if (!StructKinds().count(field.Kind))
					Fail(decl.File, field.Pos, "unknown field kind '" + field.Kind + "'");
				const FieldOptions options = ParseOptions(field, decl.File);
				if (field.Kind == "Enum" || field.Kind == "Object" || field.Kind == "Asset")
				{
					if (!options.Of.has_value())
						Fail(decl.File, field.Pos, "field '" + field.Name + "' of kind " + field.Kind + " requires Of(...)");
				}
				else if (options.Of.has_value())
				{
					Fail(decl.File, field.Pos, "field '" + field.Name + "' must not specify Of(...)");
				}
				if (field.Kind == "Enum" && !enumsByName.count(options.Of.value()))
					Fail(decl.File, field.Pos, "field '" + field.Name + "' references unknown enum '" + options.Of.value() + "'");
				if (options.Min.has_value() && !NumericKinds().count(field.Kind))
					Fail(decl.File, field.Pos, "Range is only valid on numeric fields");
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

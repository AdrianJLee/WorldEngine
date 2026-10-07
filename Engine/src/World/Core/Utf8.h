#pragma once

#include <string>

// UTF-8 边界工具(引擎里**唯一**一份实现)。
//
// 背景(2026-10-07,M54):界面里"乱码"的一种真因不是字体缺字,而是**按字节**截断字符串后留下
// 一个悬空的 UTF-8 引导字节(如 E6/E4)。它会被解码器与**后面的字节**拼成一个幽灵码点
// (实测 U+6BAE / U+4BAE / U+FFFD),而字形表里没有这些码点 ⇒ 渲染成空白;更糟的是解码器
// 会把后续 1~2 个字节也当成分量吃掉(省略号"..."就这么消失的)。
//
// 易错写法(三处都犯过):先 pop_back(),再 while(续字节) pop_back() —— 它只删掉了续字节,
// **留下了引导字节**。正确做法见本文件。
namespace World::Utf8
{
	inline bool IsContinuation(char ch)
	{
		return (static_cast<unsigned char>(ch) & 0xC0u) == 0x80u;
	}

	// 删掉末尾**一个完整字符**;若末尾本来就是半截序列(上游按字节截断留下的),
	// 把这些字节一并删干净。返回删完是否非空。
	inline bool PopBack(std::string& text)
	{
		if (text.empty())
			return false;
		size_t i = text.size();
		while (i > 0 && IsContinuation(text[i - 1]))
			--i;
		if (i > 0)
			--i;                          // 引导字节
		text.resize(i);
		return !text.empty();
	}

	// 折到 ≤ maxBytes 字节且**不切碎字符**;返回可安全使用的字节数(可直接 substr(0, n))。
	inline size_t SafeByteCount(const std::string& text, size_t maxBytes)
	{
		if (text.size() <= maxBytes)
			return text.size();
		size_t cut = maxBytes;
		while (cut > 0 && IsContinuation(text[cut]))
			--cut;                        // 退到引导字节之前
		return cut;
	}

	// 整串是否合法 UTF-8(只做结构校验:引导字节 + 正确的续字节个数)。
	inline bool IsValid(const std::string& text)
	{
		size_t i = 0;
		while (i < text.size())
		{
			const unsigned char c = static_cast<unsigned char>(text[i]);
			size_t need = 0;
			if (c < 0x80) need = 0;
			else if ((c >> 5) == 0x6) need = 1;
			else if ((c >> 4) == 0xE) need = 2;
			else if ((c >> 3) == 0x1E) need = 3;
			else return false;
			if (i + need >= text.size() + 0 && need > 0 && i + need > text.size() - 1)
				return false;
			for (size_t k = 1; k <= need; ++k)
				if (i + k >= text.size() || !IsContinuation(text[i + k]))
					return false;
			i += need + 1;
		}
		return true;
	}

	inline std::string TrimToBytes(const std::string& text, size_t maxBytes)
	{
		return text.substr(0, SafeByteCount(text, maxBytes));
	}
}

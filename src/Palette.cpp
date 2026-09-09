#include <Palette.h>

#include <cstdio>
#include <vector>

namespace
{
	bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& bytes)
	{
		FILE* f = std::fopen(path.c_str(), "rb");
		if (!f) return false;
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		if (sz < 0) { std::fclose(f); return false; }
		fseek(f, 0, SEEK_SET);
		bytes.resize((size_t)sz);
		size_t got = sz ? std::fread(bytes.data(), 1, (size_t)sz, f) : 0;
		std::fclose(f);
		return got == bytes.size();
	}

	// 文本解析：收集 0..255 的整数，够 768 个即成功。注释: ; # //，方括号、分隔符一律跳过。
	bool TryParseText(const unsigned char* d, size_t n, std::uint8_t* out)
	{
		std::size_t write = 0;
		bool inComment = false;
		bool wasSlash = false;
		std::size_t i = 0;
		while (i < n)
		{
			unsigned char c = d[i];
			// 注释：// 到行尾；# 或 ; 到行尾
			if (inComment)
			{
				if (c == '\n' || c == '\r') inComment = false;
				if (c == '\n') wasSlash = false;
				++i;
				continue;
			}
			if (wasSlash && c == '/') { inComment = true; wasSlash = false; ++i; continue; }
			if (c == '/') { wasSlash = true; ++i; continue; }
			wasSlash = false;
			if (c == '#' || c == ';') { inComment = true; ++i; continue; }
			if (c == '[' || c == ']') { ++i; continue; }

			if (c >= '0' && c <= '9')
			{
				int val = 0;
				size_t len = 0;
				while (i < n && d[i] >= '0' && d[i] <= '9') { val = val * 10 + (d[i] - '0'); ++i; ++len; }
				// 只接受 0..255 单通道值；多字节"/"拼接成的超范围 token 会被丢弃
				if (val >= 0 && val <= 255)
				{
					out[write++] = (std::uint8_t)val;
					if (write >= 768) return true;
				}
				continue;
			}
			++i;
		}
		return false;
	}
}

bool PaletteLoader::Load(const std::string& path, std::uint8_t* out768)
{
	std::vector<unsigned char> bytes;
	if (!ReadWholeFile(path, bytes))
		return false;

	// 1) 先按文本格式尝试；成功则用了 256 色
	if (TryParseText(bytes.data(), bytes.size(), out768))
		return true;

	// 2) 否则按二进制 768 字节 RGB
	if (bytes.size() >= 768)
	{
		for (size_t i = 0; i < 768; ++i)
			out768[i] = bytes[i];
		return true;
	}
	return false;
}
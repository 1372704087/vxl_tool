#include <PngWriter.h>

#include <cstring>

namespace
{
	// CRC32（PNG 规范，多项式 0xEDB88320）
	std::uint32_t Crc32(const std::uint8_t* data, size_t len)
	{
		static std::uint32_t table[256];
		static bool init = false;
		if (!init)
		{
			for (std::uint32_t i = 0; i < 256; ++i)
			{
				std::uint32_t c = i;
				for (int k = 0; k < 8; ++k)
					c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
				table[i] = c;
			}
			init = true;
		}
		std::uint32_t crc = 0xFFFFFFFFu;
		for (size_t i = 0; i < len; ++i)
			crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
		return crc ^ 0xFFFFFFFFu;
	}

	// Adler32（zlib 校验）
	std::uint32_t Adler32(const std::uint8_t* data, size_t len)
	{
		std::uint32_t a = 1, b = 0;
		for (size_t i = 0; i < len; ++i)
		{
			a = (a + data[i]) % 65521u;
			b = (b + a) % 65521u;
		}
		return (b << 16) | a;
	}

	void Push32(std::vector<std::uint8_t>& out, std::uint32_t v)
	{
		out.push_back((std::uint8_t)(v >> 24));
		out.push_back((std::uint8_t)((v >> 16) & 0xFF));
		out.push_back((std::uint8_t)((v >> 8) & 0xFF));
		out.push_back((std::uint8_t)(v & 0xFF));
	}

	// 写一个 PNG chunk：length + type + data + crc
	void WriteChunk(std::vector<std::uint8_t>& out, const char type[4],
		const std::uint8_t* data, size_t len)
	{
		Push32(out, (std::uint32_t)len);
		size_t typePos = out.size();
		for (int i = 0; i < 4; ++i)
			out.push_back((std::uint8_t)type[i]);
		if (data && len)
			out.insert(out.end(), data, data + len);
		std::uint32_t crc = Crc32(out.data() + typePos, 4 + len);
		Push32(out, crc);
	}

	// 将扫描线数据打包为 zlib stored 流
	void BuildZlibStored(const std::vector<std::uint8_t>& raw,
		std::vector<std::uint8_t>& out)
	{
		// zlib 头：CMF=0x78, FLG=0x01（无字典，最快）
		out.push_back(0x78);
		out.push_back(0x01);

		size_t pos = 0;
		while (pos < raw.size())
		{
			size_t remain = raw.size() - pos;
			size_t blockLen = remain > 65535 ? 65535 : remain;
			bool final = (pos + blockLen >= raw.size());

			// 块头：BFINAL(bit0) + BTYPE=00(stored)
			out.push_back(final ? 0x01 : 0x00);
			// LEN / NLEN（小端）
			out.push_back((std::uint8_t)(blockLen & 0xFF));
			out.push_back((std::uint8_t)((blockLen >> 8) & 0xFF));
			std::uint16_t nlen = (std::uint16_t)(~blockLen & 0xFFFF);
			out.push_back((std::uint8_t)(nlen & 0xFF));
			out.push_back((std::uint8_t)((nlen >> 8) & 0xFF));
			// 原始数据
			out.insert(out.end(), raw.begin() + pos, raw.begin() + pos + blockLen);
			pos += blockLen;
		}

		// Adler32（大端）
		Push32(out, Adler32(raw.data(), raw.size()));
	}
}

bool PngWriter::WriteRGB(int width, int height,
	const std::vector<std::uint8_t>& rgb,
	std::vector<std::uint8_t>& out_data)
{
	out_data.clear();
	if (width <= 0 || height <= 0)
		return false;
	if ((size_t)width * height * 3 > rgb.size())
		return false;

	// PNG 签名
	static const std::uint8_t sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
	out_data.insert(out_data.end(), sig, sig + 8);

	// IHDR
	{
		std::uint8_t ihdr[13];
		ihdr[0] = (std::uint8_t)(width >> 24);
		ihdr[1] = (std::uint8_t)(width >> 16);
		ihdr[2] = (std::uint8_t)(width >> 8);
		ihdr[3] = (std::uint8_t)width;
		ihdr[4] = (std::uint8_t)(height >> 24);
		ihdr[5] = (std::uint8_t)(height >> 16);
		ihdr[6] = (std::uint8_t)(height >> 8);
		ihdr[7] = (std::uint8_t)height;
		ihdr[8] = 8;   // bit depth
		ihdr[9] = 2;   // color type: truecolor RGB
		ihdr[10] = 0;  // compression
		ihdr[11] = 0;  // filter
		ihdr[12] = 0;  // interlace
		WriteChunk(out_data, "IHDR", ihdr, 13);
	}

	// 扫描线：每行 = filter 字节(0) + RGB 数据
	std::vector<std::uint8_t> raw;
	raw.reserve((size_t)height * (1 + (size_t)width * 3));
	for (int y = 0; y < height; ++y)
	{
		raw.push_back(0);  // filter: None
		const std::uint8_t* row = &rgb[(size_t)y * width * 3];
		raw.insert(raw.end(), row, row + (size_t)width * 3);
	}

	// IDAT（zlib stored）
	std::vector<std::uint8_t> zlib;
	BuildZlibStored(raw, zlib);
	WriteChunk(out_data, "IDAT", zlib.data(), zlib.size());

	// IEND
	WriteChunk(out_data, "IEND", nullptr, 0);

	return true;
}

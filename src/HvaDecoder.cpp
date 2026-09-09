#include <HvaDecoder.h>

#include <cstring>

namespace
{
	inline std::uint32_t rd32(const std::uint8_t* p)
	{
		return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) |
			((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
	}

	inline float rdf32(const std::uint8_t* p)
	{
		std::uint32_t bits = rd32(p);
		float f;
		std::memcpy(&f, &bits, 4);
		return f;
	}
}

bool HvaDecoder::Decode(const std::uint8_t* data, int size,
	int& out_frameCount, int& out_sectionCount,
	std::vector<HvaSectionInfo>& out_sections,
	std::vector<float>& out_transforms)
{
	out_frameCount = 0;
	out_sectionCount = 0;
	out_sections.clear();
	out_transforms.clear();

	if (!data || size < 24)
		return false;

	// ---- Header (24 字节) ----
	out_frameCount = (int)rd32(data + 16);
	out_sectionCount = (int)rd32(data + 20);

	if (out_frameCount <= 0 || out_frameCount > 65536)
		return false;
	if (out_sectionCount <= 0 || out_sectionCount > 512)
		return false;

	// ---- Section Names（sectionCount × 16）----
	int pos = 24;
	out_sections.reserve(out_sectionCount);
	for (int i = 0; i < out_sectionCount; ++i)
	{
		if (pos + 16 > size)
			return false;
		char name[17] = { 0 };
		std::memcpy(name, data + pos, 16);
		pos += 16;
		out_sections.push_back({ name });
	}

	// ---- Transform Matrices（frameCount × sectionCount × 48）----
	int matrixCount = out_frameCount * out_sectionCount;
	int needBytes = matrixCount * 48;
	if (pos + needBytes > size)
		return false;
	// [建议/C11] 此处先做了 needBytes 的 size 校验，恶意大 header 会在 reserve 前被拦截，安全。
	//   但值得注意的是最大合法 matrixCount = 65536×512 = 33M，needBytes≈1.6GB，
	//   out_transforms.reserve 需要 12×33M×4≈1.6GB 浮点内存，接近 32 位进程上限。
	//   若项目可能运行在 32 位/低内存设备，建议按实际文件 size 放宽上限或分块处理。

	out_transforms.reserve(matrixCount * 12);
	for (int i = 0; i < matrixCount; ++i)
	{
		for (int j = 0; j < 12; ++j)
			out_transforms.push_back(rdf32(data + pos + i * 48 + j * 4));
	}

	return true;
}

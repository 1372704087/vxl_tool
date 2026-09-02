#include <HvaEncoder.h>

#include <cstring>

namespace
{
	inline void wr32(std::vector<std::uint8_t>& out, std::uint32_t v)
	{
		out.push_back((std::uint8_t)(v & 0xFF));
		out.push_back((std::uint8_t)((v >> 8) & 0xFF));
		out.push_back((std::uint8_t)((v >> 16) & 0xFF));
		out.push_back((std::uint8_t)((v >> 24) & 0xFF));
	}

	inline void wrf32(std::vector<std::uint8_t>& out, float f)
	{
		std::uint32_t bits;
		std::memcpy(&bits, &f, 4);
		wr32(out, bits);
	}
}

void HvaEncoder::Identity(float* m)
{
	for (int i = 0; i < 12; ++i)
		m[i] = 0.0f;
	m[0] = 1.0f;
	m[5] = 1.0f;
	m[10] = 1.0f;
}

void HvaEncoder::Translate(float* m, float tx, float ty, float tz)
{
	Identity(m);
	m[3] = tx;
	m[7] = ty;
	m[11] = tz;
}

bool HvaEncoder::Encode(const std::vector<std::string>& sectionNames,
	int frameCount,
	const std::vector<float>& transforms,
	const std::string& fileName,
	std::vector<std::uint8_t>& out_data)
{
	out_data.clear();

	int sectionCount = (int)sectionNames.size();
	if (sectionCount <= 0 || sectionCount > 512)
		return false;
	if (frameCount <= 0 || frameCount > 65536)
		return false;
	if ((int)transforms.size() < frameCount * sectionCount * 12)
		return false;

	// ---- Header (24 字节) ----
	// fileName(16)：超长截断，不足补 0
	char name[16] = { 0 };
	std::strncpy(name, fileName.c_str(), 15);
	for (int i = 0; i < 16; ++i)
		out_data.push_back((std::uint8_t)name[i]);

	wr32(out_data, (std::uint32_t)frameCount);
	wr32(out_data, (std::uint32_t)sectionCount);

	// ---- Section Names（sectionCount × 16）----
	for (const auto& s : sectionNames)
	{
		char sn[16] = { 0 };
		std::strncpy(sn, s.c_str(), 15);
		for (int i = 0; i < 16; ++i)
			out_data.push_back((std::uint8_t)sn[i]);
	}

	// ---- Transform Matrices（frameCount × sectionCount × 48）----
	for (int f = 0; f < frameCount; ++f)
	{
		for (int s = 0; s < sectionCount; ++s)
		{
			const float* m = &transforms[((size_t)f * sectionCount + s) * 12];
			for (int j = 0; j < 12; ++j)
				wrf32(out_data, m[j]);
		}
	}

	return true;
}

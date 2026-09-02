#include <VxlNormals.h>

#include <cmath>

namespace VxlNormals {

int FindNormalIndex(int normalsMode, float nx, float ny, float nz)
{
	int count = 0;
	const float* table = GetNormal(normalsMode, count);
	if (count <= 0)
		return 0;

	// 归一化输入向量
	float len = std::sqrt(nx * nx + ny * ny + nz * nz);
	if (len < 1e-6f)
		return 0;  // 零向量（内部体素）→ 索引 0
	nx /= len;
	ny /= len;
	nz /= len;

	// 找最大点积（最接近的表项）
	int best = 0;
	float bestDot = -2.0f;
	for (int i = 0; i < count; ++i)
	{
		float dot = table[i * 3] * nx + table[i * 3 + 1] * ny + table[i * 3 + 2] * nz;
		if (dot > bestDot)
		{
			bestDot = dot;
			best = i;
		}
	}
	return best;
}

} // namespace VxlNormals

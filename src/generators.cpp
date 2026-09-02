#include <generators.h>

#include <VxlNormals.h>

#include <cstdint>

// 调色板固定索引（与 BuildMilitaryPalette 对应）
enum
{
	PAL_TRACK = 1,   // 深灰（履带）
	PAL_MGRAY = 2,   // 中灰
	PAL_LGRAY = 3,   // 浅灰
	PAL_DGREEN = 4,  // 深绿（车体）
	PAL_MGREEN = 5,  // 中绿（炮塔）
	PAL_LGREEN = 6,  // 浅绿
	PAL_BROWN = 7,   // 深棕
	PAL_TAN = 8,     // 沙色
	PAL_DRED = 9,    // 暗红
	PAL_RED = 10,    // 红
	PAL_DBLUE = 11,  // 深蓝
	PAL_BLUE = 12,   // 蓝
	PAL_WHITE = 13,  // 白
	PAL_BLACK = 14,  // 黑
	PAL_DGRAY = 15,  // 深灰
};

void BuildMilitaryPalette(std::vector<std::uint8_t>& palette)
{
	palette.assign(768, 0);
	auto set = [&](int idx, int r, int g, int b) {
		palette[idx * 3 + 0] = (std::uint8_t)r;
		palette[idx * 3 + 1] = (std::uint8_t)g;
		palette[idx * 3 + 2] = (std::uint8_t)b;
	};

	set(0, 0, 0, 0);
	set(PAL_TRACK, 58, 58, 58);
	set(PAL_MGRAY, 108, 108, 108);
	set(PAL_LGRAY, 158, 158, 158);
	set(PAL_DGREEN, 52, 88, 44);
	set(PAL_MGREEN, 78, 118, 58);
	set(PAL_LGREEN, 118, 158, 88);
	set(PAL_BROWN, 90, 60, 30);
	set(PAL_TAN, 178, 148, 98);
	set(PAL_DRED, 108, 30, 30);
	set(PAL_RED, 178, 40, 40);
	set(PAL_DBLUE, 40, 50, 110);
	set(PAL_BLUE, 70, 100, 180);
	set(PAL_WHITE, 228, 228, 228);
	set(PAL_BLACK, 20, 20, 20);
	set(PAL_DGRAY, 80, 80, 80);

	// 16-31：阵营色 remap（红渐变，预览时显示原始色）
	for (int i = 0; i < 16; ++i)
		set(16 + i, 110 + i * 8, 28, 28);

	// 32+：中性灰渐变填充（避免未定义索引全黑）
	for (int i = 32; i < 256; ++i)
	{
		int v = ((i - 32) * 6) % 235 + 10;
		set(i, v, v, v);
	}
}

bool BuildSectionFromVoxels(const std::string& name,
	const std::vector<VxlVoxel>& voxels,
	int sizeX, int sizeY, int sizeZ,
	int normalsMode,
	VxlSection& out_section)
{
	if (sizeX <= 0 || sizeY <= 0 || sizeZ <= 0 || sizeX > 255 || sizeY > 255 || sizeZ > 255)
		return false;

	// 占用网格 + 颜色网格
	std::vector<std::uint8_t> occ((size_t)sizeX * sizeY * sizeZ, 0);
	std::vector<std::uint8_t> col((size_t)sizeX * sizeY * sizeZ, 0);
	auto idx3 = [&](int x, int y, int z) { return ((size_t)z * sizeY + y) * sizeX + x; };

	for (const auto& v : voxels)
	{
		if (v.x < 0 || v.x >= sizeX || v.y < 0 || v.y >= sizeY || v.z < 0 || v.z >= sizeZ)
			return false;
		occ[idx3(v.x, v.y, v.z)] = 1;
		col[idx3(v.x, v.y, v.z)] = v.colorIndex;
	}

	auto isFilled = [&](int x, int y, int z) -> bool {
		if (x < 0 || x >= sizeX || y < 0 || y >= sizeY || z < 0 || z >= sizeZ)
			return false;
		return occ[idx3(x, y, z)] != 0;
	};

	out_section.name = name;
	out_section.sizeX = sizeX;
	out_section.sizeY = sizeY;
	out_section.sizeZ = sizeZ;
	out_section.normalsMode = normalsMode;
	out_section.hvaMultiplier = 0.083333f;
	out_section.minBounds[0] = 0.0f; out_section.minBounds[1] = 0.0f; out_section.minBounds[2] = 0.0f;
	out_section.maxBounds[0] = (float)sizeX; out_section.maxBounds[1] = (float)sizeY; out_section.maxBounds[2] = (float)sizeZ;
	out_section.spans.clear();

	for (int y = 0; y < sizeY; ++y)
	{
		for (int x = 0; x < sizeX; ++x)
		{
			VxlSpan span;
			span.x = x;
			span.y = y;
			for (int z = 0; z < sizeZ; ++z)
			{
				if (!isFilled(x, y, z))
					continue;
				VxlVoxel v;
				v.x = x; v.y = y; v.z = z;
				v.colorIndex = col[idx3(x, y, z)];

				// 邻域法线：暴露面朝外
				float nx = (isFilled(x + 1, y, z) ? 0.0f : 1.0f) - (isFilled(x - 1, y, z) ? 0.0f : 1.0f);
				float ny = (isFilled(x, y + 1, z) ? 0.0f : 1.0f) - (isFilled(x, y - 1, z) ? 0.0f : 1.0f);
				float nz = (isFilled(x, y, z + 1) ? 0.0f : 1.0f) - (isFilled(x, y, z - 1) ? 0.0f : 1.0f);
				if (nx == 0.0f && ny == 0.0f && nz == 0.0f)
				{
					nx = 0.0f; ny = 0.0f; nz = 1.0f;  // 内部体素：默认朝上
				}
				v.normalIndex = (std::uint8_t)VxlNormals::FindNormalIndex(normalsMode, nx, ny, nz);
				span.voxels.push_back(v);
			}
			out_section.spans.push_back(std::move(span));
		}
	}

	return true;
}
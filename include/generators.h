#pragma once

// 构建 VXL 的基本工具：军事调色板与体素集合 → VxlSection。
// 坐标约定（与 RA2 一致）：X=左右(宽) / Y=前后(长,车头朝+Y) / Z=上下(高)。

#include <cstdint>
#include <string>
#include <vector>

#include <VxlTypes.h>

// 构建 256 色军事调色板（768 字节 RGB）。
// 固定索引：1=深灰(履带) 2=中灰 3=浅灰 4=深绿(车体) 5=中绿(炮塔) 6=浅绿
//           7=深棕 8=沙色 9=暗红 10=红 11=深蓝 12=蓝 13=白 14=黑 15=深灰
//           16-31=阵营色 remap（红渐变）
void BuildMilitaryPalette(std::vector<std::uint8_t>& palette);

// 从体素集合构建 VxlSection（自动计算法线、bounds、spans）。
// voxels 需含 x/y/z/colorIndex；normalIndex 由邻域法线自动计算。
// normalsMode：1..4（RA2 用 4）。
bool BuildSectionFromVoxels(const std::string& name,
	const std::vector<VxlVoxel>& voxels,
	int sizeX, int sizeY, int sizeZ,
	int normalsMode,
	VxlSection& out_section);
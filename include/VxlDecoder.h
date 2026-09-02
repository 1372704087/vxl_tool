#pragma once

// VXL 体素模型解码器（自包含版，用于回读验证编码器输出）。
// 逻辑与 gamemd 工程 src/Data/VxlDecoder.cpp 完全一致。

#include <cstdint>
#include <vector>

#include <VxlTypes.h>

class VxlDecoder
{
public:
	// 解析 VXL 数据。返回 section 列表（含所有体素）。
	static bool Decode(const std::uint8_t* data, int size, std::vector<VxlSection>& out_sections,
		int& out_voxelCount);

	// 生成该 section 所有体素的坐标/颜色（展平）
	static void GetVoxels(const VxlSection& section, std::vector<VxlVoxel>& out_voxels);

	// 读取 VXL 头部自带的调色板（768 字节 RGB，offset 34 起，8-bit 已展开）。
	static bool GetPalette(const std::uint8_t* data, int size, std::uint8_t* out_rgb768);
};

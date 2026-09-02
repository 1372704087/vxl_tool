#pragma once

// HVA 解码器（用于回读验证 HvaEncoder 输出）。
// 布局与 gamemd 工程 HvaDecoder 一致：
//   Header(24): fileName(16) + frameCount(4) + sectionCount(4)
//   SectionNames: sectionCount × 16 字节
//   Matrices: frameCount × sectionCount × 48 字节（3×4 行主序矩阵，12 float）

#include <cstdint>
#include <string>
#include <vector>

struct HvaSectionInfo
{
	std::string name;
};

class HvaDecoder
{
public:
	// 解析 HVA 数据。返回帧数、section 数和所有矩阵。
	// out_transforms: 扁平存储，索引 [frame * sectionCount + section] × 12 float。
	static bool Decode(const std::uint8_t* data, int size,
		int& out_frameCount, int& out_sectionCount,
		std::vector<HvaSectionInfo>& out_sections,
		std::vector<float>& out_transforms);
};

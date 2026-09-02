#pragma once

// VXL 体素模型的共享数据类型（与 Phoenix YR / gamemd 工程的 VxlDecoder.h 一致）。
// 独立于 Windows/D3D11，可跨平台编译；也便于直接拷入 gamemd 工程复用。

#include <cstdint>
#include <string>
#include <vector>

// 单个体素
struct VxlVoxel
{
	int x, y, z;            // 体素格子坐标（X 左右 / Y 前后 / Z 上下）
	std::uint8_t colorIndex;   // 调色板颜色索引
	std::uint8_t normalIndex;  // 法线索引（对应 normalsMode 法线表）
};

// 一个 (x,y) 格内的体素列（span）
struct VxlSpan
{
	int x, y;                    // 所在格（x/y 平面）
	std::vector<VxlVoxel> voxels;  // 该格的体素列（按 z 升序）
};

// VXL 的一个 section（部件/肢体）
struct VxlSection
{
	std::string name;          // 段名（如 "Body"、"turret"）
	int sizeX, sizeY, sizeZ;   // 体素网格尺寸
	int normalsMode;           // 法线模式（1..4，RA2 用 4）

	float minBounds[3];        // 包围盒最小角（模型空间）
	float maxBounds[3];        // 包围盒最大角（模型空间）
	float hvaMultiplier;       // HVA 缩放系数（RA2 通常 0.083333）

	std::vector<VxlSpan> spans;   // 所有格（按 y 外层、x 内层遍历）
};

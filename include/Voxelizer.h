#pragma once

// 网格体素化器：把三角网格（OBJ）转换为体素集合。
// 采用射线投射法（ray casting）判定体素中心是否在网格内部，
// 沿 +X/+Y/+Z 三轴各投射一次取多数表决，以规避射线擦边/过顶点导致的误判。

#include <cstdint>
#include <vector>

#include <ObjLoader.h>
#include <VxlTypes.h>

class Voxelizer
{
public:
	// 将三角网格体素化。
	//   mesh       : 输入网格
	//   gridSize   : 体素网格最长轴边长（4..256，默认 64）
	//   baseColor  : 无顶点色时的基准调色板索引；0 = 按高度渐变（深绿→浅绿）
	//   upAxis     : OBJ 的"上"轴（'y' = Blender 默认，'z' = 3ds Max 默认）
	//   palette    : 768 字节 RGB 调色板（顶点色→最近色映射用；可传空跳过）
	//   out_voxels : 输出体素（x/y/z/colorIndex）
	//   out_size   : 输出网格尺寸 [sizeX, sizeY, sizeZ]
	static bool Voxelize(const ObjMesh& mesh, int gridSize, int baseColor, char upAxis,
		const std::vector<std::uint8_t>& palette,
		std::vector<VxlVoxel>& out_voxels, int out_size[3]);
};

#pragma once

// OBJ 网格解析器：读取 Wavefront OBJ 文本（顶点/面/可选顶点色）。
// 支持：
//   v x y z           顶点
//   v x y z r g b     顶点 + 顶点色（0..1 或 0..255）
//   f 1 2 3           面（支持 1/1、1//1 等索引形式，忽略纹理/法线索引）
//   o / g / usemtl / mtllib / s / vn / vt 等行被忽略
// 仅支持三角面（四边形/多边形会被三角剖分）。

#include <cstdint>
#include <string>
#include <vector>

// 一个三角面（顶点索引 0 基）
struct ObjFace
{
	int v0, v1, v2;
};

// 解析后的网格
struct ObjMesh
{
	std::vector<float> positions;      // 扁平 [x,y,z] × n
	std::vector<std::uint8_t> colors;  // 扁平 [r,g,b] × n（hasVertexColors 时有效）
	std::vector<ObjFace> faces;
	bool hasVertexColors = false;
	float minX = 0, minY = 0, minZ = 0;
	float maxX = 0, maxY = 0, maxZ = 0;
};

class ObjLoader
{
public:
	// 解析 OBJ 文本，成功返回 true 并填充 out。
	static bool Load(const std::string& text, ObjMesh& out);
};

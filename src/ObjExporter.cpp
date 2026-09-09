#include <ObjExporter.h>
#include <ObjLoader.h>
#include <VxlDecoder.h>

#include <cstdio>

namespace
{
	// 一个 section 的稀疏体素网格
	struct Grid
	{
		int sx = 0, sy = 0, sz = 0;
		std::vector<std::uint8_t> occ;    // 1 = 被占据
		std::vector<std::uint8_t> color;  // 每格的颜色索引（占用时有效）

		size_t Idx(int x, int y, int z) const { return ((size_t)z * sy + y) * sx + x; }
		bool In(int x, int y, int z) const { return x >= 0 && y >= 0 && z >= 0 && x < sx && y < sy && z < sz; }
		bool Solid(int x, int y, int z) const { return In(x, y, z) && occ[Idx(x, y, z)] != 0; }
	};

	// 立方体 8 个角（单位格）。顺序：
	//   0..3 = 底面(z=z0)，4..7 = 顶面(z=z1)，其中 0,1 靠近 -Y、2,3 靠近 +Y；0,3 靠 -X。
	static void Corners(int x, int y, int z, int s, int* out) // 返回 8 组 (X,Y,Z) 式坐标(写3倍)
	{
		// 直接按分量写，避免歧义：
		//   顶/底共用 x 方向 0..1、y 方向 0..1
		int X0 = x * s, X1 = (x + 1) * s;
		int Y0 = y * s, Y1 = (y + 1) * s;
		int Z0 = z * s, Z1 = (z + 1) * s;
		// 0=--, 1=+-, 2=++, 3=-+ (底面)；4..7 同下但 z=Z1
		out[0] = X0; out[1] = Y0; out[2] = Z0;
		out[3] = X1; out[4] = Y0; out[5] = Z0;
		out[6] = X1; out[7] = Y1; out[8] = Z0;
		out[9] = X0; out[10] = Y1; out[11] = Z0;
		out[12] = X0; out[13] = Y0; out[14] = Z1;
		out[15] = X1; out[16] = Y0; out[17] = Z1;
		out[18] = X1; out[19] = Y1; out[20] = Z1;
		out[21] = X0; out[22] = Y1; out[23] = Z1;
	}

	// 6 个外露面：每项 = (方向的单位判定, 该面上 4 个角的下标 0..7)
	// 角下标来自 Corners 输出的 8 个点顺序。
	struct Face
	{
		int axis;       // 0=X, 1=Y, 2=Z
		int neg;        // 1=负轴, 0=正轴
		int c[4];
	};
	static const Face kFaces[6] = {
		{ 0, 0, { 1, 2, 6, 5 } },  // +X
		{ 0, 1, { 0, 3, 7, 4 } },  // -X
		{ 1, 0, { 3, 2, 6, 7 } },  // +Y
		{ 1, 1, { 0, 1, 5, 4 } },  // -Y
		{ 2, 0, { 4, 5, 6, 7 } },  // +Z
		{ 2, 1, { 0, 1, 2, 3 } },  // -Z
	};

	// 计算三角 (a,b,c) 的叉积法向在 axis 轴上的符号（用整数坐标，仅判方向，放大不受影响）
	inline long XprodAxis(const int a[3], const int b[3], const int c[3], int axis)
	{
		// n = (b-a)x(c-a)，取 axis 分量。axis0= (b1-a1)(c2-a2)-(b2-a2)(c1-a1)；axis1、axis2 循环。
		long ba[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
		long ca[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
		int u = (axis + 1) % 3, w = (axis + 2) % 3;
		return ba[u] * ca[w] - ba[w] * ca[u];
	}
}

bool ObjExporter::ExportObjText(const std::vector<VxlSection>& sections,
	const std::vector<std::uint8_t>& palette,
	int scale,
	std::string& out)
{
	if (scale < 1) scale = 1;
	out.clear();

	std::string body;   // 先累积，便于统计再写头
	body.reserve(1 << 20);

	int totalVertices = 0;
	int totalFaces = 0;

	for (size_t si = 0; si < sections.size(); ++si)
	{
		const VxlSection& sec = sections[si];

		// 1) 建立占用网格 + 颜色
		Grid g;
		g.sx = sec.sizeX;
		g.sy = sec.sizeY;
		g.sz = sec.sizeZ;
		if (g.sx <= 0 || g.sy <= 0 || g.sz <= 0)
			continue;
		size_t cells = (size_t)g.sx * g.sy * g.sz;
		g.occ.assign(cells, 0);
		g.color.assign(cells, 0);

		std::vector<VxlVoxel> voxels;
		VxlDecoder::GetVoxels(sec, voxels);
		for (const VxlVoxel& v : voxels)
		{
			if (!g.In(v.x, v.y, v.z))
				continue;   // 防御：越界体素忽略
			size_t i = g.Idx(v.x, v.y, v.z);
			g.occ[i] = 1;
			g.color[i] = v.colorIndex;
		}

		// 2) 遍历，输出与空气相邻的外露面
		int faceStart = totalFaces;   // 该 section 开始前的面数（用于统计本组）
		for (int z = 0; z < g.sz; ++z)
			for (int y = 0; y < g.sy; ++y)
				for (int x = 0; x < g.sx; ++x)
				{
					if (!g.occ[g.Idx(x, y, z)])
						continue;

					// 体素颜色（调色板安全取色）
					int ci = g.color[g.Idx(x, y, z)];
					int r = 255, gg2 = 255, b = 255;
					if (ci >= 0 && (size_t)ci * 3 + 2 < palette.size())
					{
						r = palette[ci * 3 + 0];
						gg2 = palette[ci * 3 + 1];
						b = palette[ci * 3 + 2];
					}

					int corners[24];
					Corners(x, y, z, scale, corners);

					for (int fi = 0; fi < 6; ++fi)
					{
						const Face& f = kFaces[fi];
						// 邻格是否被占？占用则此面不暴露（内部面剔除）
						int nx = x, ny = y, nz = z;
						if (f.axis == 0) nx += f.neg ? -1 : 1;
						else if (f.axis == 1) ny += f.neg ? -1 : 1;
						else nz += f.neg ? -1 : 1;
						if (g.Solid(nx, ny, nz))
							continue;

						// 取 4 角坐标
						const int* p[4];
						for (int k = 0; k < 4; ++k) p[k] = &corners[f.c[k] * 3];

						// 校正反向：保证 (p0,p1,p2) 叉积法向指向面的外侧（即 axis 方向）。
						// 反向需把面顶点顺序倒转（交换 p1<->p3），交换 p2/p3 不会翻转扇形法向。
						int sign = (f.neg ? -1 : 1);
						long cross = XprodAxis(p[0], p[1], p[2], f.axis);
						// 需要的方向：正轴时 cross 应 >0；负轴时应 <0，等价于 cross * sign > 0。
						if (cross * sign < 0)
							{ const int* t = p[1]; p[1] = p[3]; p[3] = t; }

						char line[160];
						int base = totalVertices + 1;
						for (int k = 0; k < 4; ++k)
						{
							std::snprintf(line, sizeof line, "v %d %d %d %d %d %d\n",
								p[k][0], p[k][1], p[k][2], r, gg2, b);
							body += line;
							++totalVertices;
						}
						std::snprintf(line, sizeof line, "f %d %d %d\n", base, base + 1, base + 2);
						body += line;
						std::snprintf(line, sizeof line, "f %d %d %d\n", base, base + 2, base + 3);
						body += line;
						totalFaces += 2;
					}
				}

		// 汇总该 section（写一个分组头）
		char hdr[192];
		std::snprintf(hdr, sizeof hdr,
			"o %s\n# %s: %d voxel, %d face, 网格 %dx%dx%d, scale=%d\n",
			sec.name.c_str(), sec.name.c_str(),
			(int)voxels.size(), totalFaces - faceStart, g.sx, g.sy, g.sz, scale);
		out += hdr;
		out += body;
		body.clear();
	}

	// 头部注释（统一写在最前面，便于阅读）
	if (totalFaces == 0)
		out = "# 空模型：无 body 面被输出\n" + out;
	return true;
}
#include <Voxelizer.h>

#include <algorithm>
#include <cmath>

namespace
{
	// 均匀空间网格：加速"最近三角形"查询与射线投射
	struct TriGrid
	{
		int nx = 1, ny = 1, nz = 1;
		float ox = 0, oy = 0, oz = 0, cell = 1;
		std::vector<std::vector<int>> cells;

		static int ClampCell(float v, int n)
		{
			int i = (int)v;
			if (i < 0) i = 0;
			if (i >= n) i = n - 1;
			return i;
		}

		void Build(const ObjMesh& mesh, int maxDim = 32)
		{
			float bx = mesh.maxX - mesh.minX, by = mesh.maxY - mesh.minY, bz = mesh.maxZ - mesh.minZ;
			float maxD = std::max({ bx, by, bz });
			if (maxD <= 0) maxD = 1.0f;
			nx = std::max(1, (int)std::lround(bx / maxD * maxDim));
			ny = std::max(1, (int)std::lround(by / maxD * maxDim));
			nz = std::max(1, (int)std::lround(bz / maxD * maxDim));
			ox = mesh.minX; oy = mesh.minY; oz = mesh.minZ;
			cell = maxD / maxDim;
			cells.assign((size_t)nx * ny * nz, {});
			const float* P = mesh.positions.data();
			for (size_t i = 0; i < mesh.faces.size(); ++i)
			{
				const ObjFace& f = mesh.faces[i];
				const float* a = &P[f.v0 * 3];
				const float* b = &P[f.v1 * 3];
				const float* c = &P[f.v2 * 3];
				float mnx = std::min({ a[0], b[0], c[0] }), mxx = std::max({ a[0], b[0], c[0] });
				float mny = std::min({ a[1], b[1], c[1] }), mxy = std::max({ a[1], b[1], c[1] });
				float mnz = std::min({ a[2], b[2], c[2] }), mxz = std::max({ a[2], b[2], c[2] });
				int x0 = ClampCell((mnx - ox) / cell, nx), x1 = ClampCell((mxx - ox) / cell, nx);
				int y0 = ClampCell((mny - oy) / cell, ny), y1 = ClampCell((mxy - oy) / cell, ny);
				int z0 = ClampCell((mnz - oz) / cell, nz), z1 = ClampCell((mxz - oz) / cell, nz);
				for (int z = z0; z <= z1; ++z)
					for (int y = y0; y <= y1; ++y)
						for (int x = x0; x <= x1; ++x)
							cells[((size_t)z * ny + y) * nx + x].push_back((int)i);
			}
		}
	};

	// Möller–Trumbore：射线 O + t*D 与三角形 ABC 求交，返回 t（>0 命中），否则 -1
	float RayTriangle(const float* o, const float* d,
		const float* a, const float* b, const float* c)
	{
		const float e1x = b[0] - a[0], e1y = b[1] - a[1], e1z = b[2] - a[2];
		const float e2x = c[0] - a[0], e2y = c[1] - a[1], e2z = c[2] - a[2];
		const float px = d[1] * e2z - d[2] * e2y;
		const float py = d[2] * e2x - d[0] * e2z;
		const float pz = d[0] * e2y - d[1] * e2x;
		const float det = e1x * px + e1y * py + e1z * pz;
		if (det > -1e-9f && det < 1e-9f)
			return -1.0f;
		const float inv = 1.0f / det;
		const float tx = o[0] - a[0], ty = o[1] - a[1], tz = o[2] - a[2];
		const float u = (tx * px + ty * py + tz * pz) * inv;
		if (u < 0.0f || u > 1.0f)
			return -1.0f;
		const float qx = ty * e1z - tz * e1y;
		const float qy = tz * e1x - tx * e1z;
		const float qz = tx * e1y - ty * e1x;
		const float v = (d[0] * qx + d[1] * qy + d[2] * qz) * inv;
		if (v < 0.0f || u + v > 1.0f)
			return -1.0f;
		const float t = (e2x * qx + e2y * qy + e2z * qz) * inv;
		return t;
	}

	// 沿指定轴（axis=0/1/2 对应 X/Y/Z，正方向）投射射线，统计与网格相交次数。
	// 沿射线方向逐格行走，只测试射线经过格子内的面（空间网格加速）。
	int CountHits(const TriGrid& g, const ObjMesh& mesh,
		float px, float py, float pz, int axis)
	{
		const float* P = mesh.positions.data();
		const float d[3] = { axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f };
		const float o[3] = { px, py, pz };
		int hits = 0;
		int cx = TriGrid::ClampCell((px - g.ox) / g.cell, g.nx);
		int cy = TriGrid::ClampCell((py - g.oy) / g.cell, g.ny);
		int cz = TriGrid::ClampCell((pz - g.oz) / g.cell, g.nz);
		auto testCell = [&](int x, int y, int z) {
			const auto& cell = g.cells[((size_t)z * g.ny + y) * g.nx + x];
			for (int fi : cell)
			{
				const ObjFace& f = mesh.faces[fi];
				float t = RayTriangle(o, d, &P[f.v0 * 3], &P[f.v1 * 3], &P[f.v2 * 3]);
				if (t > 1e-6f)
					++hits;
			}
		};
		if (axis == 0)
			for (int x = cx; x < g.nx; ++x) testCell(x, cy, cz);
		else if (axis == 1)
			for (int y = cy; y < g.ny; ++y) testCell(cx, y, cz);
		else
			for (int z = cz; z < g.nz; ++z) testCell(cx, cy, z);
		return hits;
	}

	// 点是否在网格内部：三轴射线投射多数表决
	bool PointInMesh(const TriGrid& g, const ObjMesh& mesh,
		float px, float py, float pz)
	{
		int votes = 0;
		if (CountHits(g, mesh, px, py, pz, 0) % 2 == 1) ++votes;
		if (CountHits(g, mesh, px, py, pz, 1) % 2 == 1) ++votes;
		if (CountHits(g, mesh, px, py, pz, 2) % 2 == 1) ++votes;
		return votes >= 2;
	}

	// 点到三角形最近点（Ericson, Real-Time Collision Detection）
	void ClosestPointTriangle(const float* p, const float* a, const float* b, const float* c, float out[3])
	{
		float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
		float ac[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
		float ap[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
		float d1 = ab[0] * ap[0] + ab[1] * ap[1] + ab[2] * ap[2];
		float d2 = ac[0] * ap[0] + ac[1] * ap[1] + ac[2] * ap[2];
		if (d1 <= 0.0f && d2 <= 0.0f) { out[0] = a[0]; out[1] = a[1]; out[2] = a[2]; return; }
		float bp[3] = { p[0] - b[0], p[1] - b[1], p[2] - b[2] };
		float d3 = ab[0] * bp[0] + ab[1] * bp[1] + ab[2] * bp[2];
		float d4 = ac[0] * bp[0] + ac[1] * bp[1] + ac[2] * bp[2];
		if (d3 >= 0.0f && d4 <= d3) { out[0] = b[0]; out[1] = b[1]; out[2] = b[2]; return; }
		float vc = d1 * d4 - d3 * d2;
		if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
		{
			float v = d1 / (d1 - d3);
			out[0] = a[0] + v * ab[0]; out[1] = a[1] + v * ab[1]; out[2] = a[2] + v * ab[2]; return;
		}
		float cp[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] };
		float d5 = ab[0] * cp[0] + ab[1] * cp[1] + ab[2] * cp[2];
		float d6 = ac[0] * cp[0] + ac[1] * cp[1] + ac[2] * cp[2];
		if (d6 >= 0.0f && d5 <= d6) { out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; return; }
		float vb = d5 * d2 - d1 * d6;
		if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
		{
			float w = d2 / (d2 - d6);
			out[0] = a[0] + w * ac[0]; out[1] = a[1] + w * ac[1]; out[2] = a[2] + w * ac[2]; return;
		}
		float va = d3 * d6 - d5 * d4;
		if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
		{
			float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
			out[0] = b[0] + w * (c[0] - b[0]); out[1] = b[1] + w * (c[1] - b[1]); out[2] = b[2] + w * (c[2] - b[2]); return;
		}
		float denom = 1.0f / (va + vb + vc);
		float v = vb * denom;
		float w = vc * denom;
		out[0] = a[0] + ab[0] * v + ac[0] * w;
		out[1] = a[1] + ab[1] * v + ac[1] * w;
		out[2] = a[2] + ab[2] * v + ac[2] * w;
	}

	// 重心插值顶点色
	void InterpColor(const ObjMesh& mesh, const ObjFace& f, const float* q, std::uint8_t out[3])
	{
		const float* P = mesh.positions.data();
		const std::uint8_t* C = mesh.colors.data();
		const float* a = &P[f.v0 * 3];
		const float* b = &P[f.v1 * 3];
		const float* c = &P[f.v2 * 3];
		float v0[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
		float v1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
		float v2[3] = { q[0] - a[0], q[1] - a[1], q[2] - a[2] };
		float d00 = v0[0] * v0[0] + v0[1] * v0[1] + v0[2] * v0[2];
		float d01 = v0[0] * v1[0] + v0[1] * v1[1] + v0[2] * v1[2];
		float d11 = v1[0] * v1[0] + v1[1] * v1[1] + v1[2] * v1[2];
		float d20 = v2[0] * v0[0] + v2[1] * v0[1] + v2[2] * v0[2];
		float d21 = v2[0] * v1[0] + v2[1] * v1[1] + v2[2] * v1[2];
		float denom = d00 * d11 - d01 * d01;
		float vv = 0.0f, ww = 0.0f;
		if (denom > 1e-12f)
		{
			vv = (d11 * d20 - d01 * d21) / denom;
			ww = (d00 * d21 - d01 * d20) / denom;
		}
		float uu = 1.0f - vv - ww;
		const std::uint8_t* ca = &C[f.v0 * 3];
		const std::uint8_t* cb = &C[f.v1 * 3];
		const std::uint8_t* cc = &C[f.v2 * 3];
		// [修复 C1] 重心插值顶点色修正：
		//   v0 = C-A、v1 = B-A，故 vv 是顶点 C 的权重、ww 是顶点 B 的权重、uu 是顶点 A 的权重。
		//   因此 B、C 的颜色应分别乘其权重 ww、vv（原实现把 cb/cc 与权重张冠李戴，导致颜色互换）。
		for (int k = 0; k < 3; ++k)
			out[k] = (std::uint8_t)(uu * ca[k] + vv * cc[k] + ww * cb[k] + 0.5f);
	}

	// 取最近三角形并插值顶点色（空间网格加速）
	bool NearestTriangleColor(const TriGrid& g, const ObjMesh& mesh,
		float px, float py, float pz, std::uint8_t out[3])
	{
		const float* P = mesh.positions.data();
		int cx = TriGrid::ClampCell((px - g.ox) / g.cell, g.nx);
		int cy = TriGrid::ClampCell((py - g.oy) / g.cell, g.ny);
		int cz = TriGrid::ClampCell((pz - g.oz) / g.cell, g.nz);
		float best = 1e30f;
		bool found = false;
		std::uint8_t bestC[3] = { 0, 0, 0 };
		for (int r = 0; ; ++r)
		{
			if (r > 0 && (r - 1) * g.cell > std::sqrt(best))
				break;
			int x0 = std::max(0, cx - r), x1 = std::min(g.nx - 1, cx + r);
			int y0 = std::max(0, cy - r), y1 = std::min(g.ny - 1, cy + r);
			int z0 = std::max(0, cz - r), z1 = std::min(g.nz - 1, cz + r);
			for (int z = z0; z <= z1; ++z)
				for (int y = y0; y <= y1; ++y)
					for (int x = x0; x <= x1; ++x)
					{
						if (r > 0 && x > x0 && x < x1 && y > y0 && y < y1 && z > z0 && z < z1)
							continue;  // 只处理 shell 边界
						const auto& cell = g.cells[((size_t)z * g.ny + y) * g.nx + x];
						for (int fi : cell)
						{
							const ObjFace& f = mesh.faces[fi];
							const float* a = &P[f.v0 * 3];
							const float* b = &P[f.v1 * 3];
							const float* c = &P[f.v2 * 3];
							float q[3];
							float p[3] = { px, py, pz };
							ClosestPointTriangle(p, a, b, c, q);
							float dx = q[0] - px, dy = q[1] - py, dz = q[2] - pz;
							float d = dx * dx + dy * dy + dz * dz;
							if (d < best)
							{
								best = d;
								found = true;
								InterpColor(mesh, f, q, bestC);
							}
						}
					}
			if (x0 == 0 && y0 == 0 && z0 == 0 && x1 == g.nx - 1 && y1 == g.ny - 1 && z1 == g.nz - 1)
				break;
		}
		if (!found)
			return false;
		out[0] = bestC[0]; out[1] = bestC[1]; out[2] = bestC[2];
		return true;
	}

	// RGB → 调色板最近色索引
	int NearestPalette(const std::vector<std::uint8_t>& palette, int r, int g, int b)
	{
		if (palette.size() < 768)
			return 4;
		int best = 0, bestD = 1 << 30;
		for (int i = 0; i < 256; ++i)
		{
			int pr = palette[i * 3 + 0], pg = palette[i * 3 + 1], pb = palette[i * 3 + 2];
			int dr = pr - r, dg = pg - g, db = pb - b;
			int d = dr * dr + dg * dg + db * db;
			if (d < bestD) { bestD = d; best = i; }
		}
		return best;
	}

	// 高度渐变：底部深绿 → 顶部浅绿
	int HeightColor(float nz)
	{
		if (nz < 0.33f) return 4;   // 深绿
		if (nz < 0.66f) return 5;   // 中绿
		return 6;                   // 浅绿
	}
}

bool Voxelizer::Voxelize(const ObjMesh& mesh, int gridSize, int baseColor, char upAxis,
	const std::vector<std::uint8_t>& palette,
	std::vector<VxlVoxel>& out_voxels, int out_size[3])
{
	out_voxels.clear();
	if (gridSize < 4) gridSize = 4;
	if (gridSize > 256) gridSize = 256;

	// 轴映射：OBJ 空间 ↔ VXL 空间
	//   upAxis 'y'（Blender 默认）: VXL(x,y,z) = OBJ(x, z, y)
	//   upAxis 'z'（3ds Max 默认）: VXL(x,y,z) = OBJ(x, y, z)
	auto mapObjToVxl = [&](float ox, float oy, float oz, float& vx, float& vy, float& vz) {
		if (upAxis == 'y') { vx = ox; vy = oz; vz = oy; }
		else { vx = ox; vy = oy; vz = oz; }
	};
	auto mapVxlToObj = [&](float vx, float vy, float vz, float& ox, float& oy, float& oz) {
		if (upAxis == 'y') { ox = vx; oy = vz; oz = vy; }
		else { ox = vx; oy = vy; oz = vz; }
	};

	// 空间网格：同时加速"内部判定"射线投射与顶点色最近三角形查询。
	// maxDim 按面数自适应，保证每格面数可控（大网格用更细的格子）。
	int gridMaxDim = 32;
	if (mesh.faces.size() > 200000) gridMaxDim = 64;
	if (mesh.faces.size() > 800000) gridMaxDim = 96;
	if (mesh.faces.size() > 2000000) gridMaxDim = 128;
	TriGrid triGrid;
	triGrid.Build(mesh, gridMaxDim);

	// VXL 空间包围盒（变换 8 角）
	float corners[8][3] = {
		{ mesh.minX, mesh.minY, mesh.minZ }, { mesh.maxX, mesh.minY, mesh.minZ },
		{ mesh.minX, mesh.maxY, mesh.minZ }, { mesh.maxX, mesh.maxY, mesh.minZ },
		{ mesh.minX, mesh.minY, mesh.maxZ }, { mesh.maxX, mesh.minY, mesh.maxZ },
		{ mesh.minX, mesh.maxY, mesh.maxZ }, { mesh.maxX, mesh.maxY, mesh.maxZ },
	};
	float bmin[3] = { 1e30f, 1e30f, 1e30f }, bmax[3] = { -1e30f, -1e30f, -1e30f };
	for (const auto& c : corners)
	{
		float vx, vy, vz;
		mapObjToVxl(c[0], c[1], c[2], vx, vy, vz);
		bmin[0] = std::min(bmin[0], vx); bmax[0] = std::max(bmax[0], vx);
		bmin[1] = std::min(bmin[1], vy); bmax[1] = std::max(bmax[1], vy);
		bmin[2] = std::min(bmin[2], vz); bmax[2] = std::max(bmax[2], vz);
	}
	float bx = bmax[0] - bmin[0], by = bmax[1] - bmin[1], bz = bmax[2] - bmin[2];
	if (bx <= 0) bx = 1.0f;
	if (by <= 0) by = 1.0f;
	if (bz <= 0) bz = 1.0f;

	float maxDim = std::max({ bx, by, bz });
	int sx = std::max(1, (int)std::lround(bx / maxDim * gridSize));
	int sy = std::max(1, (int)std::lround(by / maxDim * gridSize));
	int sz = std::max(1, (int)std::lround(bz / maxDim * gridSize));
	out_size[0] = sx; out_size[1] = sy; out_size[2] = sz;

	for (int z = 0; z < sz; ++z)
	{
		for (int y = 0; y < sy; ++y)
		{
			for (int x = 0; x < sx; ++x)
			{
				float vx = bmin[0] + (x + 0.5f) * (bx / sx);
				float vy = bmin[1] + (y + 0.5f) * (by / sy);
				float vz = bmin[2] + (z + 0.5f) * (bz / sz);
				float ox, oy, oz;
				mapVxlToObj(vx, vy, vz, ox, oy, oz);
				if (!PointInMesh(triGrid, mesh, ox, oy, oz))
					continue;

				int color = baseColor;
				if (color <= 0)
				{
					if (mesh.hasVertexColors && palette.size() >= 768)
					{
						std::uint8_t c[3];
						if (NearestTriangleColor(triGrid, mesh, ox, oy, oz, c))
							color = NearestPalette(palette, c[0], c[1], c[2]);
						else
							color = HeightColor((vz - bmin[2]) / bz);
					}
					else
					{
						color = HeightColor((vz - bmin[2]) / bz);
					}
				}
				out_voxels.push_back({ x, y, z, (std::uint8_t)color, 0 });
			}
		}
	}
	return true;
}

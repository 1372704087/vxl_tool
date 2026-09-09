#include <VoxelRenderer.h>

#include <VxlDecoder.h>
#include <VxlNormals.h>

#include <algorithm>
#include <cmath>

namespace
{
	// 体素 → 模型空间（与 gamemd MapVoxelWithHva 一致）
	inline void MapVoxel(const VxlSection& sec, const VxlVoxel& v,
		const float* hm, float& x, float& y, float& z)
	{
		float sx = sec.sizeX > 0 ? (sec.maxBounds[0] - sec.minBounds[0]) / sec.sizeX : 1.0f;
		float sy = sec.sizeY > 0 ? (sec.maxBounds[1] - sec.minBounds[1]) / sec.sizeY : 1.0f;
		float sz = sec.sizeZ > 0 ? (sec.maxBounds[2] - sec.minBounds[2]) / sec.sizeZ : 1.0f;
		float gx = v.x * sx;
		float gy = v.y * sy;
		float gz = v.z * sz;
		if (hm)
		{
			x = hm[0] * gx + hm[1] * gy + hm[2] * gz + hm[3] * sec.hvaMultiplier * sx;
			y = hm[4] * gx + hm[5] * gy + hm[6] * gz + hm[7] * sec.hvaMultiplier * sy;
			z = hm[8] * gx + hm[9] * gy + hm[10] * gz + hm[11] * sec.hvaMultiplier * sz;
		}
		else
		{
			x = gx; y = gy; z = gz;
		}
		x += sec.minBounds[0];
		y += sec.minBounds[1];
		z += sec.minBounds[2];
	}

	// 取体素法线并应用 HVA 旋转（与 gamemd SetVoxelNormal 一致）
	inline void GetNormal(const VxlSection& sec, const VxlVoxel& v,
		const float* hm, float& nx, float& ny, float& nz)
	{
		int count = 0;
		const float* nrm = VxlNormals::GetNormal(sec.normalsMode, count);
		int ni = v.normalIndex;
		if (ni >= count) ni = count - 1;
		if (ni < 0) ni = 0;
		nx = nrm[ni * 3 + 0];
		ny = nrm[ni * 3 + 1];
		nz = nrm[ni * 3 + 2];
		if (hm)
		{
			float ox = hm[0] * nx + hm[1] * ny + hm[2] * nz;
			float oy = hm[4] * nx + hm[5] * ny + hm[6] * nz;
			float oz = hm[8] * nx + hm[9] * ny + hm[10] * nz;
			nx = ox; ny = oy; nz = oz;
		}
	}

	struct DrawVoxel
	{
		float sx, sy;   // 屏幕坐标（模型单位）
		float depth;    // 深度（越大越近）
		std::uint8_t r, g, b;
	};
}

bool VoxelRenderer::Render(const std::vector<VxlSection>& sections,
	const std::vector<std::uint8_t>& palette,
	const std::vector<float>& hvaTransforms,
	int hvaSectionCount, int frame,
	const RenderOptions& opt,
	std::vector<std::uint8_t>& out_rgba)
{
	const int W = opt.imageSize;
	const int H = opt.imageSize;
	if (W <= 0 || H <= 0)
		return false;
	// [修复 C3] 调色板必须包含 256×3 字节；否则后续 palette[ci*3+2] 会越界读。
	if (palette.size() < 768)
		return false;

	// 天空渐变背景（上冷蓝 → 下浅靛），营造悬浮于云海的氛围
	const unsigned bgTop[3] = { 66, 88, 128 };
	const unsigned bgBot[3] = { 34, 48, 82 };
	out_rgba.assign((size_t)W * H * 4, 0);
	for (int yy = 0; yy < H; ++yy)
	{
		float t = (float)yy / (float)std::max(H - 1, 1);
		unsigned r = (unsigned)(bgTop[0] + (bgBot[0] - bgTop[0]) * t);
		unsigned g = (unsigned)(bgTop[1] + (bgBot[1] - bgTop[1]) * t);
		unsigned b = (unsigned)(bgTop[2] + (bgBot[2] - bgTop[2]) * t);
		for (int xx = 0; xx < W; ++xx)
		{
			size_t idx = ((size_t)yy * W + xx) * 4;
			out_rgba[idx + 0] = (std::uint8_t)r;
			out_rgba[idx + 1] = (std::uint8_t)g;
			out_rgba[idx + 2] = (std::uint8_t)b;
			out_rgba[idx + 3] = 255;
		}
	}

	// 相机变换（绕 Z 旋转 yaw，绕 X 旋转 pitch）
	float cy = std::cos(opt.yaw), sy = std::sin(opt.yaw);
	float cp = std::cos(opt.pitch), sp = std::sin(opt.pitch);

	// 光照方向（模型空间，上-前-右）
	float lx = 0.4f, ly = 0.6f, lz = 0.7f;
	{
		float len = std::sqrt(lx * lx + ly * ly + lz * lz);
		lx /= len; ly /= len; lz /= len;
	}
	const float ambient = 0.42f;
	const float diffuse = 0.62f;

	// 收集所有体素的投影
	std::vector<DrawVoxel> voxels;
	voxels.reserve(20000);
	float minSX = 1e30f, maxSX = -1e30f, minSY = 1e30f, maxSY = -1e30f;

	for (size_t s = 0; s < sections.size(); ++s)
	{
		const VxlSection& sec = sections[s];

		const float* hm = nullptr;
		if (!hvaTransforms.empty() && hvaSectionCount > 0 && (int)s < hvaSectionCount)
		{
			size_t base = ((size_t)frame * hvaSectionCount + s) * 12;
			if (base + 12 <= hvaTransforms.size())
				hm = &hvaTransforms[base];
		}

		std::vector<VxlVoxel> secVoxels;
		VxlDecoder::GetVoxels(sec, secVoxels);

		for (const auto& v : secVoxels)
		{
			float mx, my, mz;
			MapVoxel(sec, v, hm, mx, my, mz);

			// 相机旋转
			float rx = mx * cy - my * sy;
			float ry = mx * sy + my * cy;
			float rz = mz;
			float py = ry * cp - rz * sp;
			float pz = ry * sp + rz * cp;

			DrawVoxel d;
			d.sx = rx;
			d.sy = -py;      // 屏幕 y 向下
			d.depth = pz;    // 越大越近

			// 法线漫反射着色
			float nx, ny, nz;
			GetNormal(sec, v, hm, nx, ny, nz);
			float diff = nx * lx + ny * ly + nz * lz;
			if (diff < 0.0f) diff = 0.0f;
			float light = ambient + diffuse * diff;
			if (light > 1.0f) light = 1.0f;

			// [修复 C4] 移除原"ci<0 / ci>255"死代码。v.colorIndex 为 uint8_t，
			//   其值天然在 [0,255]，而 palette 长度已在函数入口校验为 >=768，索引始终安全。
			int ci = v.colorIndex;
			// [修复 C5] 光照后 +0.5f 再强转做四舍五入，避免环境光下中低亮度体素被截断偏暗。
			d.r = (std::uint8_t)(palette[ci * 3 + 0] * light + 0.5f);
			d.g = (std::uint8_t)(palette[ci * 3 + 1] * light + 0.5f);
			d.b = (std::uint8_t)(palette[ci * 3 + 2] * light + 0.5f);

			minSX = std::min(minSX, d.sx); maxSX = std::max(maxSX, d.sx);
			minSY = std::min(minSY, d.sy); maxSY = std::max(maxSY, d.sy);
			voxels.push_back(d);
		}
	}

	if (voxels.empty())
		return false;

	// 计算缩放与偏移，使模型居中并适配图像
	float spanX = maxSX - minSX;
	float spanY = maxSY - minSY;
	if (spanX < 1e-6f) spanX = 1.0f;
	if (spanY < 1e-6f) spanY = 1.0f;
	int margin = W / 20;
	float avail = (float)(W - 2 * margin);
	float scale = avail / std::max(spanX, spanY) * opt.zoom;
	float offsetX = (float)W * 0.5f - (minSX + maxSX) * 0.5f * scale;
	float offsetY = (float)H * 0.5f - (minSY + maxSY) * 0.5f * scale;

	// 画家算法：先画远处（depth 小）
	std::sort(voxels.begin(), voxels.end(),
		[](const DrawVoxel& a, const DrawVoxel& b) { return a.depth < b.depth; });

	int sq = (int)std::ceil(scale);
	if (sq < 1) sq = 1;

	for (const auto& d : voxels)
	{
		int px = (int)std::lround(d.sx * scale + offsetX);
		int py = (int)std::lround(d.sy * scale + offsetY);
		for (int dy = 0; dy < sq; ++dy)
		{
			int yy = py + dy;
			if (yy < 0 || yy >= H) continue;
			for (int dx = 0; dx < sq; ++dx)
			{
				int xx = px + dx;
				if (xx < 0 || xx >= W) continue;
				size_t idx = ((size_t)yy * W + xx) * 4;
				out_rgba[idx + 0] = d.r;
				out_rgba[idx + 1] = d.g;
				out_rgba[idx + 2] = d.b;
				out_rgba[idx + 3] = 255;
			}
		}
	}

	return true;
}

namespace
{
	struct DrawV2
	{
		float sx, sy;         // 屏幕坐标
		float depth;          // 越大越近
		std::uint8_t r, g, b, a;
	};

	// 邻居法向：方向指向周围 6 邻格中"空"方（越界算空），归一化后作为该体素表面法向，
	// 从而自动获得平滑的体素曲面明暗（云/岩/屋脊均适用），无需依赖存储的 normal。
	void NeighborNormal(const char* occ, int sx, int sy, int sz,
		int cx, int cy, int cz, float& nx, float& ny, float& nz)
	{
		const int dx6[6] = { 1, -1, 0, 0, 0, 0 };
		const int dy6[6] = { 0, 0, 1, -1, 0, 0 };
		const int dz6[6] = { 0, 0, 0, 0, 1, -1 };
		float vx = 0, vy = 0, vz = 0;
		auto empty = [&](int x, int y, int z) {
			if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) return true;
			return occ[((size_t)z * sy + y) * sx + x] == 0;
		};
		for (int i = 0; i < 6; ++i)
		{
			if (empty(cx + dx6[i], cy + dy6[i], cz + dz6[i]))
			{
				vx += dx6[i]; vy += dy6[i]; vz += dz6[i];
			}
		}
		float len = std::sqrt(vx * vx + vy * vy + vz * vz);
		if (len < 1e-6f) { nx = 0; ny = 0; nz = 1; }
		else { nx = vx / len; ny = vy / len; nz = vz / len; }
	}
}

bool VoxelRenderer::RenderVxl2(const std::vector<VxlSection2>& sections,
	const VxlGlobal2* global, const RenderOptions& opt,
	std::vector<std::uint8_t>& out_rgba)
{
	const int W = opt.imageSize;
	const int H = opt.imageSize;
	if (W <= 0 || H <= 0 || sections.empty())
		return false;

	// 模型缩放（用全局 unitScale，缺省 1）
	float unit = (global && global->unitScale > 0.f) ? global->unitScale : 1.f;

	// 天空渐变背景（上冷蓝 → 下深靛），营造悬浮于云海的氛围
	const unsigned bgTop[3] = { 64, 84, 120 };
	const unsigned bgBot[3] = { 16, 24, 42 };
	out_rgba.assign((size_t)W * H * 4, 0);
	for (int yy = 0; yy < H; ++yy)
	{
		float t = (float)yy / (float)std::max(H - 1, 1);
		unsigned r = (unsigned)(bgTop[0] + (bgBot[0] - bgTop[0]) * t);
		unsigned g = (unsigned)(bgTop[1] + (bgBot[1] - bgTop[1]) * t);
		unsigned b = (unsigned)(bgTop[2] + (bgBot[2] - bgTop[2]) * t);
		for (int xx = 0; xx < W; ++xx)
		{
			size_t idx = ((size_t)yy * W + xx) * 4;
			out_rgba[idx + 0] = (std::uint8_t)r;
			out_rgba[idx + 1] = (std::uint8_t)g;
			out_rgba[idx + 2] = (std::uint8_t)b;
			out_rgba[idx + 3] = 255;
		}
	}

	float cy = std::cos(opt.yaw), sy = std::sin(opt.yaw);
	float cp = std::cos(opt.pitch), sp = std::sin(opt.pitch);

	float lx = 0.4f, ly = 0.6f, lz = 0.7f;
	{
		float len = std::sqrt(lx * lx + ly * ly + lz * lz);
		lx /= len; ly /= len; lz /= len;
	}
	const float ambient = 0.46f;
	const float diffuse = 0.58f;

	struct Occ
	{
		int sx, sy, sz;
		std::vector<char> grid;
	};
	std::vector<Occ> occs(sections.size());
	std::vector<DrawV2> draw;
	draw.reserve(60000);
	float minSX = 1e30f, maxSX = -1e30f, minSY = 1e30f, maxSY = -1e30f;

	for (size_t s = 0; s < sections.size(); ++s)
	{
		const VxlSection2& sec = sections[s];
		if (sec.sizeX == 0 || sec.sizeY == 0 || sec.sizeZ == 0)
			return false;
		Occ& o = occs[s];
		o.sx = (int)sec.sizeX; o.sy = (int)sec.sizeY; o.sz = (int)sec.sizeZ;
		o.grid.assign((size_t)o.sx * o.sy * o.sz, 0);
		for (const auto& v : sec.voxels)
			if (v.x < (std::uint32_t)o.sx && v.y < (std::uint32_t)o.sy && v.z < (std::uint32_t)o.sz)
				o.grid[((size_t)v.z * o.sy + v.y) * o.sx + v.x] = 1;

		for (const auto& v : sec.voxels)
		{
			if (v.x >= (std::uint32_t)o.sx || v.y >= (std::uint32_t)o.sy || v.z >= (std::uint32_t)o.sz)
				continue;
			// 模型空间（体素中心）
			float mx = ((float)v.x + 0.5f - (float)o.sx * 0.5f) * unit;
			float my = ((float)v.y + 0.5f - (float)o.sy * 0.5f) * unit;
			float mz = ((float)v.z + 0.5f - (float)o.sz * 0.5f) * unit;

			float rx = mx * cy - my * sy;
			float ry = mx * sy + my * cy;
			float rz = mz;
			float py = ry * cp - rz * sp;
			float pz = ry * sp + rz * cp;

			DrawV2 d;
			d.sx = rx;
			d.sy = -py;
			d.depth = pz;

			// 邻居法向着色
			float nx, ny, nz;
			NeighborNormal(o.grid.data(), o.sx, o.sy, o.sz, (int)v.x, (int)v.y, (int)v.z, nx, ny, nz);
			float diff = nx * lx + ny * ly + nz * lz;
			if (diff < 0.0f) diff = 0.0f;
			unsigned aa = v.rgba & 0xFF;
			// 半透明元素（云、光晕）：用更亮更平的着色，避免被漫反射压暗成灰
			float light;
			if (aa < 255)
				light = 0.74f + 0.26f * diff;
			else
				light = ambient + diffuse * diff;
			if (light > 1.0f) light = 1.0f;

			unsigned rr = (v.rgba >> 24) & 0xFF;
			unsigned gg = (v.rgba >> 16) & 0xFF;
			unsigned bb = (v.rgba >> 8) & 0xFF;
			d.r = (std::uint8_t)(rr * light + 0.5f);
			d.g = (std::uint8_t)(gg * light + 0.5f);
			d.b = (std::uint8_t)(bb * light + 0.5f);
			d.a = (std::uint8_t)aa;

			minSX = std::min(minSX, d.sx); maxSX = std::max(maxSX, d.sx);
			minSY = std::min(minSY, d.sy); maxSY = std::max(maxSY, d.sy);
			draw.push_back(d);
		}
	}

	if (draw.empty())
		return false;

	float spanX = maxSX - minSX;
	float spanY = maxSY - minSY;
	if (spanX < 1e-6f) spanX = 1.0f;
	if (spanY < 1e-6f) spanY = 1.0f;
	int margin = W / 20;
	float avail = (float)(W - 2 * margin);
	float scale = avail / std::max(spanX, spanY) * opt.zoom;
	float offsetX = (float)W * 0.5f - (minSX + maxSX) * 0.5f * scale;
	float offsetY = (float)H * 0.5f - (minSY + maxSY) * 0.5f * scale;

	std::sort(draw.begin(), draw.end(),
		[](const DrawV2& a, const DrawV2& b) { return a.depth < b.depth; });

	int sq = (int)std::ceil(scale);
	if (sq < 1) sq = 1;

	for (const auto& d : draw)
	{
		int px = (int)std::lround(d.sx * scale + offsetX);
		int py = (int)std::lround(d.sy * scale + offsetY);
		float na = d.a / 255.0f;
		for (int dy = 0; dy < sq; ++dy)
		{
			int yy = py + dy;
			if (yy < 0 || yy >= H) continue;
			for (int dx = 0; dx < sq; ++dx)
			{
				int xx = px + dx;
				if (xx < 0 || xx >= W) continue;
				size_t idx = ((size_t)yy * W + xx) * 4;
				if (na >= 1.0f)
				{
					out_rgba[idx + 0] = d.r;
					out_rgba[idx + 1] = d.g;
					out_rgba[idx + 2] = d.b;
					out_rgba[idx + 3] = 255;
				}
				else if (na > 0.0f)
				{
					out_rgba[idx + 0] = (std::uint8_t)(d.r * na + out_rgba[idx + 0] * (1.0f - na) + 0.5f);
					out_rgba[idx + 1] = (std::uint8_t)(d.g * na + out_rgba[idx + 1] * (1.0f - na) + 0.5f);
					out_rgba[idx + 2] = (std::uint8_t)(d.b * na + out_rgba[idx + 2] * (1.0f - na) + 0.5f);
					out_rgba[idx + 3] = 255;
				}
			}
		}
	}

	return true;
}

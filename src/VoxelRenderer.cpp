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

	out_rgba.assign((size_t)W * H * 4, 0);
	for (size_t i = 0; i < (size_t)W * H; ++i)
	{
		out_rgba[i * 4 + 0] = opt.bg[0];
		out_rgba[i * 4 + 1] = opt.bg[1];
		out_rgba[i * 4 + 2] = opt.bg[2];
		out_rgba[i * 4 + 3] = 255;
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

			int ci = v.colorIndex;
			if (ci < 0) ci = 0;
			if (ci > 255) ci = 255;
			d.r = (std::uint8_t)(palette[ci * 3 + 0] * light);
			d.g = (std::uint8_t)(palette[ci * 3 + 1] * light);
			d.b = (std::uint8_t)(palette[ci * 3 + 2] * light);

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

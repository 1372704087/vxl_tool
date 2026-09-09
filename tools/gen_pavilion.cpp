// gen_pavilion.cpp —— 程序化生成"漂浮云海上的东方阁楼"VXL2 体素场景。
//
// 用法: gen_pavilion <out.vxl2>
// 特点:
//   * 单 section，绝对坐标（vxl2 无 255 轴长限制），z 轴向上
//   * RGBA 直存；云海采用 alpha 半透明层叠，渲染时 alpha 混合
//   * 分层材质元数据（stone/wood/tile/rock/grass/cloud/plant/lantern/gold），
//     并附带 PBR 参数（roughness/metalness/emissive 用于后续下游渲染）
//   * 全局元数据：upAxis='z'（3ds Max 语义）
//
// 生成后可用: vxltool render <out.vxl2> -o preview.png --size 760 --yaw -0.55 --pitch -0.42

#include <Vxl2.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
	// RGBA 便捷宏
	constexpr std::uint32_t RGBA(int r, int g, int b, int a = 255)
	{
		return ((std::uint32_t)r << 24) | ((std::uint32_t)g << 16) | ((std::uint32_t)b << 8) | (std::uint32_t)a;
	}

	// 材质索引（用作 layer 元数据）
	enum Mat : std::uint16_t
	{
		M_STONE = 0, M_WOOD, M_TILE, M_ROCK, M_GRASS, M_CLOUD, M_PLANT, M_LANTERN, M_GOLD
	};

	// 确定性的整数哈希（用于云/树木伪随机）
	inline std::uint32_t Hash3(std::int64_t x, std::int64_t y, std::int64_t z, std::uint32_t seed)
	{
		std::uint32_t h = seed;
		std::int64_t v = x * 374761393LL + y * 668265263LL + z * 1274126177LL;
		h ^= (std::uint32_t)(v & 0xFFFFFFFFu);
		h *= 0x85ebca6bu;
		h ^= h >> 13;
		h *= 0xc2b2ae35u;
		h ^= h >> 16;
		return h;
	}

	const int SX = 150, SY = 150, SZ = 92;   // 网格尺寸
	const int C = SX / 2;                    // 中心 x/y

	struct Builder
	{
		VxlSection2 sec;
		std::unordered_set<std::uint32_t> seen;

		Builder()
		{
			sec.name = "PavilionScene";
			sec.sizeX = SX; sec.sizeY = SY; sec.sizeZ = SZ;
			sec.lodLevel = 0;
			seen.reserve(200000);
		}

		inline bool In(int x, int y, int z) const
		{
			return x >= 0 && y >= 0 && z >= 0 && x < SX && y < SY && z < SZ;
		}

		inline void Put(int x, int y, int z, std::uint32_t rgba, std::uint16_t layer = 0)
		{
			if (!In(x, y, z)) return;
			std::uint32_t key = ((std::uint32_t)x * SY + y) * SZ + z;
			if (seen.count(key)) return;   // 去重：同格只保留首个
			seen.insert(key);
			VxlVoxel2 v;
			v.x = (std::uint32_t)x; v.y = (std::uint32_t)y; v.z = (std::uint32_t)z;
			v.rgba = rgba;
			v.normal = 0;   // 0 = 引擎/渲染器自动计算法向
			v.layer = layer;
			sec.voxels.push_back(v);
		}

		// 实心盒（包含边缘）
		void Box(int x0, int x1, int y0, int y1, int z0, int z1, std::uint32_t c, std::uint16_t mat)
		{
			for (int x = x0; x <= x1; ++x)
				for (int y = y0; y <= y1; ++y)
					for (int z = z0; z <= z1; ++z)
						Put(x, y, z, c, mat);
		}

		// 边框盒（空腔，作为墙体/立柱骨架）
		void Frame(int x0, int x1, int y0, int y1, int z0, int z1, int thick,
			std::uint32_t c, std::uint16_t mat)
		{
			for (int i = 0; i < thick; ++i)
			{
				Box(x0 + i, x1 - i, y0 + i, y0 + i, z0, z1, c, mat);
				Box(x0 + i, x1 - i, y1 - i, y1 - i, z0, z1, c, mat);
				Box(x0 + i, x0 + i, y0 + i, y1 - i, z0, z1, c, mat);
				Box(x1 - i, x1 - i, y0 + i, y1 - i, z0, z1, c, mat);
			}
		}

		// 平瓦屋顶：底层沿伸出檐的大平瓦 + 内层升起的平台 + 四角高翘飞檐/四边略翘
		void Roof(int cx, int cy, int zBottom, int eaveHalf, int deckHalf,
			std::uint32_t tile, std::uint32_t tileEdge)
		{
			for (int x = cx - eaveHalf; x <= cx + eaveHalf; ++x)
			{
				for (int y = cy - eaveHalf; y <= cy + eaveHalf; ++y)
				{
					int dx = std::abs(x - cx), dy = std::abs(y - cy);
					bool inDeck = dx <= deckHalf && dy <= deckHalf;
					bool onEdge = std::max(dx, dy) >= eaveHalf - 1;
					// 底层大瓦（出檐）
					Put(x, y, zBottom + 0, onEdge ? tileEdge : tile, M_TILE);
					Put(x, y, zBottom + 1, tile, M_TILE);
					if (inDeck)
					{
						// 内层平台（下一层的地板）
						Put(x, y, zBottom + 2, tile, M_TILE);
					}
				}
			}
			// 四角 + 四边中点
			struct Tip { int x, y; };
			Tip tips[8] = {
				{ cx - eaveHalf, cy - eaveHalf }, { cx + eaveHalf, cy - eaveHalf },
				{ cx - eaveHalf, cy + eaveHalf }, { cx + eaveHalf, cy + eaveHalf },
				{ cx - eaveHalf, cy }, { cx + eaveHalf, cy },
				{ cx, cy - eaveHalf }, { cx, cy + eaveHalf }
			};
			// 四角飞檐明显高翘（东方"翘角"剪影核心）
			for (int i = 0; i < 4; ++i)
				for (int k = 2; k <= 5; ++k)
					Put(tips[i].x, tips[i].y, zBottom + k, tileEdge, M_TILE);
			// 四边中点略翘（檐脊弧度）
			for (int i = 4; i < 8; ++i)
				for (int k = 2; k <= 3; ++k)
					Put(tips[i].x, tips[i].y, zBottom + k, tileEdge, M_TILE);
		}

		// 立柱 + 门廊层：四角柱与檐下横梁、半高栏杆
		void ColumnStorey(int cx, int cy, int half, int z0, int z1,
			int colR, std::uint32_t wood, std::uint32_t railing)
		{
			const int corner[4][2] = { { cx - half, cy - half }, { cx + half, cy - half },
				{ cx - half, cy + half }, { cx + half, cy + half } };
			for (auto& co : corner)
				Box(co[0] - colR, co[0] + colR, co[1] - colR, co[1] + colR, z0, z1, wood, M_WOOD);
			// 檐下横梁
			Frame(cx - half - colR, cx + half + colR, cy - half - colR, cy + half + colR,
				z1 - 1, z1, colR, wood, M_WOOD);
			// 半高栏杆（四边）
			Box(cx - half - colR, cx - half - colR, cy - half - colR, cy + half + colR,
				z0, z0 + 2, railing, M_WOOD);
			Box(cx + half + colR, cx + half + colR, cy - half - colR, cy + half + colR,
				z0, z0 + 2, railing, M_WOOD);
			Box(cx - half - colR, cx + half + colR, cy - half - colR, cy - half - colR,
				z0, z0 + 2, railing, M_WOOD);
			Box(cx - half - colR, cx + half + colR, cy + half + colR, cy + half + colR,
				z0, z0 + 2, railing, M_WOOD);
		}

		// 灯笼（挂在檐下，金色顶盖 + 暖红球身 + 中心高亮发光）
		void Lantern(int x, int y, int zTop)
		{
			for (int k = 0; k < 3; ++k)
				Put(x, y, zTop - k, RGBA(70, 46, 34), M_WOOD);   // 挂绳
			// 灯身：金顶 + 亮红球 + 中心高亮（先放，光晕后补外圈）
			Box(x - 1, x + 1, y - 1, y + 1, zTop - 5 - 1, zTop - 5 - 1, RGBA(232, 178, 66), M_GOLD);  // 金顶盖
			Box(x - 1, x + 1, y - 1, y + 1, zTop - 5, zTop - 3, RGBA(226, 66, 30), M_LANTERN);       // 灯身
			Put(x, y, zTop - 4, RGBA(255, 196, 118), M_LANTERN);       // 中心高亮（发光感）
			Put(x, y, zTop - 3, RGBA(255, 178, 96), M_LANTERN);
			Put(x, y, zTop - 5 - 1 - 1, RGBA(204, 62, 40), M_LANTERN); // 底部坠饰
			// 暖红光晕（半透明，填补外部，让红屋顶下也能看出暖光）
			Box(x - 2, x + 2, y - 2, y + 2, zTop - 3, zTop - 5, RGBA(255, 130, 40, 130), M_LANTERN);
		}

		// 树：树干 + 圆润多层树冠 + 根部土丘
		void Tree(int x, int y, int zBase, int hTrunk, int r, std::uint32_t bark,
			std::uint32_t leaf, std::uint32_t leafEdge)
		{
			// 根部土丘（让树木扎根）
			for (int dx = -1; dx <= 1; ++dx)
				for (int dy = -1; dy <= 1; ++dy)
					if (std::abs(dx) + std::abs(dy) <= 2)
						Put(x + dx, y + dy, zBase, RGBA(108, 92, 60), M_ROCK);
			for (int k = 0; k < hTrunk; ++k)
				Put(x, y, zBase + k, bark, M_WOOD);
			int top = zBase + hTrunk;
			for (int z = top; z < top + r * 2 + 1; ++z)
			{
				int rr = (int)std::ceil((float)r * (1.0f - (float)(z - top) / (float)(r * 2)));
				int rNext = (z < top + r) ? (r + 1 - (z - top)) : (r - (z - top - r));
				if (rNext < 2) rNext = 2;
				for (int dx = -rNext; dx <= rNext; ++dx)
				{
					for (int dy = -rNext; dy <= rNext; ++dy)
					{
						if (dx * dx + dy * dy <= rNext * rNext)
						{
							bool edge = dx * dx + dy * dy > (rNext - 2) * (rNext - 2);
							Put(x + dx, y + dy, z, edge ? leafEdge : leaf, M_PLANT);
						}
					}
				}
				(void)rr;
			}
		}

		// 不规则岩岛：圆台，顶部半径大，越深越小，外缘打碎
		void Island(int zTop, int rTop, int zBottom, int rBottom, std::uint32_t rock,
			std::uint32_t grass)
		{
			for (int z = zBottom; z <= zTop; ++z)
			{
				float t = (float)(z - zBottom) / (float)(zTop - zBottom);
				float r = rBottom + (rTop - rBottom) * t;
				int ir = (int)std::round(r);
				for (int dx = -ir; dx <= ir; ++dx)
					for (int dy = -ir; dy <= ir; ++dy)
					{
						if (dx * dx + dy * dy > (ir + 1) * (ir + 1)) continue;
						// 外缘噪声打碎，形成岩石肌理
						if (std::abs(dx * dx + dy * dy - ir * ir) <= ir &&
							(Hash3(dx, dy, z, 777u) & 31u) < 4u)
							continue;
						std::uint32_t c = rock;
						if (z == zTop || z == zTop - 1)
						{
							// 草顶仅覆盖内圈；外圈留出泥土/岩壁边缘，让岩质感可见
							bool rim = dx * dx + dy * dy > (ir - 4) * (ir - 4);
							auto hn = Hash3(dx, dy, 0, 5u);
							if (rim)
							{
								c = RGBA(88, 70, 56);                       // 泥土悬崖边
								if ((hn & 7u) == 0) c = RGBA(140, 120, 92);  // 偶见浅石
							}
							else
							{
								c = grass;    // 草皮
								if ((hn & 3u) == 1) c = RGBA(104, 148, 70);          // 浅草斑
								else if ((hn & 3u) == 2) c = RGBA(88, 128, 60);      // 深草斑
							}
						}
						else
						{
							auto hn = Hash3(dx, dy, z, 9u);
							if ((hn & 4u) == 0) c = RGBA(78, 70, 62);            // 岩缝
							else if ((hn & 7u) == 5) c = RGBA(112, 100, 88);     // 岩亮面
						}
						int gi = (int)(dx + C), gj = (int)(dy + C);
						Put(gi, gj, z, c, (z == zTop || z == zTop - 1) ? M_GRASS : M_ROCK);
						// 草顶凸起
						if (z == zTop && Hash3(dx, dy, 1, 3u) < 8u)
							Put(gi, gj, z + 1, RGBA(118, 154, 74), M_GRASS);
						// 侧面零星外凸的岩块，增强体积感
						if (z > zTop - 6 && z < zBottom + 4 && Hash3(dx, dy, 2, 4u) < 5u)
						{
							int ox = (dx > 0) ? 1 : (dx < 0 ? -1 : 0);
							int oy = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);
							if (ox || oy) Put(gi + ox, gj + oy, z, RGBA(100, 90, 80), M_ROCK);
						}
					}
			}
		}

		// 云海：半透明的成片云朵，近岛更密更高
		void CloudSea(int zBottom, int zTop, int rMin, int rMax, float core)
		{
			for (int x = 0; x < SX; ++x)
				for (int y = 0; y < SY; ++y)
				{
					int dx = x - C, dy = y - C;
					float r = std::sqrt((float)(dx * dx + dy * dy));
					for (int z = zBottom; z <= zTop; ++z)
					{
						// 距离衰减的密度 + 高度噪声
						auto h = Hash3(x, y, z, 1234u);
						float dens = core * (1.0f - r / (float)rMax);
						if (dens < 0.06f) dens = 0.06f;
						// 云层整体位置：底层平、上层成朵
						float layerA = (float)(z - zBottom) / (float)(zTop - zBottom + 1);
						float thr = 0.38f - 0.04f * dens + (layerA - 0.6f) * 0.5f;
						// 岛正下方更密更高
						if (r < rMin) { thr -= 0.18f; }
						float n = (float)((h % 1000)) / 999.0f;
						if (n > thr)
						{
							int alpha = (int)(160 + 70 * dens * n);
							if (alpha > 240) alpha = 240;
							if (alpha < 150) alpha = 150;
							// 云朵：暖白顶 + 冷蓝底（晨光氛围）
							int hh = (int)((h >> 16) & 15u);
							int warm = (z < zBottom + (zTop - zBottom) / 3) ? 0 : (int)((z - zBottom) * 6);
							int rl = 240 + (hh % 4) - warm / 2;
							if (rl > 252) rl = 252;
							int gl = rl - 1 - warm / 3;
							if (gl < 214) gl = 214;
							if (gl > 250) gl = 250;
							int bl = rl + (warm > 0 ? -4 : 6);
							if (bl > 253) bl = 253;
							if (bl < 228) bl = 228;
							Put(x, y, z, RGBA(rl, gl, bl), M_CLOUD);
						}
					}
				}
		}

		// 远景浮岛 + 浮石（纵深）
		void FarIsland(int cx, int cy, int zBase, int r, std::uint32_t rock, std::uint32_t grass,
			bool withTree)
		{
			for (int z = zBase - r / 2; z <= zBase; ++z)
			{
				float t = (float)(z - (zBase - r / 2)) / (float)(r / 2);
				int ir = (int)(1 + t * (r - 1));
				for (int dx = -ir; dx <= ir; ++dx)
					for (int dy = -ir; dy <= ir; ++dy)
					{
						if (dx * dx + dy * dy > (ir + 1) * (ir + 1)) continue;
						Put(cx + dx, cy + dy, z, z == zBase ? grass : rock,
							z == zBase ? M_GRASS : M_ROCK);
					}
			}
			if (withTree && r >= 4)
				Tree(cx, cy, zBase + 1, 4, 2, RGBA(90, 60, 40), RGBA(70, 112, 58), RGBA(56, 96, 50));
		}
	};

	// 分层材质元数据（用于信息展示 / 下游引擎）
	void AddMaterials(VxlSection2& sec)
	{
		sec.materials =
		{
			{ 0, "stone",   0.6f, 0.05f, 0 },
			{ 1, "wood",    0.8f, 0.0f,  0 },
			{ 2, "tile",    0.75f, 0.1f, 0 },
			{ 3, "rock",    0.9f, 0.0f,  0 },
			{ 4, "grass",   0.85f, 0.0f, 0 },
			{ 5, "cloud",   1.0f, 0.0f,  0 },
			{ 6, "plant",   0.8f, 0.0f,  0 },
			{ 7, "lantern", 0.4f, 0.0f,  0xE62818 },
			{ 8, "gold",    0.3f, 0.9f,  0xD8B048 }
		};
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf("用法: gen_pavilion <out.vxl2>\n");
		return 1;
	}

	Builder b;
	const std::uint32_t
		ROCK    = RGBA(104, 96, 86),
		ROCKD   = RGBA(80, 72, 64),
		GRASS   = RGBA(96, 136, 66),
		GSL     = RGBA(116, 156, 78),
		STONE   = RGBA(228, 223, 209),
		STONED  = RGBA(198, 191, 175),
		WOOD    = RGBA(140, 94, 58),
		WOODD   = RGBA(100, 64, 40),
		TILE    = RGBA(148, 44, 40),
		TILEE   = RGBA(176, 62, 50),
		GOLD    = RGBA(222, 178, 74);

	// 1) 岩岛 + 草顶
	b.Island(40, 30, 20, 13, ROCK, GRASS);

	// 1b) 岛底垂落的根系藤蔓，强化"浮空"感
	for (int k = 0; k < 14; ++k)
	{
		int ang = (int)(Hash3(k, 11, 3, 21u) % 360);
		float rad = 10.f + (float)(Hash3(k, 12, 3, 21u) % 4);
		int ix = C + (int)std::round(rad * std::cos((float)ang * 3.14159f / 180.f));
		int iy = C + (int)std::round(rad * std::sin((float)ang * 3.14159f / 180.f));
		int len = 3 + (int)(Hash3(k, 13, 3, 21u) % 5);
		for (int kk = 0; kk < len; ++kk)
		{
			int zz = 20 - kk;
			b.Put(ix, iy, zz, (kk < len - 1) ? RGBA(70, 84, 58) : RGBA(60, 96, 48), M_PLANT);
		}
	}
	// 底部几块碎裂坠石
	for (int k = 0; k < 5; ++k)
	{
		int ra = C + ((int)(Hash3(k, 5, 6, 7u) % 26) - 13);
		int rb = C + ((int)(Hash3(k, 6, 6, 7u) % 26) - 13);
		int zz = 12 + (int)(Hash3(k, 7, 6, 7u) % 5);
		b.Box(ra, ra, rb, rb, zz, zz + 1, ROCKD, M_ROCK);
	}

	// 2) 台基 + 台阶（+y 前方入口）
	b.Box(C - 13, C + 13, C - 13, C + 13, 41, 44, STONE, M_STONE);
	b.Box(C - 13, C + 13, C - 13, C + 13, 41, 41, STONED, M_STONE);   // 台基腰线
	// 三级台阶（正前方 +y）
	for (int s = 0; s < 3; ++s)
	{
		int z = 41 + s;
		int span = 14 - s;
		b.Box(C - span, C + span, C + 14 + s * 2, C + 14 + s * 2 + 1, z, z, STONE, M_STONE);
		b.Box(C - span, C + span, C + 14 + s * 2 + 1, C + 14 + s * 2 + 1, z + 1, z + 1, STONE, M_STONE);
	}

	// 3) 三层阁楼
	//   一层：柱廊（半高栏杆）+ 出入门 + 平瓦出檐 + 檐下灯笼
	b.ColumnStorey(C, C, 10, 45, 50, 1, WOOD, WOODD);
	// 出入口（+y 门洞，深色双门）
	b.Box(C - 2, C + 2, C + 10, C + 10, 45, 49, WOODD, M_WOOD);
	b.Box(C - 2, C + 2, C + 9, C + 9, 45, 47, RGBA(60, 40, 28), M_WOOD); // 门框阴影
	b.Roof(C, C, 50, 12, 7, TILE, TILEE);
	// 一层檐下灯笼：挂在四向门廊（柱间隙）的飞檐下，正侧都可见
	b.Lantern(C - 1, C + 10, 49);
	b.Lantern(C + 1, C + 10, 49);
	b.Lantern(C - 1, C - 10, 49);
	b.Lantern(C + 1, C - 10, 49);
	b.Lantern(C + 10, C - 1, 49);
	b.Lantern(C - 10, C + 1, 49);

	//   二层
	b.ColumnStorey(C, C, 7, 55, 59, 1, WOOD, WOODD);
	b.Roof(C, C, 59, 9, 5, TILE, TILEE);
	// 二层也挂 2 个小灯笼（正侧）
	b.Lantern(C - 1, C + 7, 58);
	b.Lantern(C + 7, C + 1, 58);

	//   三层（顶层）
	b.ColumnStorey(C, C, 4, 64, 67, 0, WOOD, WOODD);
	b.Roof(C, C, 67, 6, 3, TILE, TILEE);

	//   塔刹（金顶）
	b.Box(C, C, C, C, 70, 72, GOLD, M_GOLD);
	for (int k = 1; k <= 2; ++k)   // 琉璃宝珠
		b.Box(C - k, C + k, C - k, C + k, 72 + k, 72 + k, GOLD, M_GOLD);

	// 4) 植被：四角松 + 平台前矮株
	b.Tree(C - 24, C - 24, 41, 7, 3, RGBA(90, 60, 40), RGBA(66, 108, 56), RGBA(50, 90, 46));
	b.Tree(C + 24, C - 24, 41, 6, 3, RGBA(90, 60, 40), RGBA(76, 118, 62), RGBA(60, 100, 52));
	b.Tree(C - 26, C + 22, 41, 8, 3, RGBA(96, 64, 42), RGBA(70, 112, 58), RGBA(56, 96, 50));
	b.Tree(C + 26, C + 22, 41, 7, 3, RGBA(90, 60, 40), RGBA(62, 104, 54), RGBA(46, 86, 44));
	// 矮灌木
	for (int k = 0; k < 5; ++k)
	{
		int ax = (int)(Hash3(k, 1, 1, 44u) % 22u) - 11;
		int ay = (int)(Hash3(k, 2, 1, 44u) % 22u) - 11;
		int bxp = C + ax, byp = C + (k < 2 ? -12 : 12) + (ay % 6);
		b.Box(bxp - 1, bxp + 1, byp - 1, byp + 1, 42, 42, GSL, M_PLANT);
		b.Box(bxp - 1, bxp + 1, byp - 1, byp + 1, 43, 43, GRASS, M_PLANT);
	}

	// 5) 远景浮岛（纵深）+ 飘浮怪石
	b.FarIsland(C + 40, C + 40, 26, 7, ROCK, GSL, true);
	b.FarIsland(C - 44, C + 8, 24, 5, ROCK, GRASS, false);
	b.FarIsland(C - 6, C - 46, 28, 6, ROCK, GSL, true);
	for (int k = 0; k < 6; ++k)
	{
		int bx = (int)(Hash3(k, 7, 2, 99u) % (SX - 12)) + 6;
		int by = (int)(Hash3(k, 8, 2, 99u) % (SY - 12)) + 6;
		int bz = 50 + (int)(Hash3(k, 9, 2, 99u) % 12);
		if (std::sqrt((float)((bx - C) * (bx - C) + (by - C) * (by - C))) < 22.f) continue;
		b.Box(bx - 1, bx + 1, by - 1, by + 1, bz, bz + 1, ROCKD, M_ROCK);
	}

	// 6) 云海（最底层，半透明成片）
	b.CloudSea(8, 22, 20, 60, 1.0f);

	// 全局元数据：上轴 z、缩放
	VxlGlobal2 global;
	global.hasGlobal = true;
	global.upAxis = 'z';
	global.unitScale = 1.0f;

	AddMaterials(b.sec);

	std::vector<std::uint8_t> data;
	if (!Vxl2::Encode({ b.sec }, data, &global) || data.empty())
	{
		std::printf("错误: VXL2 编码失败\n");
		return 1;
	}
	FILE* f = std::fopen(argv[1], "wb");
	if (!f) { std::printf("错误: 无法写 %s\n", argv[1]); return 1; }
	std::fwrite(data.data(), 1, data.size(), f);
	std::fclose(f);
	std::printf("已生成 %s：体素=%zu，尺寸=%ux%ux%u，文件=%zu 字节\n",
		argv[1], b.sec.voxels.size(), (unsigned)b.sec.sizeX,
		(unsigned)b.sec.sizeY, (unsigned)b.sec.sizeZ, data.size());
	return 0;
}
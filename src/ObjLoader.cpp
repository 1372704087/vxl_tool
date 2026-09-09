#include <ObjLoader.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <locale>
#include <sstream>

namespace
{
	// [修复 C8] 生成一个"始终使用经典的 'C' locale、忽略全局 locale"的字符串流，
	//   保证小数点固定为 '.'，不受 de_DE 等逗号小数点 locale 影响（替代 std::sscanf("%f")）。
	inline std::istringstream LocaleFreeStream(const std::string& s)
	{
		std::istringstream ss(s);
		ss.imbue(std::locale::classic());
		return ss;
	}
	// 把 "1/2/3"、"1//3"、"1" 解析为顶点索引（1 基），返回 0 基索引
	int ParseVertexIndex(const std::string& tok)
	{
		std::string v;
		for (char c : tok)
		{
			if (c == '/')
				break;
			v.push_back(c);
		}
		if (v.empty())
			return -1;
		return std::atoi(v.c_str()) - 1;  // OBJ 是 1 基
	}

	// 解析 "r g b"（0..1 或 0..255），写入 c[3]。
	// 顶点行格式为 "v x y z r g b"，颜色是后 3 个浮点数；
	// 要求确有 6 个浮点数，否则视为无顶点色。
	bool ParseColor(const std::string& s, std::uint8_t c[3])
	{
		// [修复 C8] 用优选的 'C' locale 流解析，避免受全局 locale 小数点影响。
		auto ss = LocaleFreeStream(s);
		float x, y, z, r, g, b;
		if (!(ss >> x >> y >> z >> r >> g >> b))
			return false;
		auto to8 = [](float v) -> std::uint8_t {
			if (v <= 1.0f)
				v *= 255.0f;
			if (v < 0) v = 0;
			if (v > 255) v = 255;
			return (std::uint8_t)(v + 0.5f);
		};
		c[0] = to8(r);
		c[1] = to8(g);
		c[2] = to8(b);
		return true;
	}

	// [修复 C7] 对任意凸/凹多边形做耳切法三角剖分（替代对凹多边形失效的 fan 剖分）。
	// 思路：用 Newell 法向选主轴把多边形投影到 2D，规整为 CCW 后反复剪"耳"。
	// 所有三角形以原始顶点索引写回；成功返回 true 并把结果追加到 out。
	// 退化（重复点/共线导致无法继续剪耳）返回 false，由调用方回退 fan，保证旧行为不回归。
	bool TriangulatePolygon(const std::vector<float>& P, const std::vector<int>& idx,
		std::vector<ObjFace>& out)
	{
		const int n = (int)idx.size();
		if (n < 3) return false;
		if (n == 3)
		{
			out.push_back(ObjFace{ idx[0], idx[1], idx[2] });
			return true;
		}

		// 1) Newell 法向 → 丢弃最大分量所在轴，得到非退化的 2D 投影
		float nx = 0, ny = 0, nz = 0;
		for (int i = 0; i < n; ++i)
		{
			int j = (i + 1) % n;
			const float* a = &P[idx[i] * 3];
			const float* b = &P[idx[j] * 3];
			nx += (a[1] - b[1]) * (a[2] + b[2]);
			ny += (a[2] - b[2]) * (a[0] + b[0]);
			nz += (a[0] - b[0]) * (a[1] + b[1]);
		}
		bool dropX = (std::fabs(nx) >= std::fabs(ny) && std::fabs(nx) >= std::fabs(nz));
		bool dropY = (!dropX && std::fabs(ny) >= std::fabs(nz));

		std::vector<int> ring = idx;                 // 顶点在环中的槽位
		std::vector<float> u(n), v(n);
		for (int i = 0; i < n; ++i)
		{
			const float* p = &P[ring[i] * 3];
			if (dropX)      { u[i] = p[1]; v[i] = p[2]; }
			else if (dropY) { u[i] = p[0]; v[i] = p[2]; }
			else            { u[i] = p[0]; v[i] = p[1]; }
		}

		// 符号面积 -> 负则反向，确保投影后为 CCW（对耳切与包含测试表征一致即可）
		double area2 = 0;
		for (int i = 0; i < n; ++i)
		{
			int j = (i + 1) % n;
			area2 += (double)u[i] * v[j] - (double)u[j] * v[i];
		}
		if (area2 < 0)
		{
			std::reverse(ring.begin(), ring.end());
			std::reverse(u.begin(), u.end());
			std::reverse(v.begin(), v.end());
		}

		auto cross2 = [&](int a, int b, int c) -> double {
			return (double)(u[b] - u[a]) * (v[c] - v[a])
				- (double)(v[b] - v[a]) * (u[c] - u[a]);
		};
		auto insideTri = [&](int a, int b, int c, int p) -> bool {
			const double eps = 1e-9;
			return cross2(a, b, p) >= -eps && cross2(b, c, p) >= -eps && cross2(c, a, p) >= -eps;
		};

		// 2) 耳切：反复寻找"凸 + 内部无其它点"的顶点并剪下
		std::vector<bool> removed(n, false);
		int remaining = n;
		int guard = n * n + 8;   // 防御性上限，防病态输入死循环
		while (remaining > 3 && guard-- > 0)
		{
			bool clipped = false;
			for (int i = 0; i < n; ++i)
			{
				if (removed[i]) continue;
				int prev = i; do { prev = (prev + n - 1) % n; } while (removed[prev]);
				int next = i; do { next = (next + 1) % n; } while (removed[next]);
				if (prev == next) continue;

				const double c = cross2(prev, i, next);
				if (c < 1e-9) continue;              // 凹或共线，非耳

				bool hasPt = false;
				for (int k = 0; k < n; ++k)
				{
					if (k == i || k == prev || k == next || removed[k]) continue;
					if (insideTri(prev, i, next, k)) { hasPt = true; break; }
				}
				if (hasPt) continue;

				out.push_back(ObjFace{ ring[prev], ring[i], ring[next] });
				removed[i] = true;
				--remaining;
				clipped = true;
			}
			if (!clipped) break;   // 无法继续，判定退化
		}

		if (remaining == 3)
		{
			int a = -1, b = -1, c = -1;
			for (int i = 0; i < n; ++i)
			{
				if (removed[i]) continue;
				if (a < 0) a = i; else if (b < 0) b = i; else { c = i; break; }
			}
			out.push_back(ObjFace{ ring[a], ring[b], ring[c] });
			return true;
		}
		return false;
	}
}

bool ObjLoader::Load(const std::string& text, ObjMesh& out)
{
	out.positions.clear();
	out.colors.clear();
	out.faces.clear();
	out.hasVertexColors = false;

	std::istringstream iss(text);
	std::string line;
	while (std::getline(iss, line))
	{
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
			line.pop_back();

		size_t i = 0;
		while (i < line.size() && std::isspace((unsigned char)line[i]))
			++i;
		if (i >= line.size() || line[i] == '#')
			continue;

		size_t j = i;
		while (j < line.size() && !std::isspace((unsigned char)line[j]))
			++j;
		std::string kw = line.substr(i, j - i);

		if (kw == "v")
		{
			// [修复 C8] 顶点坐标解析改用 'C' locale 流（替代 std::sscanf）。
			auto ss = LocaleFreeStream(line.substr(j));
			float x, y, z;
			if (!(ss >> x >> y >> z))
				continue;
			out.positions.push_back(x);
			out.positions.push_back(y);
			out.positions.push_back(z);

			// 可选顶点色：v x y z r g b
			std::uint8_t c[3] = { 200, 200, 200 };
			if (ParseColor(line.substr(j), c))
			{
				out.hasVertexColors = true;
			}
			out.colors.push_back(c[0]);
			out.colors.push_back(c[1]);
			out.colors.push_back(c[2]);
		}
		else if (kw == "f")
		{
			std::vector<int> idx;
			std::istringstream fs(line.substr(j));
			std::string tok;
			while (fs >> tok)
			{
				int vi = ParseVertexIndex(tok);
				if (vi >= 0)
					idx.push_back(vi);
			}
			// [修复 C7] 三角剖分：优先耳切法（支持凹多边形）；退化面回退 fan，保持兼容。
			if (idx.size() >= 3)
			{
				size_t before = out.faces.size();
				if (!TriangulatePolygon(out.positions, idx, out.faces))
				{
					out.faces.resize(before);   // 丢弃耳切法的部分结果，回退 fan
					for (size_t k = 1; k + 1 < idx.size(); ++k)
					{
						ObjFace f;
						f.v0 = idx[0];
						f.v1 = idx[k];
						f.v2 = idx[k + 1];
						out.faces.push_back(f);
					}
				}
			}
		}
		// 其余关键字（vn/vt/o/g/usemtl/mtllib/s 等）忽略
	}

	if (out.positions.empty() || out.faces.empty())
		return false;

	// 计算包围盒
	out.minX = out.maxX = out.positions[0];
	out.minY = out.maxY = out.positions[1];
	out.minZ = out.maxZ = out.positions[2];
	for (size_t i = 3; i < out.positions.size(); i += 3)
	{
		float x = out.positions[i + 0];
		float y = out.positions[i + 1];
		float z = out.positions[i + 2];
		if (x < out.minX) out.minX = x;
		if (x > out.maxX) out.maxX = x;
		if (y < out.minY) out.minY = y;
		if (y > out.maxY) out.maxY = y;
		if (z < out.minZ) out.minZ = z;
		if (z > out.maxZ) out.maxZ = z;
	}

	// 校验顶点索引合法性
	int n = (int)(out.positions.size() / 3);
	for (const auto& f : out.faces)
	{
		if (f.v0 < 0 || f.v0 >= n || f.v1 < 0 || f.v1 >= n || f.v2 < 0 || f.v2 >= n)
			return false;
	}

	return true;
}

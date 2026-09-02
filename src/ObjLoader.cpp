#include <ObjLoader.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace
{
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
		float x, y, z, r, g, b;
		if (std::sscanf(s.c_str(), "%f %f %f %f %f %f", &x, &y, &z, &r, &g, &b) != 6)
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
			float x, y, z;
			if (std::sscanf(line.c_str() + j, "%f %f %f", &x, &y, &z) != 3)
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
			// 三角剖分：fan
			for (size_t k = 1; k + 1 < idx.size(); ++k)
			{
				ObjFace f;
				f.v0 = idx[0];
				f.v1 = idx[k];
				f.v2 = idx[k + 1];
				out.faces.push_back(f);
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

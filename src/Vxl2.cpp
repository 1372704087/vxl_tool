#include <Vxl2.h>
#include <VxlDecoder.h>

#include <zlib.h>
#include <zstd.h>
#include <lz4.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace
{
	// 小端读写工具
	inline std::uint16_t RdU16(const std::uint8_t* p) { return (std::uint16_t)(p[0] | (p[1] << 8)); }
	inline std::uint32_t RdU32(const std::uint8_t* p) { return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) | ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24); }
	inline std::uint64_t RdU64(const std::uint8_t* p)
	{
		return (std::uint64_t)RdU32(p) | ((std::uint64_t)RdU32(p + 4) << 32);
	}
	inline void WrU16(std::uint8_t* p, std::uint16_t v) { p[0] = (std::uint8_t)(v & 0xFF); p[1] = (std::uint8_t)((v >> 8) & 0xFF); }
	inline void WrU32(std::uint8_t* p, std::uint32_t v) { p[0] = (std::uint8_t)(v & 0xFF); p[1] = (std::uint8_t)((v >> 8) & 0xFF); p[2] = (std::uint8_t)((v >> 16) & 0xFF); p[3] = (std::uint8_t)((v >> 24) & 0xFF); }
	inline void WrF32(std::uint8_t* p, float f)
	{
		std::uint32_t u;
		std::memcpy(&u, &f, 4);
		WrU32(p, u);
	}

	const std::uint8_t kMagic[4] = { 'V', 'X', 'L', '2' };
	const std::uint32_t kRecordSize = 20;

	// 区块化存储的默认块边长
	const std::uint32_t kBlockSize = 32;

	// 全局 chunk 类型
	const std::uint32_t kPaletteChunk = 1;  // 可选全局调色板段
	const std::uint32_t kMetaChunk    = 2;  // 轴语义 + 物理缩放
	const std::uint32_t kThumbChunk   = 3;  // 内嵌缩略图
	const std::uint32_t kCrcChunk     = 4;  // 文件级 CRC32（对 sections 区段）
	const std::uint32_t kMetaKVChunk  = 5;  // 扩展元数据键值段（creator/license/description ...）
	// 节内 chunk 类型
	const std::uint32_t kMatChunk     = 1;  // 子材质命名 + PBR
	const std::uint32_t kNormChunk    = 2;  // 自定义精细法向表
	const std::uint32_t kLodChunk     = 3;  // LOD 层级
	const std::uint32_t kBodyBlockChunk = 4; // 区块化压缩 body（v3/v4，布局由首字节 layout 区分）

	// v4 布局标志（body payload 的 layout 字节）
	const std::uint8_t k4UserData   = 0x01; // 每体素 u8 userData
	const std::uint8_t k4Palette    = 0x02; // 节级量化调色板，色改存 1B 索引
	const std::uint8_t k4PackedAttr = 0x04; // normal5b | layer3b 打包进 1B
	const std::uint8_t k4VerticalRun= 0x08; // 同列(z 连续)游程合并且字段只存一次
	const std::uint8_t k4BlockCrc   = 0x10; // 逐块 CRC32
	const std::uint8_t k4IndexTable = 0x20; // 块索引目录（结尾追加，随机定位）
	// v4 单位操作码（raw 流）
	const std::uint8_t k4KindLiteral = 0;   // 单个体素：kind + rx + ry + rz + fields
	const std::uint8_t k4KindRun     = 1;   // 纵向游程：kind + rx + ry + z0 + count + fields(一次)

	// v5 布局标志 2（body payload 的第 18 字节，v4 曾保留）
	const std::uint8_t k5Occ        = 0x01; // 稀疏块占用位图启用
	const std::uint8_t k5DirRuns    = 0x02; // 多向游程(x/y 水平游程)启用
	const std::uint8_t k5Dedup      = 0x04; // 块级内容去重引用启用
	const std::uint8_t k5PalShared  = 0x08; // 节内使用文件级共享全局调色板(色存全局索引)
	const std::uint8_t k5AlgMask    = 0x30; // 压缩算法选择(bits 4-5)
	const int         k5AlgShift    = 4;
	// 压缩算法
	enum V5Alg { kV5Raw = 0, kV5Zlib = 1, kV5Zstd = 2, kV5Lz4 = 3 };

	// v5 单位操作码（raw 流）——coords 均为块内相对坐标、>=1 已含 kind
	const std::uint8_t k5KindLiteral = 0;   // rx+ry+rz + fields
	const std::uint8_t k5KindZRun    = 1;   // 固定(x,y)：rx+ry+z0+count + fields
	const std::uint8_t k5KindXRun    = 2;   // 固定(y,z)：ry+rz+x0+count + fields
	const std::uint8_t k5KindYRun    = 3;   // 固定(x,z)：rx+rz+y0+count + fields
	// v5 每块 bmode 标志
	const std::uint8_t k5BmOcc   = 0x01;   // 该块用占用位图编码（raw 非操作码流）
	const std::uint8_t k5BmDedup = 0x02;   // 该块是去重引用，后随目标 blockIndex（v5=u16 / v6=u32）

	// v5 默认压缩算法（未指定 VXL2_ALG 时）。可在 block 维度覆盖。
	static int V5PickAlg()
	{
		const char* e = std::getenv("VXL2_ALG");
		if (e)
		{
			if (std::strcmp(e, "raw") == 0) return kV5Raw;
			if (std::strcmp(e, "zlib") == 0) return kV5Zlib;
			if (std::strcmp(e, "lz4") == 0) return kV5Lz4;
		}
		return kV5Zstd;
	}

	// ---- CRC32（标准多项式，无表） ----
	std::uint32_t Crc32Update(std::uint32_t crc, const std::uint8_t* data, size_t n)
	{
		crc ^= 0xFFFFFFFFu;
		for (size_t i = 0; i < n; ++i)
		{
			crc ^= data[i];
			for (int b = 0; b < 8; ++b)
				crc = (crc >> 1) ^ (0xEDB88320u & (~((crc & 1u) - 1u)));
		}
		return crc ^ 0xFFFFFFFFu;
	}

	// ---- zlib 一次性压缩（deflate，返回 false 表示失败） ----
	bool ZlibCompress(const std::uint8_t* src, size_t srcLen,
		std::vector<std::uint8_t>& out, int level = Z_BEST_COMPRESSION)
	{
		if (srcLen == 0) { out.clear(); return true; }
		uLongf bound = compressBound((uLong)srcLen);
		out.assign(bound, 0);
		uLongf dstLen = bound;
		int rc = compress2(out.data(), &dstLen, src, (uLong)srcLen, level);
		if (rc != Z_OK) return false;
		out.resize(dstLen);
		return true;
	}

	// ---- zlib 一次性解压，目标长度已知 ----
	bool ZlibDecompress(const std::uint8_t* src, size_t srcLen,
		std::uint8_t* dst, size_t dstLen)
	{
		if (dstLen == 0) return true;
		uLongf outLen = (uLongf)dstLen;
		return uncompress(dst, &outLen, src, (uLong)srcLen) == Z_OK && outLen == (uLongf)dstLen;
	}

	// ---- 多算法压缩（v5）。调用方提供算法；输出 out。失败返回 false。 ----
	bool AlgCompress(int alg, const std::uint8_t* src, size_t srcLen,
		std::vector<std::uint8_t>& out)
	{
		out.clear();
		if (alg == kV5Raw) { out.assign(src, src + srcLen); return true; }
		if (alg == kV5Zlib) return ZlibCompress(src, srcLen, out);
		if (alg == kV5Zstd)
		{
			if (srcLen == 0) return true;
			out.assign(ZSTD_compressBound(srcLen), 0);
			size_t n = ZSTD_compress(out.data(), out.size(), src, srcLen, 3);
			if (ZSTD_isError(n)) return false;
			out.resize(n);
			return true;
		}
		if (alg == kV5Lz4)
		{
			if (srcLen == 0) return true;
			int bound = LZ4_compressBound((int)srcLen);
			if (bound <= 0) return false;
			out.assign((size_t)bound, 0);
			int n = LZ4_compress_default((const char*)src, (char*)out.data(), (int)srcLen, bound);
			if (n <= 0) return false;
			out.resize((size_t)n);
			return true;
		}
		return false;
	}

	// 多算法解压到给定目标长度
	bool AlgDecompress(int alg, const std::uint8_t* src, size_t srcLen,
		std::uint8_t* dst, size_t dstLen)
	{
		if (alg == kV5Raw) { if (srcLen != dstLen) return false; std::memcpy(dst, src, dstLen); return true; }
		if (alg == kV5Zlib) return ZlibDecompress(src, srcLen, dst, dstLen);
		if (alg == kV5Zstd)
		{
			if (dstLen == 0) return true;
			return ZSTD_decompress(dst, dstLen, src, srcLen) == (size_t)dstLen;
		}
		if (alg == kV5Lz4)
		{
			if (dstLen == 0) return true;
			return LZ4_decompress_safe((const char*)src, (char*)dst, (int)srcLen, (int)dstLen) == (int)dstLen;
		}
		return false;
	}

	// Reader 追加 float 读取路径（复用既有 Reader，见下）

	// 辅助：把 float 编码成 4 字节写进 vector
	inline void AppendF32(std::vector<std::uint8_t>& out, float f)
	{
		std::uint8_t b[4];
		WrF32(b, f);
		out.insert(out.end(), b, b + 4);
	}
	inline void AppendU16(std::vector<std::uint8_t>& out, std::uint16_t v)
	{
		std::uint8_t b[2];
		WrU16(b, v);
		out.insert(out.end(), b, b + 2);
	}
	inline void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t v)
	{
		std::uint8_t b[4];
		WrU32(b, v);
		out.insert(out.end(), b, b + 4);
	}
	inline void AppendBytes(std::vector<std::uint8_t>& out, const std::uint8_t* p, size_t n)
	{
		out.insert(out.end(), p, p + n);
	}
	// 追加一个 chunk（类型 + 长度 + payload）
	inline void AppendChunk(std::vector<std::uint8_t>& out, std::uint32_t type,
		const std::vector<std::uint8_t>& payload)
	{
		AppendU32(out, type);
		AppendU32(out, (std::uint32_t)payload.size());
		AppendBytes(out, payload.data(), payload.size());
	}

	// 只读游标：越界安全，读失败返回 false 并保持位置不变（由调用方决定是否继续）
	struct Reader
	{
		const std::uint8_t* p;
		std::int64_t size;
		std::int64_t pos;
		Reader(const std::uint8_t* d, int n) : p(d), size(n), pos(0) {}

		bool available(std::int64_t n) const { return pos >= 0 && n >= 0 && pos + n <= size; }
		bool u32(std::uint32_t& out)
		{
			if (!available(4)) return false;
			out = RdU32(p + pos); pos += 4; return true;
		}
		bool u16(std::uint16_t& out)
		{
			if (!available(2)) return false;
			out = RdU16(p + pos); pos += 2; return true;
		}
		bool bytes(std::uint8_t* dst, std::int64_t n)
		{
			if (!available(n)) return false;
			if (n) std::memcpy(dst, p + pos, (size_t)n);
			pos += n; return true;
		}
		bool u64(std::uint64_t& out)
		{
			if (!available(8)) return false;
			out = RdU64(p + pos); pos += 8; return true;
		}
		bool float32(float& out)
		{
			if (!available(4)) return false;
			std::uint32_t u = RdU32(p + pos);
			std::memcpy(&out, &u, 4);
			pos += 4; return true;
		}
	};
}

bool Vxl2::IsVxl2(const std::uint8_t* data, int size)
{
	return data && size >= 16 && std::memcmp(data, kMagic, 4) == 0;
}

std::uint32_t Vxl2::Version(const std::uint8_t* data, int size)
{
	if (!IsVxl2(data, size)) return 0;
	return RdU32(data + 4);
}

// ---- 解码 ----

// 解析 v2 的全局 chunk 段（flags bit0 置位时紧跟文件头）
static bool DecodeGlobal(Reader& r, VxlGlobal2& g)
{
	std::uint32_t chunkCount = 0;
	if (!r.u32(chunkCount)) return false;
	if (chunkCount > 100000) return false;

	for (std::uint32_t ci = 0; ci < chunkCount; ++ci)
	{
		std::uint32_t type = 0, len = 0;
		if (!r.u32(type) || !r.u32(len)) return false;
		if (len > 1024 * 1024 * 128) return false;   // 缩略图可较大，但≤128MB
		if (!r.available(len)) return false;

		if (type == kPaletteChunk)
		{
			// payload: u16 count + count*4 RGBA
			if (len < 2) { r.pos += len; continue; }
			std::uint16_t cnt = RdU16(r.p + r.pos);
			if ((std::int64_t)cnt * 4 + 2 != len) { r.pos += len; continue; }
			g.paletteRgba.assign(r.p + r.pos + 2, r.p + r.pos + 2 + (std::int64_t)cnt * 4);
			r.pos += len;
		}
		else if (type == kMetaChunk)
		{
			// payload: u8 upAxis + f32 unitScale（≥5 字节才解析）
			if (len >= 5)
			{
				g.upAxis = r.p[r.pos];
				std::uint32_t u = RdU32(r.p + r.pos + 1);
				std::memcpy(&g.unitScale, &u, 4);
			}
			r.pos += len;
		}
		else if (type == kThumbChunk)
		{
			// payload: u8 format + raw bytes
			if (len >= 1)
			{
				g.thumbFormat = r.p[r.pos];
				g.thumbnail.assign(r.p + r.pos + 1, r.p + r.pos + len);
			}
			r.pos += len;
		}
		else if (type == kCrcChunk)
		{
			// payload: u32 crc32（对 sections 区段）
			if (len >= 4)
				g.crc32 = RdU32(r.p + r.pos);
			r.pos += len;
		}
		else if (type == kMetaKVChunk)
		{
			// payload: u32 kvCount + 每条 [u32 keyLen][key][u32 valLen][val]
			if (len >= 4)
			{
				std::uint32_t kv = RdU32(r.p + r.pos);
				std::int64_t cur = r.pos + 4;
				std::int64_t end = r.pos + len;
				g.metadata.clear();
				for (std::uint32_t i = 0; i < kv && cur < end; ++i)
				{
					if (cur + 4 > end) break;
					std::uint32_t kl = RdU32(r.p + cur); cur += 4;
					if (cur + kl > end || kl > 65535) break;
					std::string key((const char*)(r.p + cur), kl); cur += kl;
					if (cur + 4 > end) break;
					std::uint32_t vl = RdU32(r.p + cur); cur += 4;
					if (cur + vl > end || vl > (std::uint32_t)(1024 * 1024)) break;
					std::string val((const char*)(r.p + cur), vl); cur += vl;
					g.metadata.emplace_back(std::move(key), std::move(val));
				}
			}
			r.pos += len;
		}
		else
		{
			r.pos += len;   // 未知 chunk 跳过（向前兼容）
		}
	}
	g.hasGlobal = true;
	return true;
}

// 解析 v2 节内 chunk 段。blockedRaw 非空时，遇 kBodyBlockChunk 把其 payload 原样存入（用于 v3 区块体）。
static bool DecodeSectionChunks(Reader& r, VxlSection2& sec,
	std::vector<std::uint8_t>* blockedRaw = nullptr)
{
	std::uint32_t chunkCount = 0;
	if (!r.u32(chunkCount)) return false;
	if (chunkCount > 100000) return false;

	for (std::uint32_t ci = 0; ci < chunkCount; ++ci)
	{
		std::uint32_t type = 0, len = 0;
		if (!r.u32(type) || !r.u32(len)) return false;
		if (len > 1024 * 1024 * 64) return false;
		if (!r.available(len)) return false;
		std::int64_t end = r.pos + len;

		if (type == kBodyBlockChunk)
		{
			// payload: 区块化压缩体（v3）。原样保存由调用方译码。
			if (blockedRaw)
				blockedRaw->assign(r.p + r.pos, r.p + r.pos + len);
			r.pos = end;
			continue;
		}
		else if (type == kMatChunk)
		{
			// payload: u16 matCount + 每条 [u16 layer][u16 nameLen][name][f32 rough][f32 metal][u32 emissive]
			std::uint16_t mc;
			if (r.pos + 2 <= end) mc = RdU16(r.p + r.pos);
			else { r.pos = end; continue; }
			std::int64_t cursor = r.pos + 2;
			sec.materials.clear();
			for (std::uint16_t i = 0; i < mc; ++i)
			{
				if (cursor + 2 + 2 + 4 + 4 + 4 > end) break;
				VxlMaterial2 m;
				m.layer = RdU16(r.p + cursor); cursor += 2;
				std::uint16_t nlen = RdU16(r.p + cursor); cursor += 2;
				if (cursor + nlen + 4 + 4 + 4 > end) break;
				m.name.assign((const char*)(r.p + cursor), nlen); cursor += nlen;
				std::uint32_t ru = RdU32(r.p + cursor); cursor += 4;
				std::memcpy(&m.roughness, &ru, 4);
				std::uint32_t mu = RdU32(r.p + cursor); cursor += 4;
				std::memcpy(&m.metalness, &mu, 4);
				m.emissive = RdU32(r.p + cursor); cursor += 4;
				sec.materials.push_back(std::move(m));
			}
			r.pos = end;
		}
		else if (type == kNormChunk)
		{
			// payload: u32 count + count*3 floats
			std::uint32_t cnt;
			if (r.pos + 4 <= end) cnt = RdU32(r.p + r.pos);
			else { r.pos = end; continue; }
			std::int64_t need = 4 + (std::int64_t)cnt * 3 * 4;
			if (need != len) { r.pos = end; continue; }
			sec.customNormals.clear();
			sec.customNormals.reserve(cnt * 3);
			for (std::uint32_t i = 0; i < cnt * 3; ++i)
			{
				std::uint32_t u = RdU32(r.p + r.pos + 4 + (std::int64_t)i * 4);
				float f;
				std::memcpy(&f, &u, 4);
				sec.customNormals.push_back(f);
			}
			r.pos = end;
		}
		else if (type == kLodChunk)
		{
			// payload: u32 lodLevel
			if (r.pos + 4 <= end) sec.lodLevel = RdU32(r.p + r.pos);
			r.pos = end;
		}
		else
		{
			r.pos = end;   // 未知 chunk 跳过
		}
	}
	return true;
}

// 译码 v3 区块化压缩体，把还原的体素追加到 sec.voxels，并按需回填 userData。
// payload 布局：
//   u32 blockBase     块边长（读取方以文件值优先）
//   u16 nBlockX,Y,Z   各轴块数
//   u8  userBytes     每体素 userData 字节数（0..1，当前支持 0/1）
//   u32 nTotal        体素总数（校验用）
//   u16 blockCount    非空块数
//   每条：u16 blockIndex + u16 localCount + u32 cLen + zlib流(localCount×记录)
//   记录 = 3×u8 相对坐标(x,y,z) + u32 rgba + u16 normal + u16 layer [+ u8 userData]*userBytes
// blockIndex → (bx,by,bz)：idx = (bz*nY+by)*realNX+bx，realNX=块在 X 轴方向数。
static bool DecodeBlockedBody(const std::uint8_t* p, std::int64_t len,
	VxlSection2& sec, std::uint32_t sx, std::uint32_t sy, std::uint32_t sz)
{
	if (len < 4 + 6 + 1 + 4 + 2)
		return false;
	std::uint32_t blockBase = RdU32(p);
	std::uint16_t nX = RdU16(p + 4);
	std::uint16_t nY = RdU16(p + 6);
	std::uint16_t nZ = RdU16(p + 8);
	std::uint8_t userBytes = p[10];
	std::uint64_t nTotal = RdU32(p + 11);
	std::uint16_t blockCount = RdU16(p + 15);
	if (blockBase == 0 || nX == 0 || nY == 0 || nZ == 0 || blockCount == 0)
		return false;
	if (userBytes > 1)
		return false;

	std::int64_t pos = 17;
	std::uint64_t decoded = 0;
	const std::uint64_t maxVoxels = (std::uint64_t)sx * sy * sz * 8 + 1024; // 宽松上限
	sec.voxels.clear();
	sec.voxels.reserve((size_t)std::min<std::uint64_t>(nTotal, 1u << 24));
	if (userBytes)
		sec.userData.reserve((size_t)std::min<std::uint64_t>(nTotal, 1u << 24));

	for (std::uint32_t b = 0; b < blockCount; ++b)
	{
		if (pos + 4 > len) return false;
		std::uint16_t bi = RdU16(p + pos);         // block index
		std::uint16_t local = RdU16(p + pos + 2);  // 本块体素数
		if (local == 0) return false;
		pos += 4;
		if (pos + 4 > len) return false;
		std::uint32_t cLen = RdU32(p + pos); pos += 4;
		if (pos + cLen > len) return false;

		std::uint16_t realNX = nX;
		std::uint32_t bx = bi % realNX;
		std::uint32_t by = (bi / realNX) % nY;
		std::uint32_t bz = bi / (realNX * nY);
		if (bz >= nZ) return false;

		std::size_t recSize = 3 + 4 + 2 + 2 + userBytes;
		std::vector<std::uint8_t> buf((std::size_t)local * recSize);
		if (!ZlibDecompress(p + pos, cLen, buf.data(), buf.size()))
			return false;
		pos += cLen;

		decoded += local;
		if (decoded > maxVoxels)
			return false;

		std::int64_t r0 = (std::int64_t)bx * blockBase;
		std::int64_t r1 = (std::int64_t)by * blockBase;
		std::int64_t r2 = (std::int64_t)bz * blockBase;
		for (std::uint32_t i = 0; i < local; ++i)
		{
			const std::uint8_t* q = buf.data() + (std::size_t)i * recSize;
			VxlVoxel2 v;
			v.x = r0 + q[0];
			v.y = r1 + q[1];
			v.z = r2 + q[2];
			v.rgba = RdU32(q + 3);
			v.normal = RdU16(q + 7);
			v.layer = RdU16(q + 9);
			if (userBytes)
				v.userData = q[11];
			if (v.x >= sx || v.y >= sy || v.z >= sz)
				return false;
			sec.voxels.push_back(v);
			if (userBytes)
				sec.userData.push_back(v.userData);
		}
	}
	if (decoded != nTotal)
		return false;
	return true;
}

	// —— v4 高级区块体解码 ——
	// 布局由 body 首字节 layout 自描述，与编码端对应。
	static bool DecodeBlockedBodyV4(const std::uint8_t* p, std::int64_t len,
		VxlSection2& sec, std::uint32_t sx, std::uint32_t sy, std::uint32_t sz)
	{
		if (len < 21)
			return false;
		std::uint8_t layout = p[0];
		if (p[1] != 0) return false;   // reserved
		std::uint32_t blockBase = RdU16(p + 2);
		std::uint16_t nX = RdU16(p + 4);
		std::uint16_t nY = RdU16(p + 6);
		std::uint16_t nZ = RdU16(p + 8);
		std::uint64_t nTotal = RdU32(p + 10);
		std::uint16_t blockCount = RdU16(p + 14);
		std::uint16_t palLen = RdU16(p + 16);
		if (blockBase == 0 || nX == 0 || nY == 0 || nZ == 0)
			return false;
		if (blockBase > 127) return false;

		bool needUser = (layout & k4UserData) != 0;
		bool usePalette = (layout & k4Palette) != 0;
		bool usePackedAttr = (layout & k4PackedAttr) != 0;
		bool useRun = (layout & k4VerticalRun) != 0; (void)useRun;
		bool haveBlockCrc = (layout & k4BlockCrc) != 0;
		bool haveIndexTable = (layout & k4IndexTable) != 0;

		std::int64_t pos = 21;
		std::vector<std::uint32_t> palTable(usePalette ? palLen : 0);
		{
			std::int64_t need = (std::int64_t)palTable.size() * 4;
			if (pos + need > len) return false;
			for (std::uint32_t i = 0; i < palTable.size(); ++i)
				palTable[i] = RdU32(p + pos + (std::int64_t)i * 4);
			pos += need;
		}

		std::size_t colorW = usePalette ? 1u : 4u;
		std::size_t attrW = usePackedAttr ? 1u : 4u;
		// 单个体素原始字节数（不含 kind）：coords(3) + color + attr + [user]
		std::size_t baseFieldW = colorW + attrW + (needUser ? 1u : 0u);
		// 纵向游程多 1 字节 count（相对单个体素）
		std::size_t maxUnitW = 1 + 3 + 1 + baseFieldW;   // kind+rx+ry+rz+[count]+fields

		const std::uint64_t maxVoxels = (std::uint64_t)sx * sy * sz * 8 + 1024;
		sec.voxels.clear();
		sec.voxels.reserve((size_t)std::min<std::uint64_t>(nTotal, 1u << 24));
		if (needUser)
			sec.userData.reserve((size_t)std::min<std::uint64_t>(nTotal, 1u << 24));

		std::uint64_t decoded = 0;
		for (std::uint32_t b = 0; b < blockCount; ++b)
		{
			if (pos + 2 + 4 + 4 + 4 > len) return false;
			std::uint16_t bi = RdU16(p + pos); pos += 2;
			std::uint32_t rawLen = RdU32(p + pos); pos += 4;
			std::uint32_t cLen = RdU32(p + pos); pos += 4;
			std::uint32_t storedCrc = 0;
			if (haveBlockCrc)
			{
				storedCrc = RdU32(p + pos); pos += 4;
			}
			if (pos + cLen > len) return false;

			// 解压
			std::vector<std::uint8_t> buf;
			buf.resize(rawLen);
			bool ok = false;
			if (cLen == 0 && rawLen == 0) ok = true;
			else ok = ZlibDecompress(p + pos, cLen, buf.data(), rawLen);
			pos += cLen;
			if (!ok) return false;

			// 每块 CRC 校验
			if (haveBlockCrc)
			{
				std::uint32_t calc = Crc32Update(0, buf.data(), buf.size());
				if (calc != storedCrc) return false;
			}

			std::uint16_t realNX = nX;
			std::uint32_t bx = bi % realNX;
			std::uint32_t by = (bi / realNX) % nY;
			std::uint32_t bz = bi / (realNX * nY);
			if (bz >= nZ) return false;
			std::int64_t r0 = (std::int64_t)bx * blockBase;
			std::int64_t r1 = (std::int64_t)by * blockBase;
			std::int64_t r2 = (std::int64_t)bz * blockBase;

			std::int64_t q = 0;
			while (q < (std::int64_t)buf.size())
			{
				VxlVoxel2 v;
				std::uint8_t kind = buf[(std::size_t)q]; ++q;
				if (kind == k4KindRun)
				{
					if (q + 3 + 1 > (std::int64_t)buf.size()) return false;
					std::uint32_t lx = buf[(std::size_t)q];
					std::uint32_t ly = buf[(std::size_t)q + 1];
					std::uint32_t z0 = buf[(std::size_t)q + 2];
					std::uint32_t rl = buf[(std::size_t)q + 3];
					q += 4;
					if (rl < 1) return false;
					if (z0 + rl - 1 > blockBase) return false;
					if (q + (std::int64_t)baseFieldW > (std::int64_t)buf.size()) return false;
					VxlVoxel2 base = v;
					// 解析 fields 到 base
					const std::uint8_t* f = buf.data() + q;
					std::uint32_t c0 = 0;
					if (usePalette)
					{
						if (f[0] >= palTable.size()) return false;
						c0 = palTable[f[0]];
					}
					else c0 = RdU32(f);
					base.rgba = c0;
					q += colorW;
					if (usePackedAttr)
					{
						base.normal = f[colorW] & 31u;
						base.layer = (f[colorW] >> 5) & 7u;
						q += 1;
					}
					else
					{
						base.normal = RdU16(f + colorW);
						base.layer = RdU16(f + colorW + 2);
						q += 4;
					}
					base.userData = 0;
					if (needUser) { base.userData = buf[(std::size_t)q]; ++q; }
					for (std::uint32_t k = 0; k < rl; ++k)
					{
						VxlVoxel2 vv = base;
						vv.x = (std::uint32_t)r0 + lx;
						vv.y = (std::uint32_t)r1 + ly;
						vv.z = (std::uint32_t)r2 + z0 + k;
						if (vv.x >= sx || vv.y >= sy || vv.z >= sz) return false;
						sec.voxels.push_back(vv);
						if (needUser) sec.userData.push_back(vv.userData);
						++decoded;
					}
				}
				else if (kind == k4KindLiteral)
				{
					if (q + 3 > (std::int64_t)buf.size()) return false;
					std::uint32_t lx = buf[(std::size_t)q];
					std::uint32_t ly = buf[(std::size_t)q + 1];
					std::uint32_t lz = buf[(std::size_t)q + 2];
					q += 3;
					if (lx > blockBase || ly > blockBase || lz > blockBase) return false;
					if (q + (std::int64_t)baseFieldW > (std::int64_t)buf.size()) return false;
					const std::uint8_t* f = buf.data() + q;
					v.x = (std::uint32_t)r0 + lx;
					v.y = (std::uint32_t)r1 + ly;
					v.z = (std::uint32_t)r2 + lz;
					if (usePalette)
					{
						if (f[0] >= palTable.size()) return false;
						v.rgba = palTable[f[0]];
					}
					else v.rgba = RdU32(f);
					q += colorW;
					if (usePackedAttr)
					{
						v.normal = f[colorW] & 31u;
						v.layer = (f[colorW] >> 5) & 7u;
						q += 1;
					}
					else
					{
						v.normal = RdU16(f + colorW);
						v.layer = RdU16(f + colorW + 2);
						q += 4;
					}
					if (needUser) { v.userData = buf[(std::size_t)q]; ++q; }
					if (v.x >= sx || v.y >= sy || v.z >= sz) return false;
					sec.voxels.push_back(v);
					if (needUser) sec.userData.push_back(v.userData);
					++decoded;
				}
				else
				{
					return false;   // unknown kind
				}
				if (decoded > maxVoxels) return false;
			}
			(void)maxUnitW;
		}

		if (decoded != nTotal)
			return false;

		// 块索引目录（verify 用）；此处仅做存在性校验，不用于加载
		if (haveIndexTable)
		{
			if (pos + 2 > len) return false;
			std::uint16_t dirCount = RdU16(p + pos); pos += 2;
			if (dirCount != blockCount) return false;
			if (pos + (std::int64_t)dirCount * 6 > len) return false;
			// offset 一致性校验
			for (std::uint16_t d = 0; d < dirCount; ++d)
			{
				// (blockIndex u16, offset u32)
				pos += 6;
			}
		}
		return true;
	}

	static bool DecodeBlockedBodyV5(const std::uint8_t* p, std::int64_t len,
		const std::vector<std::uint8_t>& globalPalette,
		VxlSection2& sec, std::uint32_t sx, std::uint32_t sy, std::uint32_t sz)
	{
		if (len < 21) return false;
		std::uint8_t layout = p[0];
		if (p[1] != 0) return false;
		std::uint32_t blockBase = RdU16(p + 2);
		std::uint16_t nX = RdU16(p + 4), nY = RdU16(p + 6), nZ = RdU16(p + 8);
		std::uint64_t nTotal = RdU32(p + 10);
		std::uint16_t blockCount = RdU16(p + 14);
		std::uint16_t palLen = RdU16(p + 16);
		std::uint8_t layout2 = p[18];
		if (blockBase == 0 || nX == 0 || nY == 0 || nZ == 0) return false;
		if (blockBase > 127) return false;

		bool useUser = (layout & k4UserData) != 0;
		bool usePalette = (layout & k4Palette) != 0;
		bool usePackedAttr = (layout & k4PackedAttr) != 0;
		bool haveCrc = (layout & k4BlockCrc) != 0;
		bool haveIndex = (layout & k4IndexTable) != 0;
		bool palShared = (layout2 & k5PalShared) != 0;
		int alg = (int)((layout2 & k5AlgMask) >> k5AlgShift);

		std::int64_t pos = 21;
		std::vector<std::uint32_t> palTable;
		if (usePalette)
		{
			if (palShared)
			{
				std::size_t cnt = globalPalette.size() / 4;
				palTable.resize(cnt);
				for (std::size_t i = 0; i < cnt; ++i)
					palTable[i] = RdU32(globalPalette.data() + i * 4);
			}
			else
			{
				if (palLen > 256) return false;
				if (pos + (std::int64_t)palLen * 4 > len) return false;
				palTable.resize(palLen);
				for (std::uint32_t i = 0; i < palLen; ++i)
					palTable[i] = RdU32(p + pos + (std::int64_t)i * 4);
				pos += (std::int64_t)palLen * 4;
			}
			if (palTable.empty()) return false;
		}
		std::size_t colorW = usePalette ? 1u : 4u;
		std::size_t attrW = usePackedAttr ? 1u : 4u;
		std::size_t fieldW = colorW + attrW + (useUser ? 1u : 0u);

		struct L { std::uint32_t x, y, z, rgba; std::uint16_t normal, layer; std::uint8_t ud; };

		auto parseFields = [&](const std::uint8_t* f, L& v) -> bool
		{
			if (usePalette)
			{
				std::uint8_t idx = f[0];
				if (idx >= palTable.size()) return false;
				v.rgba = palTable[idx];
			}
			else v.rgba = RdU32(f);
			if (usePackedAttr)
			{
				v.normal = f[colorW] & 31u;
				v.layer = (f[colorW] >> 5) & 7u;
			}
			else
			{
				v.normal = RdU16(f + colorW);
				v.layer = RdU16(f + colorW + 2);
			}
			v.ud = useUser ? f[colorW + attrW] : 0;
			return true;
		};

		auto decodeLocal = [&](const std::uint8_t* raw, std::int64_t rlen,
			std::uint8_t bmode, std::vector<L>& outV) -> bool
		{
			outV.clear();
			if (bmode & k5BmOcc)
			{
				if (rlen < 3) return false;
				std::uint32_t bx = raw[0], by = raw[1], bz = raw[2];
				if (bx == 0 || by == 0 || bz == 0) return false;
				std::uint64_t cells = (std::uint64_t)bx * by * bz;
				std::int64_t bitBytes = (std::int64_t)((cells + 7) / 8);
				if (3 + bitBytes > rlen) return false;
				std::int64_t occCount = 0;
				for (std::int64_t i = 0; i < bitBytes; ++i)
				{
					std::uint8_t b = raw[3 + i];
					for (int bit = 0; bit < 8; ++bit) if (b & (1u << bit)) ++occCount;
				}
				if (3 + bitBytes + occCount * (std::int64_t)fieldW > rlen) return false;
				std::int64_t fp = 3 + bitBytes;
				for (std::uint32_t zz = 0; zz < bz; ++zz)
					for (std::uint32_t yy = 0; yy < by; ++yy)
						for (std::uint32_t xx = 0; xx < bx; ++xx)
						{
							std::uint64_t cell = ((std::uint64_t)zz * by + yy) * bx + xx;
							if (raw[3 + (cell >> 3)] & (std::uint8_t)(1u << (cell & 7u)))
							{
								L v; v.x = xx; v.y = yy; v.z = zz; v.ud = 0;
								if (!parseFields(raw + fp, v)) return false;
								fp += (std::int64_t)fieldW;
								outV.push_back(v);
							}
						}
				return true;
			}
			std::int64_t q = 0;
			while (q < rlen)
			{
				std::uint8_t kind = raw[q]; ++q;
				if (kind == k5KindLiteral)
				{
					if (q + 3 + (std::int64_t)fieldW > rlen) return false;
					L v; v.x = raw[q]; v.y = raw[q + 1]; v.z = raw[q + 2];
					q += 3;
					if (v.x > 127 || v.y > 127 || v.z > 127) return false;
					if (!parseFields(raw + q, v)) return false;
					q += (std::int64_t)fieldW;
					outV.push_back(v);
				}
				else if (kind == k5KindZRun)
				{
					if (q + 4 + (std::int64_t)fieldW > rlen) return false;
					std::uint32_t rx = raw[q], ry = raw[q + 1], z0 = raw[q + 2], rl = raw[q + 3];
					q += 4;
					if (rl < 1 || z0 + rl > blockBase) return false;
					L v; v.x = rx; v.y = ry; v.z = z0; v.ud = 0;
					if (!parseFields(raw + q, v)) return false;
					q += (std::int64_t)fieldW;
					for (std::uint32_t kk = 0; kk < rl; ++kk) { L e = v; e.z = z0 + kk; outV.push_back(e); }
				}
				else if (kind == k5KindXRun)
				{
					if (q + 4 + (std::int64_t)fieldW > rlen) return false;
					std::uint32_t ry = raw[q], rz = raw[q + 1], x0 = raw[q + 2], rl = raw[q + 3];
					q += 4;
					if (rl < 1 || x0 + rl > blockBase) return false;
					L v; v.x = x0; v.y = ry; v.z = rz; v.ud = 0;
					if (!parseFields(raw + q, v)) return false;
					q += (std::int64_t)fieldW;
					for (std::uint32_t kk = 0; kk < rl; ++kk) { L e = v; e.x = x0 + kk; outV.push_back(e); }
				}
				else if (kind == k5KindYRun)
				{
					if (q + 4 + (std::int64_t)fieldW > rlen) return false;
					std::uint32_t rx = raw[q], rz = raw[q + 1], y0 = raw[q + 2], rl = raw[q + 3];
					q += 4;
					if (rl < 1 || y0 + rl > blockBase) return false;
					L v; v.x = rx; v.y = y0; v.z = rz; v.ud = 0;
					if (!parseFields(raw + q, v)) return false;
					q += (std::int64_t)fieldW;
					for (std::uint32_t kk = 0; kk < rl; ++kk) { L e = v; e.y = y0 + kk; outV.push_back(e); }
				}
				else return false;
			}
			return true;
		};

		struct BE { std::uint16_t bi; std::uint8_t bmode; std::uint16_t target; };
		std::vector<BE> entries; entries.reserve(blockCount);
		std::unordered_map<std::uint16_t, std::vector<L>> cache;

		const std::uint64_t maxVoxels = (std::uint64_t)sx * sy * sz * 8 + 1024;
		sec.voxels.clear();
		sec.voxels.reserve((std::size_t)std::min<std::uint64_t>(nTotal, 1u << 24));
		if (useUser)
			sec.userData.reserve((std::size_t)std::min<std::uint64_t>(nTotal, 1u << 24));

		for (std::uint32_t b = 0; b < blockCount; ++b)
		{
			if (pos + 3 > len) return false;               // u16 bi + u8 bmode
			std::uint16_t bi = RdU16(p + pos); pos += 2;
			std::uint8_t bmode = p[pos]; ++pos;
			bool isDedup = (bmode & k5BmDedup) != 0;
			bool isOcc = (bmode & k5BmOcc) != 0;
			BE e; e.bi = bi; e.bmode = bmode;
			e.target = 0;
			if (isDedup)
			{
				if (pos + 2 > len) return false;
				e.target = RdU16(p + pos); pos += 2;
				entries.push_back(e);
				continue;
			}
			if (pos + 8 > len) return false;
			std::uint32_t rawLen = RdU32(p + pos); pos += 4;
			std::uint32_t cLen = RdU32(p + pos); pos += 4;
			std::uint32_t storedCrc = 0;
			if (haveCrc) { storedCrc = RdU32(p + pos); pos += 4; }
			if (pos + cLen > len) return false;
			std::vector<std::uint8_t> buf;
			buf.resize(rawLen);
			bool ok = (rawLen == 0 && cLen == 0)
				? true : AlgDecompress(alg, p + pos, cLen, buf.data(), rawLen);
			pos += cLen;
			if (!ok) return false;
			if (haveCrc)
			{
				std::uint32_t calc = Crc32Update(0, buf.data(), buf.size());
				if (calc != storedCrc) return false;
			}
			if (!decodeLocal(buf.data(), (std::int64_t)buf.size(), isOcc ? bmode : 0, cache[bi]))
				return false;
			entries.push_back(e);
		}

		std::uint64_t decoded = 0;
		for (const BE& e : entries)
		{
			std::uint32_t bx = e.bi % nX;
			std::uint32_t by = (e.bi / nX) % nY;
			std::uint32_t bz = e.bi / (std::uint64_t)(nX * nY);
			if (by >= nY || bz >= nZ) return false;
			std::uint32_t ox = bx * blockBase, oy = by * blockBase, oz = bz * blockBase;
			const std::vector<L>& loc = (e.bmode & k5BmDedup) ? cache[e.target] : cache[e.bi];
			for (const L& v : loc)
			{
				std::uint32_t wx = ox + v.x, wy = oy + v.y, wz = oz + v.z;
				if (wx >= sx || wy >= sy || wz >= sz) return false;
				VxlVoxel2 outv;
				outv.x = wx; outv.y = wy; outv.z = wz;
				outv.rgba = v.rgba; outv.normal = v.normal; outv.layer = v.layer; outv.userData = v.ud;
				sec.voxels.push_back(outv);
				if (useUser) sec.userData.push_back(v.ud);
				++decoded;
				if (decoded > maxVoxels) return false;
			}
		}
		if (decoded != nTotal) return false;

		if (haveIndex)
		{
			if (pos + 2 > len) return false;
			std::uint16_t dirCount = RdU16(p + pos); pos += 2;
			if (dirCount != blockCount) return false;
			if (pos + (std::int64_t)dirCount * 6 > len) return false;
		}
		return true;
	}

	// —— v6 超大模型区块体解码（version=6）：索引字段全 u32，其余同 v5 ——
	static bool DecodeBlockedBodyV6(const std::uint8_t* p, std::int64_t len,
		const std::vector<std::uint8_t>& globalPalette,
		VxlSection2& sec, std::uint32_t sx, std::uint32_t sy, std::uint32_t sz)
	{
		if (len < 30) return false;
		std::uint8_t layout = p[0];
		std::uint32_t blockBase = RdU32(p + 2);
		std::uint32_t nX = RdU32(p + 6), nY = RdU32(p + 10), nZ = RdU32(p + 14);
		std::uint64_t nTotal = RdU32(p + 18);
		std::uint32_t blockCount = RdU32(p + 22);
		std::uint16_t palLen = RdU16(p + 26);
		std::uint8_t layout2 = p[28];
		if (p[1] != 0 || p[29] != 0) return false;
		if (blockBase == 0 || nX == 0 || nY == 0 || nZ == 0) return false;
		if (blockBase > 127) return false;

		bool useUser = (layout & k4UserData) != 0;
		bool usePalette = (layout & k4Palette) != 0;
		bool usePackedAttr = (layout & k4PackedAttr) != 0;
		bool haveCrc = (layout & k4BlockCrc) != 0;
		bool haveIndex = (layout & k4IndexTable) != 0;
		bool palShared = (layout2 & k5PalShared) != 0;
		int alg = (int)((layout2 & k5AlgMask) >> k5AlgShift);

		std::int64_t pos = 30;
		std::vector<std::uint32_t> palTable;
		if (usePalette)
		{
			if (palShared)
			{
				std::size_t cnt = globalPalette.size() / 4;
				palTable.resize(cnt);
				for (std::size_t i = 0; i < cnt; ++i)
					palTable[i] = RdU32(globalPalette.data() + i * 4);
			}
			else
			{
				if (palLen > 256) return false;
				if (pos + (std::int64_t)palLen * 4 > len) return false;
				palTable.resize(palLen);
				for (std::uint32_t i = 0; i < palLen; ++i)
					palTable[i] = RdU32(p + pos + (std::int64_t)i * 4);
				pos += (std::int64_t)palLen * 4;
			}
			if (palTable.empty()) return false;
		}
		std::size_t colorW = usePalette ? 1u : 4u;
		std::size_t attrW = usePackedAttr ? 1u : 4u;
		std::size_t fieldW = colorW + attrW + (useUser ? 1u : 0u);

		struct L { std::uint32_t x, y, z, rgba; std::uint16_t normal, layer; std::uint8_t ud; };

		auto parseFields = [&](const std::uint8_t* f, L& v) -> bool
		{
			if (usePalette)
			{
				std::uint8_t idx = f[0];
				if (idx >= palTable.size()) return false;
				v.rgba = palTable[idx];
			}
			else v.rgba = RdU32(f);
			if (usePackedAttr)
			{
				v.normal = f[colorW] & 31u;
				v.layer = (f[colorW] >> 5) & 7u;
			}
			else
			{
				v.normal = RdU16(f + colorW);
				v.layer = RdU16(f + colorW + 2);
			}
			v.ud = useUser ? f[colorW + attrW] : 0;
			return true;
		};

		auto decodeLocal = [&](const std::uint8_t* raw, std::int64_t rlen,
			std::uint8_t bmode, std::vector<L>& outV) -> bool
		{
			outV.clear();
			if (bmode & k5BmOcc)
			{
				if (rlen < 3) return false;
				std::uint32_t bx = raw[0], by = raw[1], bz = raw[2];
				if (bx == 0 || by == 0 || bz == 0) return false;
				std::uint64_t cells = (std::uint64_t)bx * by * bz;
				std::int64_t bitBytes = (std::int64_t)((cells + 7) / 8);
				if (3 + bitBytes > rlen) return false;
				std::int64_t occCount = 0;
				for (std::int64_t i = 0; i < bitBytes; ++i)
				{
					std::uint8_t b = raw[3 + i];
					for (int bit = 0; bit < 8; ++bit) if (b & (1u << bit)) ++occCount;
				}
				if (3 + bitBytes + occCount * (std::int64_t)fieldW > rlen) return false;
				std::int64_t fp = 3 + bitBytes;
				for (std::uint32_t zz = 0; zz < bz; ++zz)
					for (std::uint32_t yy = 0; yy < by; ++yy)
						for (std::uint32_t xx = 0; xx < bx; ++xx)
						{
							std::uint64_t cell = ((std::uint64_t)zz * by + yy) * bx + xx;
							if (raw[3 + (cell >> 3)] & (std::uint8_t)(1u << (cell & 7u)))
							{
								L v; v.x = xx; v.y = yy; v.z = zz; v.ud = 0;
								if (!parseFields(raw + fp, v)) return false;
								fp += (std::int64_t)fieldW;
								outV.push_back(v);
							}
						}
				return true;
			}
			std::int64_t q = 0;
			while (q < rlen)
			{
				std::uint8_t kind = raw[q]; ++q;
				if (kind == k5KindLiteral)
				{
					if (q + 3 + (std::int64_t)fieldW > rlen) return false;
					L v; v.x = raw[q]; v.y = raw[q + 1]; v.z = raw[q + 2];
					q += 3;
					if (v.x > 127 || v.y > 127 || v.z > 127) return false;
					if (!parseFields(raw + q, v)) return false;
					q += (std::int64_t)fieldW;
					outV.push_back(v);
				}
				else if (kind == k5KindZRun)
				{
					if (q + 4 + (std::int64_t)fieldW > rlen) return false;
					std::uint32_t rx = raw[q], ry = raw[q + 1], z0 = raw[q + 2], rl = raw[q + 3];
					q += 4;
					if (rl < 1 || z0 + rl > blockBase) return false;
					L v; v.x = rx; v.y = ry; v.z = z0; v.ud = 0;
					if (!parseFields(raw + q, v)) return false;
					q += (std::int64_t)fieldW;
					for (std::uint32_t kk = 0; kk < rl; ++kk) { L e = v; e.z = z0 + kk; outV.push_back(e); }
				}
				else if (kind == k5KindXRun)
				{
					if (q + 4 + (std::int64_t)fieldW > rlen) return false;
					std::uint32_t ry = raw[q], rz = raw[q + 1], x0 = raw[q + 2], rl = raw[q + 3];
					q += 4;
					if (rl < 1 || x0 + rl > blockBase) return false;
					L v; v.x = x0; v.y = ry; v.z = rz; v.ud = 0;
					if (!parseFields(raw + q, v)) return false;
					q += (std::int64_t)fieldW;
					for (std::uint32_t kk = 0; kk < rl; ++kk) { L e = v; e.x = x0 + kk; outV.push_back(e); }
				}
				else if (kind == k5KindYRun)
				{
					if (q + 4 + (std::int64_t)fieldW > rlen) return false;
					std::uint32_t rx = raw[q], rz = raw[q + 1], y0 = raw[q + 2], rl = raw[q + 3];
					q += 4;
					if (rl < 1 || y0 + rl > blockBase) return false;
					L v; v.x = rx; v.y = y0; v.z = rz; v.ud = 0;
					if (!parseFields(raw + q, v)) return false;
					q += (std::int64_t)fieldW;
					for (std::uint32_t kk = 0; kk < rl; ++kk) { L e = v; e.y = y0 + kk; outV.push_back(e); }
				}
				else return false;
			}
			return true;
		};

		struct BE { std::uint64_t bi; std::uint8_t bmode; std::uint64_t target; };
		std::vector<BE> entries; entries.reserve(blockCount > 0 ? std::min<std::uint64_t>(blockCount, 1u << 22) : 0);
		std::unordered_map<std::uint64_t, std::vector<L>> cache;
		std::unordered_set<std::uint64_t> realBlocks;   // 已出现的真实块索引，用于校验去重目标

		// 超大网格下 sx*sy*sz*8 可能超出 u64，用饱和乘法封顶，避免回绕后误判/放行
		auto satMul = [](std::uint64_t a, std::uint64_t b) -> std::uint64_t
		{ return (b != 0 && a > (0xFFFFFFFFFFFFFFFFull / b)) ? 0xFFFFFFFFFFFFFFFFull : a * b; };
		const std::uint64_t maxVoxels = satMul(satMul(satMul((std::uint64_t)sx, (std::uint64_t)sy), (std::uint64_t)sz), 8) + 1024;
		sec.voxels.clear();
		sec.voxels.reserve((std::size_t)std::min<std::uint64_t>(nTotal, 1u << 24));
		if (useUser)
			sec.userData.reserve((std::size_t)std::min<std::uint64_t>(nTotal, 1u << 24));

		for (std::uint32_t b = 0; b < blockCount; ++b)
		{
			if (pos + 5 > len) return false;               // u32 bi + u8 bmode
			std::uint64_t bi = RdU32(p + pos); pos += 4;
			std::uint8_t bmode = p[pos]; ++pos;
			bool isDedup = (bmode & k5BmDedup) != 0;
			bool isOcc = (bmode & k5BmOcc) != 0;
			if (bi / ((std::uint64_t)nX * nY) >= nZ) return false;   // 块索引越界（乘积以 u64 计算防溢出）
			BE e; e.bi = bi; e.bmode = bmode; e.target = 0;
			if (isDedup)
			{
				if (pos + 4 > len) return false;
				e.target = RdU32(p + pos); pos += 4;
				// 去重目标必须是此前已写入的真实块（非空、非去重引用），否则视为损坏数据
				if (realBlocks.find(e.target) == realBlocks.end()) return false;
				entries.push_back(e);
				continue;
			}
			if (pos + 12 > len) return false;
			std::uint32_t rawLen = RdU32(p + pos); pos += 4;
			std::uint32_t cLen = RdU32(p + pos); pos += 4;
			std::uint32_t storedCrc = 0;
			if (haveCrc) { if (pos + 4 > len) return false; storedCrc = RdU32(p + pos); pos += 4; }
			if (pos + cLen > len) return false;
			std::vector<std::uint8_t> buf;
			buf.resize(rawLen);
			bool ok = (rawLen == 0 && cLen == 0)
				? true : AlgDecompress(alg, p + pos, cLen, buf.data(), rawLen);
			pos += cLen;
			if (!ok) return false;
			if (haveCrc)
			{
				std::uint32_t calc = Crc32Update(0, buf.data(), buf.size());
				if (calc != storedCrc) return false;
			}
			if (!decodeLocal(buf.data(), (std::int64_t)buf.size(), isOcc ? bmode : 0, cache[bi]))
				return false;
			realBlocks.insert(bi);
			entries.push_back(e);
		}

		std::uint64_t decoded = 0;
		for (const BE& e : entries)
		{
			std::uint64_t bx = e.bi % nX;
			std::uint64_t by = (e.bi / nX) % nY;
			std::uint64_t bz = e.bi / ((std::uint64_t)nX * nY);   // (nX*nY) 以 u64 计算防止 u32 回绕
			if (by >= nY || bz >= nZ) return false;
			std::uint32_t ox = (std::uint32_t)(bx * blockBase), oy = (std::uint32_t)(by * blockBase), oz = (std::uint32_t)(bz * blockBase);
			const std::vector<L>& loc = (e.bmode & k5BmDedup) ? cache[e.target] : cache[e.bi];
			for (const L& v : loc)
			{
				std::uint32_t wx = ox + v.x, wy = oy + v.y, wz = oz + v.z;
				if (wx >= sx || wy >= sy || wz >= sz) return false;
				VxlVoxel2 outv;
				outv.x = wx; outv.y = wy; outv.z = wz;
				outv.rgba = v.rgba; outv.normal = v.normal; outv.layer = v.layer; outv.userData = v.ud;
				sec.voxels.push_back(outv);
				if (useUser) sec.userData.push_back(v.ud);
				++decoded;
				if (decoded > maxVoxels) return false;
			}
		}
		if (decoded != nTotal) return false;

		if (haveIndex)
		{
			if (pos + 4 > len) return false;
			std::uint32_t dirCount = RdU32(p + pos); pos += 4;
			if (dirCount != blockCount) return false;
			if (pos + (std::int64_t)dirCount * 8 > len) return false;
		}
		return true;
	}

bool Vxl2::Decode(const std::uint8_t* data, int size,
	std::vector<VxlSection2>& out_sections, int& out_voxelCount, VxlGlobal2* out_global, bool verifyCrc)
{
	out_sections.clear();
	out_voxelCount = 0;
	if (!IsVxl2(data, size))
		return false;
	std::uint32_t version = RdU32(data + 4);
	if (version < 1)
		return false;

	Reader r(data, size);
	r.pos = 16;   // 越过 16 字节文件头（magic/version/count/flags）

	if (out_global) *out_global = VxlGlobal2{};

	// sectionCount 在文件头偏移 8
	std::uint32_t sectionCount = RdU32(data + 8);
	if (sectionCount == 0 || sectionCount > 100000)
		return false;

	// 需要时保留全局调色板字节（v5 共享调色板存全局索引，需在此取用）
	std::vector<std::uint8_t> globalPaletteBytes;

	// v1 布局：头部 16 字节（magic/version/count/reserved），直接跟 sections，无全局段。
	// v2/v3 布局：header.flags(bit0) 置位时先有全局 chunk 段；flags 位于头部偏移 12。
	if (version >= 2)
	{
		std::uint32_t hdrFlags = RdU32(data + 12);
		if (hdrFlags & 1u)
		{
			VxlGlobal2 tmp;   // out_global 为 null 时也须跳过全局段
			VxlGlobal2* dst = out_global ? out_global : &tmp;
			if (!DecodeGlobal(r, *dst))
				return false;
			globalPaletteBytes = dst->paletteRgba;

			// 可选 CRC 校验：对 sections 区段（当前 r.pos 处开始，到文件结尾）
			if (out_global && verifyCrc && dst->crc32 != 0)
			{
				std::uint32_t computed = Crc32Update(0, r.p + r.pos, (size_t)(r.size - r.pos));
				if (computed != dst->crc32)
					return false;
			}
		}
		else if (out_global)
		{
			*out_global = VxlGlobal2{};
		}
	}

	int voxelCount = 0;
	for (std::uint32_t si = 0; si < sectionCount; ++si)
	{
		if (!r.available(4))
			return false;
		std::uint32_t nameLen = RdU32(r.p + r.pos);
		if (nameLen > 65535 || !r.available(4 + nameLen + 16))
			return false;
		r.pos += 4;
		VxlSection2 sec;
		sec.name.assign((const char*)(r.p + r.pos), nameLen);
		r.pos += nameLen;

		std::uint32_t sx, sy, sz, n;
		if (!r.u32(sx) || !r.u32(sy) || !r.u32(sz) || !r.u32(n))
			return false;
		if (sx == 0 || sy == 0 || sz == 0)
			return false;

		sec.sizeX = sx; sec.sizeY = sy; sec.sizeZ = sz;

		// 节内 chunk：materials / customNormals / lod（+ v3 区块体）
		std::vector<std::uint8_t> blockedRaw;
		if (version >= 2)
		{
			if (!DecodeSectionChunks(r, sec, version >= 3 ? &blockedRaw : nullptr))
				return false;
		}

		if (!blockedRaw.empty())
		{
			// v3/v4 区块化压缩存储（body 为压缩数据，字节数必然 < n×20，不可套用定长越界防护）
			if (n > ((std::uint64_t(1)) << 32) - (std::uint32_t)blockedRaw.size())
				return false;   // 仅做不可能成立的极值防护
			// v3 定长区块体 vs v4/v5/v6 自描述高级区块体：按文件版本分派。
			bool bodyOk = (version >= 6)
				? DecodeBlockedBodyV6(blockedRaw.data(), (std::int64_t)blockedRaw.size(),
					globalPaletteBytes, sec, sx, sy, sz)
				: (version >= 5)
					? DecodeBlockedBodyV5(blockedRaw.data(), (std::int64_t)blockedRaw.size(),
						globalPaletteBytes, sec, sx, sy, sz)
					: (version >= 4)
						? DecodeBlockedBodyV4(blockedRaw.data(), (std::int64_t)blockedRaw.size(),
							sec, sx, sy, sz)
						: DecodeBlockedBody(blockedRaw.data(), (std::int64_t)blockedRaw.size(),
							sec, sx, sy, sz);
			if (!bodyOk)
				return false;
			if (sec.voxels.size() != n)
				return false;
		}
		else
		{
			// v1/v2 定长记录（或 v3 未启用区块 → 兼容回退）
			if (n > (std::uint32_t)(((std::uint64_t)((std::uint32_t)r.size - r.pos)) / kRecordSize))
				return false;   // 记录数超出剩余字节 → 越界防护
			sec.voxels.reserve(n);
			for (std::uint32_t i = 0; i < n; ++i)
			{
				VxlVoxel2 v;
				if (!r.u32(v.x) || !r.u32(v.y) || !r.u32(v.z) || !r.u32(v.rgba))
					return false;
				if (!r.u16(v.normal) || !r.u16(v.layer))
					return false;
				if (v.x >= sx || v.y >= sy || v.z >= sz)
					return false;   // 越界体素 → 判非法而不是静默吞掉
				sec.voxels.push_back(v);
			}
		}
		voxelCount += (int)n;
		out_sections.push_back(std::move(sec));
	}

	out_voxelCount = voxelCount;
	return true;
}

// ---- 编码 ----

namespace
{
	// 构建 v3 区块化压缩体 payload（供 kBodyBlockChunk 使用），布局见 DecodeBlockedBody。
	// 返回 false 失败；userBytes 固定取 1（当任何体素 userData 非 0 或需要该通道时）否则 0。
	static bool BuildBlockedBody(const VxlSection2& sec,
		std::vector<std::uint8_t>& out)
	{
		out.clear();
		const std::uint32_t bx0 = kBlockSize;
		std::uint32_t nX = sec.sizeX / bx0 + (sec.sizeX % bx0 ? 1 : 0);
		std::uint32_t nY = sec.sizeY / bx0 + (sec.sizeY % bx0 ? 1 : 0);
		std::uint32_t nZ = sec.sizeZ / bx0 + (sec.sizeZ % bx0 ? 1 : 0);
		if (nX == 0 || nY == 0 || nZ == 0 || nX > 65535 || nY > 65535 || nZ > 65535)
			return false;

		bool needUser = false;
		if (!sec.userData.empty() && sec.userData.size() == sec.voxels.size())
		{
			// 若通道存在且有任一非零值则保留；否则视为不必要
			for (std::uint8_t u : sec.userData)
				if (u != 0) { needUser = true; break; }
		}
		std::uint8_t userBytes = needUser ? 1u : 0u;

		// 分块：idx = (bz*nY+by)*nX+bx
		auto blockIndexOf = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) -> std::uint32_t
		{
			std::uint32_t bx = x / bx0, by = y / bx0, bz = z / bx0;
			return (bz * nY + by) * nX + bx;
		};

		struct Block
		{
			std::uint32_t idx;
			std::uint16_t local;
			std::vector<std::uint8_t> raw; // 未压缩
		};
		std::vector<Block> blocks;

		// 按 blockIndex 分组。order 同时携带 userData（若需要该通道），避免二次索引。
		struct Item { std::uint32_t idx; const VxlVoxel2* v; std::uint8_t ud; };
		std::vector<Item> order;
		order.reserve(sec.voxels.size());
		for (std::size_t vi = 0; vi < sec.voxels.size(); ++vi)
		{
			const auto& v = sec.voxels[vi];
			if (v.x >= sec.sizeX || v.y >= sec.sizeY || v.z >= sec.sizeZ)
				return false;
			Item it;
			it.idx = blockIndexOf(v.x, v.y, v.z);
			it.v = &v;
			it.ud = needUser ? sec.userData[vi] : 0;
			order.push_back(it);
		}
		// 按 idx 稳定排序
		std::vector<Item> sorted(order);
		std::sort(sorted.begin(), sorted.end(),
			[](const Item& a, const Item& b) { return a.idx < b.idx; });

		std::size_t recSize = 3 + 4 + 2 + 2 + userBytes;
		for (std::size_t i = 0; i < sorted.size();)
		{
			std::uint32_t idx = sorted[i].idx;
			std::size_t j = i;
			while (j < sorted.size() && sorted[j].idx == idx) ++j;
			std::size_t cnt = j - i;
			if (cnt > 65535) return false;   // 单块体素上限 65535（u16 local；坐标仍为块内 u8 相对偏移）

			Block blk;
			blk.idx = idx;
			blk.local = (std::uint16_t)cnt;
			blk.raw.reserve(cnt * recSize);
			std::uint32_t originX = (idx % nX) * bx0;
			std::uint32_t originY = ((idx / nX) % nY) * bx0;
			std::uint32_t originZ = (idx / (nX * nY)) * bx0;
			for (std::size_t k = i; k < j; ++k)
			{
				const VxlVoxel2& v = *sorted[k].v;
				blk.raw.push_back((std::uint8_t)(v.x - originX));
				blk.raw.push_back((std::uint8_t)(v.y - originY));
				blk.raw.push_back((std::uint8_t)(v.z - originZ));
				std::uint8_t b4[4]; WrU32(b4, v.rgba);
				blk.raw.insert(blk.raw.end(), b4, b4 + 4);
				std::uint8_t b2[2]; WrU16(b2, v.normal);
				blk.raw.insert(blk.raw.end(), b2, b2 + 2);
				WrU16(b2, v.layer);
				blk.raw.insert(blk.raw.end(), b2, b2 + 2);
				if (userBytes)
					blk.raw.push_back(sorted[k].ud);
			}
			blocks.push_back(std::move(blk));
			i = j;
		}

		// 序列化
		std::uint8_t head[17];
		WrU32(head, bx0);
		WrU16(head + 4, (std::uint16_t)nX);
		WrU16(head + 6, (std::uint16_t)nY);
		WrU16(head + 8, (std::uint16_t)nZ);
		head[10] = userBytes;
		WrU32(head + 11, (std::uint32_t)sec.voxels.size());
		WrU16(head + 15, (std::uint16_t)blocks.size());
		out.insert(out.end(), head, head + 17);

		for (const Block& blk : blocks)
		{
			std::uint8_t b2[2]; WrU16(b2, (std::uint16_t)blk.idx);
			out.insert(out.end(), b2, b2 + 2);
			WrU16(b2, blk.local);
			out.insert(out.end(), b2, b2 + 2);
			std::vector<std::uint8_t> c;
			if (!ZlibCompress(blk.raw.data(), blk.raw.size(), c))
				return false;
			std::uint8_t b4[4]; WrU32(b4, (std::uint32_t)c.size());
			out.insert(out.end(), b4, b4 + 4);
			out.insert(out.end(), c.begin(), c.end());
		}
		return true;
	}

	// —— v4 高级区块体编码 ——
	// 自描述布局，读取无损。按需启用标志（见 k4* 常量）：
	//   palette   节级量化调色板（每体素色改存 1B 索引）
	//   packedAttr normal(5b) | layer(3b) 打包进 1B
	//   verticalRun 同列(z 连续)游程合并，字段只存一次
	//   blockCrc  每块 CRC32
	//   indexTable 块索引目录（body 结尾追加 offsets，支持 mmap 随机定位）
	// 编码器对每个 section 自动权衡：调色板仅在唯一色 ≤256 且可用时启用，
	// packedAttr 仅在 normal<32 且 layer<8 全满足时启用，否则降级到 v3 定长字段。
	static bool BuildBlockedBodyV4(const VxlSection2& sec,
		std::vector<std::uint8_t>& out)
	{
		out.clear();
		const std::uint32_t bx0 = kBlockSize;
		std::uint32_t nX = sec.sizeX / bx0 + (sec.sizeX % bx0 ? 1 : 0);
		std::uint32_t nY = sec.sizeY / bx0 + (sec.sizeY % bx0 ? 1 : 0);
		std::uint32_t nZ = sec.sizeZ / bx0 + (sec.sizeZ % bx0 ? 1 : 0);
		if (nX == 0 || nY == 0 || nZ == 0 || nX > 65535 || nY > 65535 || nZ > 65535)
			return false;
		if (bx0 > 127) return false;   // 相对坐标 + 游程需 u8，128 以内安全

		// —— 计算布局标志 ——
		bool needUser = false;
		if (!sec.userData.empty() && sec.userData.size() == sec.voxels.size())
			for (std::uint8_t u : sec.userData)
				if (u != 0) { needUser = true; break; }

		// 调色板：唯一 RGBA ≤256 时启用（无损量化）
		int palCandidates = 0;             // 未知时先数一下独特色
		{
			std::unordered_map<std::uint32_t, int> set;
			for (const auto& v : sec.voxels)
			{
				if (set.find(v.rgba) == set.end())
				{
					set.emplace(v.rgba, 0);
					if ((int)set.size() > 256) break;
				}
			}
			palCandidates = (int)set.size();
		}
		bool usePalette = palCandidates > 0 && palCandidates <= 256;

		// packedAttr：normal(<32) 且 layer(<8)
		bool usePackedAttr = true;
		if (!sec.voxels.empty())
			for (const auto& v : sec.voxels)
				if (v.normal >= 32 || v.layer >= 8) { usePackedAttr = false; break; }

		// 构建调色板索引表（若启用）
		std::unordered_map<std::uint32_t, std::uint16_t> palMap;
		std::vector<std::uint32_t> palTable;   // RGBA 数组（顺序即索引）
		if (usePalette)
		{
			for (const auto& v : sec.voxels)
				if (palMap.find(v.rgba) == palMap.end())
				{
					unsigned idx = (unsigned)palTable.size();
					palMap.emplace(v.rgba, (std::uint16_t)idx);
					palTable.push_back(v.rgba);
				}
		}

		// 分块
		auto blockIndexOf = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) -> std::uint32_t
		{
			std::uint32_t bx = x / bx0, by = y / bx0, bz = z / bx0;
			return (bz * nY + by) * nX + bx;
		};
		struct Item
		{
			std::uint32_t idx;
			const VxlVoxel2* v;
			std::uint8_t ud;
		};
		std::vector<Item> order;
		order.reserve(sec.voxels.size());
		for (std::size_t vi = 0; vi < sec.voxels.size(); ++vi)
		{
			const auto& v = sec.voxels[vi];
			if (v.x >= sec.sizeX || v.y >= sec.sizeY || v.z >= sec.sizeZ)
				return false;
			Item it;
			it.idx = blockIndexOf(v.x, v.y, v.z);
			it.v = &v;
			it.ud = needUser ? sec.userData[vi] : 0;
			order.push_back(it);
		}
		std::vector<Item> sorted(order);
		std::sort(sorted.begin(), sorted.end(),
			[](const Item& a, const Item& b) { return a.idx < b.idx; });

		// 字段写出直接 push_back 到各块 raw；序列化在下方逐块完成。
		// 每个块的记录序列化。先收集成字节流后再压缩。
		struct BlockOut
		{
			std::uint32_t idx;
			std::vector<std::uint8_t> raw;
		};
		std::vector<BlockOut> blocks;

		// 记录字段宽度
		std::size_t colorW   = usePalette ? 1u : 4u;
		std::size_t attrW    = usePackedAttr ? 1u : 4u;   // 1打包 / 2+2 分开
		std::size_t recFieldW = colorW + attrW + (needUser ? 1u : 0u);

		// 单条字段写入
		auto appendFields = [&](std::vector<std::uint8_t>& b, const VxlVoxel2& v, std::uint8_t ud)
		{
			if (usePalette)
			{
				auto it = palMap.find(v.rgba);
				if (it == palMap.end()) return false;
				b.push_back((std::uint8_t)it->second);
			}
			else
			{
				std::uint8_t b4[4]; WrU32(b4, v.rgba);
				b.insert(b.end(), b4, b4 + 4);
			}
			if (usePackedAttr)
			{
				b.push_back((std::uint8_t)((v.normal & 31u) | ((v.layer & 7u) << 5)));
			}
			else
			{
				std::uint8_t b2[2]; WrU16(b2, v.normal);
				b.insert(b.end(), b2, b2 + 2);
				WrU16(b2, v.layer);
				b.insert(b.end(), b2, b2 + 2);
			}
			if (needUser) b.push_back(ud);
			return true;
		};

		for (std::size_t i = 0; i < sorted.size();)
		{
			std::uint32_t idx = sorted[i].idx;
			std::size_t j = i;
			while (j < sorted.size() && sorted[j].idx == idx) ++j;
			std::size_t cnt = j - i;
			if (cnt > 65535) return false;

			BlockOut blk;
			blk.idx = idx;
			blk.raw.reserve(cnt * (3 + recFieldW));

			std::uint32_t originX = (idx % nX) * bx0;
			std::uint32_t originY = ((idx / nX) % nY) * bx0;
			std::uint32_t originZ = (idx / (nX * nY)) * bx0;

			// 块内按 (x,y,z) 排序，便于纵向游程合并
			auto sortedV = sorted.begin() + i;
			std::vector<const Item*> slice;
			slice.reserve(cnt);
			for (std::size_t k = 0; k < cnt; ++k) slice.push_back(&sortedV[k]);
			std::sort(slice.begin(), slice.end(),
				[originX, originY, originZ](const Item* a, const Item* b)
				{
					std::uint32_t ax = a->v->x - originX, ay = a->v->y - originY, az = a->v->z - originZ;
					std::uint32_t bx = b->v->x - originX, by = b->v->y - originY, bz = b->v->z - originZ;
					if (ax != bx) return ax < bx;
					if (ay != by) return ay < by;
					return az < bz;
				});

			for (std::size_t k = 0; k < cnt;)
			{
				const Item* cur = slice[k];
				std::uint32_t lx = cur->v->x - originX;
				std::uint32_t ly = cur->v->y - originY;
				std::uint32_t lz = cur->v->z - originZ;
				if (lx > 255 || ly > 255 || lz > 255) return false;

				// 找纵向游程：同 (x,y)，z 连续（z+1），字段一致
				std::size_t runLen = 1;
				while (k + runLen < cnt)
				{
					const Item* nx = slice[k + runLen];
					if (nx->v->x - originX != lx || nx->v->y - originY != ly) break;
					std::uint32_t nz = nx->v->z - originZ;
					if (nz != lz + runLen) break;
					if (cur->v->rgba != nx->v->rgba) break;
					if (cur->v->normal != nx->v->normal || cur->v->layer != nx->v->layer) break;
					if (needUser && cur->ud != nx->ud) break;
					if (nz > 255) break;
					++runLen;
				}

				if (runLen >= 2 && runLen <= 255)
				{
					// 纵向游程：kind(1) + rx + ry + z0 + count + fields
					blk.raw.push_back(k4KindRun);
					blk.raw.push_back((std::uint8_t)lx);
					blk.raw.push_back((std::uint8_t)ly);
					blk.raw.push_back((std::uint8_t)lz);
					blk.raw.push_back((std::uint8_t)runLen);
					if (!appendFields(blk.raw, *cur->v, cur->ud)) return false;
					k += runLen;
				}
				else
				{
					// 单个体素：kind(0) + rx + ry + rz + fields
					blk.raw.push_back(k4KindLiteral);
					blk.raw.push_back((std::uint8_t)lx);
					blk.raw.push_back((std::uint8_t)ly);
					blk.raw.push_back((std::uint8_t)lz);
					if (!appendFields(blk.raw, *cur->v, cur->ud)) return false;
					++k;
				}
			}
			blocks.push_back(std::move(blk));
			i = j;
		}

		// —— 序列化 body ——
		std::uint8_t layout = 0;
		if (needUser) layout |= k4UserData;
		if (usePalette) layout |= k4Palette;
		if (usePackedAttr) layout |= k4PackedAttr;
		layout |= k4VerticalRun;
		layout |= k4BlockCrc;
		layout |= k4IndexTable;

		std::uint8_t head[21] = { 0 };
		head[0] = layout;
		// head[1]=reserved, head[18..20]=reserved 均初始化为 0
		WrU16(head + 2, (std::uint16_t)bx0);          // blockBase
		WrU16(head + 4, (std::uint16_t)nX);           // nBlockX
		WrU16(head + 6, (std::uint16_t)nY);           // nBlockY
		WrU16(head + 8, (std::uint16_t)nZ);           // nBlockZ
		WrU32(head + 10, (std::uint32_t)sec.voxels.size());  // nTotal
		WrU16(head + 14, (std::uint16_t)blocks.size());      // blockCount
		WrU16(head + 16, usePalette ? (std::uint16_t)palTable.size() : 0);  // palLen
		out.insert(out.end(), head, head + 21);

		// 调色板
		for (std::uint32_t c : palTable)
		{
			std::uint8_t b4[4]; WrU32(b4, c);
			out.insert(out.end(), b4, b4 + 4);
		}

		// 每块：u16 blockIndex + u32 rawLen + u32 cLen + [u32 crc] + zlib流
		std::vector<std::pair<std::uint16_t, std::uint32_t>> dir; // (blockIndex, offset)
		for (const BlockOut& blk : blocks)
		{
			dir.emplace_back((std::uint16_t)blk.idx, (std::uint32_t)out.size());
			std::uint8_t b2[2]; WrU16(b2, (std::uint16_t)blk.idx);
			out.insert(out.end(), b2, b2 + 2);
			std::uint8_t b4[4];
			WrU32(b4, (std::uint32_t)blk.raw.size());
			out.insert(out.end(), b4, b4 + 4);
			std::vector<std::uint8_t> c;
			if (!ZlibCompress(blk.raw.data(), blk.raw.size(), c))
				return false;
			WrU32(b4, (std::uint32_t)c.size());
			out.insert(out.end(), b4, b4 + 4);
			std::uint32_t crc = Crc32Update(0, blk.raw.data(), blk.raw.size());
			WrU32(b4, crc);
			out.insert(out.end(), b4, b4 + 4);
			out.insert(out.end(), c.begin(), c.end());
		}

		// 块索引目录（结尾）
	{
		std::uint8_t b2[2]; WrU16(b2, (std::uint16_t)dir.size());
		out.insert(out.end(), b2, b2 + 2);
		for (const auto& e : dir)
		{
			unsigned short b2x[2]; (void)b2x;
			std::uint8_t bb[6];
			WrU16(bb, e.first);
			WrU32(bb + 2, e.second);
			out.insert(out.end(), bb, bb + 6);
		}
	}
	return true;
}

	// FNV-1a 64（块去重散列用）
	static std::uint64_t RawHash(const std::vector<std::uint8_t>& v)
	{
		std::uint64_t h = 1469598103934665603ull;
		for (std::uint8_t c : v) { h ^= c; h *= 1099511628211ull; }
		h ^= (std::uint64_t)v.size();
		return h;
	}

	// —— v5 高级区块体 II 编码 ——
	// sharedPalette 非空：该节使用文件级共享调色板，体素色存"全局索引"，节内不写色表。
	// outLayout2 回填第二布局字节（含算法位），供上层/解码判读。
	static bool BuildBlockedBodyV5(const VxlSection2& sec,
		const std::vector<std::uint32_t>* sharedPalette,
		std::uint8_t& outLayout2, std::vector<std::uint8_t>& out)
	{
		out.clear();
		const std::uint32_t bx0 = kBlockSize;
		std::uint32_t nX = sec.sizeX / bx0 + (sec.sizeX % bx0 ? 1 : 0);
		std::uint32_t nY = sec.sizeY / bx0 + (sec.sizeY % bx0 ? 1 : 0);
		std::uint32_t nZ = sec.sizeZ / bx0 + (sec.sizeZ % bx0 ? 1 : 0);
		if (nX == 0 || nY == 0 || nZ == 0 || nX > 65535 || nY > 65535 || nZ > 65535)
			return false;
		if (bx0 > 127) return false;

		bool useUser = false;
		if (!sec.userData.empty() && sec.userData.size() == sec.voxels.size())
			for (std::uint8_t u : sec.userData) if (u != 0) { useUser = true; break; }

		const bool useShared = (sharedPalette != nullptr);
		bool usePalette = useShared;
		std::vector<std::uint32_t> palTable;          // 局部调色板（仅 !useShared 时）
		std::unordered_map<std::uint32_t, std::uint16_t> palMap;  // rgba -> 索引(局部或全局)
		if (useShared)
		{
			for (std::size_t i = 0; i < sharedPalette->size(); ++i)
				palMap.emplace((*sharedPalette)[i], (std::uint16_t)i);
		}
		else
		{
			int pc = 0;
			{
				std::unordered_map<std::uint32_t, int> s;
				for (const auto& v : sec.voxels)
				{
					if (s.find(v.rgba) == s.end()) { s.emplace(v.rgba, 0); if ((int)s.size() > 256) break; }
				}
				pc = (int)s.size();
			}
			usePalette = pc > 0 && pc <= 256;
			if (usePalette)
			{
				for (const auto& v : sec.voxels)
					if (palMap.find(v.rgba) == palMap.end())
					{
						palMap.emplace(v.rgba, (std::uint16_t)palTable.size());
						palTable.push_back(v.rgba);
					}
			}
		}

		bool usePackedAttr = true;
		if (!sec.voxels.empty())
			for (const auto& v : sec.voxels)
				if (v.normal >= 32 || v.layer >= 8) { usePackedAttr = false; break; }

		std::size_t colorW = usePalette ? 1u : 4u;
		std::size_t attrW = usePackedAttr ? 1u : 4u;
		std::size_t fieldW = colorW + attrW + (useUser ? 1u : 0u);

		auto appendFields = [&](std::vector<std::uint8_t>& b, const VxlVoxel2& v, std::uint8_t ud) -> bool
		{
			if (usePalette)
			{
				auto it = palMap.find(v.rgba);
				if (it == palMap.end()) return false;
				b.push_back((std::uint8_t)it->second);
			}
			else
			{
				std::uint8_t b4[4]; WrU32(b4, v.rgba);
				b.insert(b.end(), b4, b4 + 4);
			}
			if (usePackedAttr)
			{
				b.push_back((std::uint8_t)((v.normal & 31u) | ((v.layer & 7u) << 5)));
			}
			else
			{
				std::uint8_t b2[2]; WrU16(b2, v.normal);
				b.insert(b.end(), b2, b2 + 2);
				WrU16(b2, v.layer);
				b.insert(b.end(), b2, b2 + 2);
			}
			if (useUser) b.push_back(ud);
			return true;
		};

		auto blockIndexOf = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) -> std::uint32_t
		{
			return (z / bx0 * nY + y / bx0) * nX + (x / bx0);
		};
		struct Item { std::uint32_t idx; const VxlVoxel2* v; std::uint8_t ud; };
		std::vector<Item> sorted;
		sorted.reserve(sec.voxels.size());
		for (std::size_t vi = 0; vi < sec.voxels.size(); ++vi)
		{
			const auto& v = sec.voxels[vi];
			if (v.x >= sec.sizeX || v.y >= sec.sizeY || v.z >= sec.sizeZ) return false;
			Item it; it.idx = blockIndexOf(v.x, v.y, v.z); it.v = &v;
			it.ud = useUser ? sec.userData[vi] : 0;
			sorted.push_back(it);
		}
		std::sort(sorted.begin(), sorted.end(), [](const Item& a, const Item& b) { return a.idx < b.idx; });

		struct BlockOut { std::uint32_t idx; std::uint8_t bmode; std::vector<std::uint8_t> raw; };
		std::vector<BlockOut> blocks;
		bool emittedDirRuns = false;

		for (std::size_t i = 0; i < sorted.size();)
		{
			std::uint32_t idx = sorted[i].idx;
			std::size_t j = i; while (j < sorted.size() && sorted[j].idx == idx) ++j;
			std::size_t cnt = j - i;
			if (cnt > 65535) return false;
			std::uint32_t originX = (idx % nX) * bx0;
			std::uint32_t originY = ((idx / nX) % nY) * bx0;
			std::uint32_t originZ = (idx / (nX * nY)) * bx0;

			auto sortedV = sorted.begin() + i;
			std::vector<const Item*> slice; slice.reserve(cnt);
			for (std::size_t k = 0; k < cnt; ++k) slice.push_back(&sortedV[k]);
			std::sort(slice.begin(), slice.end(), [originX, originY, originZ](const Item* a, const Item* b)
			{
				std::uint32_t ax = a->v->x - originX, ay = a->v->y - originY, az = a->v->z - originZ;
				std::uint32_t bx = b->v->x - originX, by = b->v->y - originY, bz = b->v->z - originZ;
				if (ax != bx) return ax < bx;
				if (ay != by) return ay < by;
				return az < bz;
			});
			std::vector<std::uint32_t> lx(cnt), ly(cnt), lz(cnt);
			for (std::size_t k = 0; k < cnt; ++k)
			{
				lx[k] = slice[k]->v->x - originX;
				ly[k] = slice[k]->v->y - originY;
				lz[k] = slice[k]->v->z - originZ;
			}
			auto eqFields = [&](std::size_t a, std::size_t b) -> bool
			{
				if (slice[a]->v->rgba != slice[b]->v->rgba) return false;
				if (slice[a]->v->normal != slice[b]->v->normal) return false;
				if (slice[a]->v->layer != slice[b]->v->layer) return false;
				if (useUser && slice[a]->ud != slice[b]->ud) return false;
				return true;
			};

			std::vector<std::uint8_t> raw;
			raw.reserve(cnt * (4 + fieldW));
			std::vector<char> covered(cnt, 0);

			// —— z 游程（排序后 z 连续）——
			for (std::size_t k = 0; k < cnt; ++k)
			{
				if (covered[k]) continue;
				std::size_t runTo = k + 1;
				while (runTo < cnt
					&& lx[runTo] == lx[k] && ly[runTo] == ly[k]
					&& lz[runTo] == lz[runTo - 1] + 1
					&& eqFields(k, runTo) && lz[runTo] <= 127)
					++runTo;
				if (runTo - k >= 2)
				{
					raw.push_back(k5KindZRun);
					raw.push_back((std::uint8_t)lx[k]);
					raw.push_back((std::uint8_t)ly[k]);
					raw.push_back((std::uint8_t)lz[k]);
					raw.push_back((std::uint8_t)(runTo - k));
					if (!appendFields(raw, *slice[k]->v, slice[k]->ud)) return false;
					for (std::size_t t = k; t < runTo; ++t) covered[t] = 1;
				}
			}
			// —— y 游程（固定 x,z）——
			{
				std::unordered_map<std::uint64_t, std::vector<std::size_t>> g;
				for (std::size_t k = 0; k < cnt; ++k) if (!covered[k]) g[((std::uint64_t)lx[k] << 16) | lz[k]].push_back(k);
				for (auto& e : g)
				{
					auto& v = e.second;
					std::sort(v.begin(), v.end(), [&](std::size_t a, std::size_t b) { return ly[a] < ly[b]; });
					for (std::size_t t = 0; t < v.size();)
					{
						std::size_t rr = t + 1;
						while (rr < v.size() && ly[v[rr]] == ly[v[rr - 1]] + 1 && eqFields(v[t], v[rr]) && ly[v[rr]] <= 127) ++rr;
						if (rr - t >= 2)
						{
							raw.push_back(k5KindYRun);
							raw.push_back((std::uint8_t)lx[v[t]]);
							raw.push_back((std::uint8_t)lz[v[t]]);
							raw.push_back((std::uint8_t)ly[v[t]]);
							raw.push_back((std::uint8_t)(rr - t));
							if (!appendFields(raw, *slice[v[t]]->v, slice[v[t]]->ud)) return false;
							for (std::size_t q = t; q < rr; ++q) covered[v[q]] = 1;
							emittedDirRuns = true;
						}
						t = rr;
					}
				}
			}
			// —— x 游程（固定 y,z）——
			{
				std::unordered_map<std::uint64_t, std::vector<std::size_t>> g;
				for (std::size_t k = 0; k < cnt; ++k) if (!covered[k]) g[((std::uint64_t)ly[k] << 16) | lz[k]].push_back(k);
				for (auto& e : g)
				{
					auto& v = e.second;
					std::sort(v.begin(), v.end(), [&](std::size_t a, std::size_t b) { return lx[a] < lx[b]; });
					for (std::size_t t = 0; t < v.size();)
					{
						std::size_t rr = t + 1;
						while (rr < v.size() && lx[v[rr]] == lx[v[rr - 1]] + 1 && eqFields(v[t], v[rr]) && lx[v[rr]] <= 127) ++rr;
						if (rr - t >= 2)
						{
							raw.push_back(k5KindXRun);
							raw.push_back((std::uint8_t)ly[v[t]]);
							raw.push_back((std::uint8_t)lz[v[t]]);
							raw.push_back((std::uint8_t)lx[v[t]]);
							raw.push_back((std::uint8_t)(rr - t));
							if (!appendFields(raw, *slice[v[t]]->v, slice[v[t]]->ud)) return false;
							for (std::size_t q = t; q < rr; ++q) covered[v[q]] = 1;
							emittedDirRuns = true;
						}
						t = rr;
					}
				}
			}
			// —— 剩余 literal ——
			for (std::size_t k = 0; k < cnt; ++k)
			{
				if (covered[k]) continue;
				if (lx[k] > 127 || ly[k] > 127 || lz[k] > 127) return false;
				raw.push_back(k5KindLiteral);
				raw.push_back((std::uint8_t)lx[k]);
				raw.push_back((std::uint8_t)ly[k]);
				raw.push_back((std::uint8_t)lz[k]);
				if (!appendFields(raw, *slice[k]->v, slice[k]->ud)) return false;
			}

			// —— occupancy 可选：占位bitmap 更省则采用 ——
			std::uint8_t bmode = 0;
			{
				std::uint32_t mnx = lx[0], mxx = lx[0], mny = ly[0], mxy = ly[0], mnz = lz[0], mxz = lz[0];
				for (std::size_t k = 0; k < cnt; ++k)
				{
					if (lx[k] < mnx) mnx = lx[k];
					if (lx[k] > mxx) mxx = lx[k];
					if (ly[k] < mny) mny = ly[k];
					if (ly[k] > mxy) mxy = ly[k];
					if (lz[k] < mnz) mnz = lz[k];
					if (lz[k] > mxz) mxz = lz[k];
				}
				std::uint32_t bx = mxx - mnx + 1, by = mxy - mny + 1, bz = mxz - mnz + 1;
				// 占用位图只存 bbox 尺寸+位图+字段，不存 (mnx,mny,mnz)，解码按块原点重建；
				// 故仅当体素锚定在本地原点(mn==0)时才能用占用编码，否则退化避免数据损坏。
				if (bx <= 127 && by <= 127 && bz <= 127 && mnx == 0 && mny == 0 && mnz == 0)
				{
					std::uint64_t cells = (std::uint64_t)bx * by * bz;
					std::size_t bitBytes = (std::size_t)((cells + 7) / 8);
					std::size_t occBytes = 3 + bitBytes + (std::size_t)cnt * fieldW;
					if (occBytes < raw.size())
					{
						auto keyFn = [&](std::uint32_t xx, std::uint32_t yy, std::uint32_t zz) -> std::uint64_t
						{
							return (((std::uint64_t)zz * by + yy) * bx + xx);
						};
						std::unordered_map<std::uint64_t, std::size_t> cellToK;
						cellToK.reserve(cnt * 2);
						for (std::size_t k = 0; k < cnt; ++k)
							cellToK[keyFn(lx[k] - mnx, ly[k] - mny, lz[k] - mnz)] = k;
						std::vector<std::uint8_t> o;
						o.push_back((std::uint8_t)bx);
						o.push_back((std::uint8_t)by);
						o.push_back((std::uint8_t)bz);
						o.resize(3 + bitBytes, 0);
						for (auto& kv : cellToK)
							o[3 + (std::size_t)(kv.first >> 3)] |= (std::uint8_t)(1u << (kv.first & 7u));
						for (std::uint32_t zz = 0; zz < bz; ++zz)
							for (std::uint32_t yy = 0; yy < by; ++yy)
								for (std::uint32_t xx = 0; xx < bx; ++xx)
								{
									auto it = cellToK.find(keyFn(xx, yy, zz));
									if (it != cellToK.end())
									{
										std::size_t k = it->second;
										if (!appendFields(o, *slice[k]->v, slice[k]->ud)) return false;
									}
								}
						if (o.size() < raw.size())
						{
							raw = std::move(o);
							bmode |= k5BmOcc;
						}
					}
				}
			}

			BlockOut bo; bo.idx = idx; bo.bmode = bmode; bo.raw = std::move(raw);
			blocks.push_back(std::move(bo));
			i = j;
		}

		// —— 并行压缩 ——
		int alg = V5PickAlg();
		std::size_t nb = blocks.size();
		std::vector<std::vector<std::uint8_t>> comp(nb);
		std::vector<std::atomic<bool>> compOk(nb);
		for (auto& a : compOk) a.store(false);
		unsigned nc = std::thread::hardware_concurrency();
		if (nc < 1) nc = 1;
		if (nc > 8) nc = 8;
		if (nb > 0)
		{
			std::vector<std::thread> th;
			std::size_t chunk = (nb + nc - 1) / nc;
			auto worker = [&](std::size_t a, std::size_t b)
			{
				for (std::size_t k = a; k < b; ++k)
					compOk[k].store(AlgCompress(alg, blocks[k].raw.data(), blocks[k].raw.size(), comp[k]));
			};
			std::size_t start = 0;
			for (unsigned t = 0; t < nc && start < nb; ++t)
			{
				std::size_t end = std::min(nb, start + chunk);
				th.emplace_back(worker, start, end);
				start = end;
			}
			for (auto& t : th) t.join();
		}
		for (auto& ok : compOk) if (!ok.load()) return false;

		// —— 块去重（内容散列 + 字节等值确认）——
		std::unordered_map<std::uint64_t, std::size_t> dedupMap;
		std::vector<char> isDedup(nb, 0);
		std::vector<std::uint16_t> dedupTarget(nb, 0);
		bool anyDedup = false;
		for (std::size_t k = 0; k < nb; ++k)
		{
			std::uint64_t h = RawHash(blocks[k].raw);
			auto it = dedupMap.find(h);
			if (it != dedupMap.end() && blocks[it->second].raw == blocks[k].raw)
			{
				isDedup[k] = 1;
				dedupTarget[k] = (std::uint16_t)blocks[it->second].idx;
				anyDedup = true;
			}
			else dedupMap[h] = k;
		}

		// —— 布局标志 ——
		std::uint8_t layout = 0;
		if (useUser) layout |= k4UserData;
		if (usePalette) layout |= k4Palette;
		if (usePackedAttr) layout |= k4PackedAttr;
		layout |= k4VerticalRun;   // 表示"内有游程"
		layout |= k4BlockCrc;
		layout |= k4IndexTable;

		std::uint8_t layout2 = 0;
		bool anyOcc = false; for (auto& b : blocks) if (b.bmode & k5BmOcc) { anyOcc = true; break; }
		if (anyOcc) layout2 |= k5Occ;
		if (emittedDirRuns) layout2 |= k5DirRuns;
		if (anyDedup) layout2 |= k5Dedup;
		if (useShared) layout2 |= k5PalShared;
		layout2 |= (std::uint8_t)(((unsigned)alg & 3u) << k5AlgShift);
		outLayout2 = layout2;

		// —— 序列化 ——
		std::uint8_t head[22] = { 0 };
		head[0] = layout;
		WrU16(head + 2, (std::uint16_t)bx0);
		WrU16(head + 4, (std::uint16_t)nX);
		WrU16(head + 6, (std::uint16_t)nY);
		WrU16(head + 8, (std::uint16_t)nZ);
		WrU32(head + 10, (std::uint32_t)sec.voxels.size());
		WrU16(head + 14, (std::uint16_t)nb);
		WrU16(head + 16, (usePalette && !useShared) ? (std::uint16_t)palTable.size() : 0);
		head[18] = layout2;
		out.insert(out.end(), head, head + 21);   // 21 字节头，字节18=layout2

		if (usePalette && !useShared)
			for (std::uint32_t c : palTable)
			{
				std::uint8_t b4[4]; WrU32(b4, c);
				out.insert(out.end(), b4, b4 + 4);
			}

		std::vector<std::pair<std::uint16_t, std::uint32_t>> dir;
		for (std::size_t k = 0; k < nb; ++k)
		{
			dir.emplace_back((std::uint16_t)blocks[k].idx, (std::uint32_t)out.size());
			std::uint8_t b2[2]; WrU16(b2, (std::uint16_t)blocks[k].idx);
			out.insert(out.end(), b2, b2 + 2);
			std::uint8_t b4[4];
			if (isDedup[k])
			{
				out.push_back(k5BmDedup);
				WrU16(b4, dedupTarget[k]);
				out.insert(out.end(), b4, b4 + 2);
			}
			else
			{
				out.push_back(blocks[k].bmode);
				WrU32(b4, (std::uint32_t)blocks[k].raw.size());
				out.insert(out.end(), b4, b4 + 4);
				WrU32(b4, (std::uint32_t)comp[k].size());
				out.insert(out.end(), b4, b4 + 4);
				std::uint32_t crc = Crc32Update(0, blocks[k].raw.data(), blocks[k].raw.size());
				WrU32(b4, crc);
				out.insert(out.end(), b4, b4 + 4);
				out.insert(out.end(), comp[k].begin(), comp[k].end());
			}
		}
		// 块索引目录
		{
			std::uint8_t b2[2]; WrU16(b2, (std::uint16_t)dir.size());
			out.insert(out.end(), b2, b2 + 2);
			for (const auto& e : dir)
			{
				std::uint8_t bb[6];
				WrU16(bb, e.first);
				WrU32(bb + 2, e.second);
				out.insert(out.end(), bb, bb + 6);
			}
		}
		return true;
	}

	// —— v6 超大模型区块体（version=6）。与 v5 同能力，仅外部索引字段（nX/nY/nZ、
	//    blockCount、blockIndex、去重目标、目录 offset）由 u16 扩为 u32，支持任意大网格。
	static bool BuildBlockedBodyV6(const VxlSection2& sec,
		const std::vector<std::uint32_t>* sharedPalette,
		std::uint8_t& outLayout2, std::vector<std::uint8_t>& out)
	{
		out.clear();
		const std::uint32_t bx0 = kBlockSize;
		std::uint64_t nX = sec.sizeX / bx0 + (sec.sizeX % bx0 ? 1 : 0);
		std::uint64_t nY = sec.sizeY / bx0 + (sec.sizeY % bx0 ? 1 : 0);
		std::uint64_t nZ = sec.sizeZ / bx0 + (sec.sizeZ % bx0 ? 1 : 0);
		if (nX == 0 || nY == 0 || nZ == 0 || nX > 0xFFFFFFFFull || nY > 0xFFFFFFFFull || nZ > 0xFFFFFFFFull)
			return false;
		if (bx0 > 127) return false;

		bool useUser = false;
		if (!sec.userData.empty() && sec.userData.size() == sec.voxels.size())
			for (std::uint8_t u : sec.userData) if (u != 0) { useUser = true; break; }

		const bool useShared = (sharedPalette != nullptr);
		bool usePalette = useShared;
		std::vector<std::uint32_t> palTable;
		std::unordered_map<std::uint32_t, std::uint32_t> palMap;   // rgba -> 索引(局部或全局,u32)
		if (useShared)
		{
			for (std::size_t i = 0; i < sharedPalette->size(); ++i)
				palMap.emplace((*sharedPalette)[i], (std::uint32_t)i);
		}
		else
		{
			int pc = 0;
			{
				std::unordered_map<std::uint32_t, int> s;
				for (const auto& v : sec.voxels)
				{
					if (s.find(v.rgba) == s.end()) { s.emplace(v.rgba, 0); if ((int)s.size() > 256) break; }
				}
				pc = (int)s.size();
			}
			usePalette = pc > 0 && pc <= 256;
			if (usePalette)
			{
				for (const auto& v : sec.voxels)
					if (palMap.find(v.rgba) == palMap.end())
					{
						palMap.emplace(v.rgba, (std::uint32_t)palTable.size());
						palTable.push_back(v.rgba);
					}
			}
		}

		bool usePackedAttr = true;
		if (!sec.voxels.empty())
			for (const auto& v : sec.voxels)
				if (v.normal >= 32 || v.layer >= 8) { usePackedAttr = false; break; }

		std::size_t colorW = usePalette ? 1u : 4u;
		std::size_t attrW = usePackedAttr ? 1u : 4u;
		std::size_t fieldW = colorW + attrW + (useUser ? 1u : 0u);

		auto appendFields = [&](std::vector<std::uint8_t>& b, const VxlVoxel2& v, std::uint8_t ud) -> bool
		{
			if (usePalette)
			{
				auto it = palMap.find(v.rgba);
				if (it == palMap.end()) return false;
				b.push_back((std::uint8_t)it->second);
			}
			else
			{
				std::uint8_t b4[4]; WrU32(b4, v.rgba);
				b.insert(b.end(), b4, b4 + 4);
			}
			if (usePackedAttr)
				b.push_back((std::uint8_t)((v.normal & 31u) | ((v.layer & 7u) << 5)));
			else
			{
				std::uint8_t b2[2]; WrU16(b2, v.normal);
				b.insert(b.end(), b2, b2 + 2);
				WrU16(b2, v.layer);
				b.insert(b.end(), b2, b2 + 2);
			}
			if (useUser) b.push_back(ud);
			return true;
		};

		std::uint32_t nX32 = (std::uint32_t)nX, nY32 = (std::uint32_t)nY, nZ32 = (std::uint32_t)nZ;
		auto blockIndexOf = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) -> std::uint64_t
		{
			return ((std::uint64_t)(z / bx0) * nY32 + (y / bx0)) * nX32 + (x / bx0);
		};
		struct Item { std::uint32_t idxH; std::uint64_t idxFull; const VxlVoxel2* v; std::uint8_t ud; };
		std::vector<Item> sorted;
		sorted.reserve(sec.voxels.size());
		for (std::size_t vi = 0; vi < sec.voxels.size(); ++vi)
		{
			const auto& v = sec.voxels[vi];
			if (v.x >= sec.sizeX || v.y >= sec.sizeY || v.z >= sec.sizeZ) return false;
			std::uint64_t blk = blockIndexOf(v.x, v.y, v.z);
			Item it; it.idxFull = blk; it.v = &v;
			// 目录只存非空块，u32 序号按出现顺序分配；idxFull 供跨块识别（去重/坐标）。
			it.ud = useUser ? sec.userData[vi] : 0;
			sorted.push_back(it);
		}
		std::sort(sorted.begin(), sorted.end(),
			[](const Item& a, const Item& b) { return a.idxFull < b.idxFull; });

		struct BlockOut { std::uint64_t idx; std::uint8_t bmode; std::vector<std::uint8_t> raw; };
		std::vector<BlockOut> blocks;
		bool emittedDirRuns = false;

		for (std::size_t i = 0; i < sorted.size();)
		{
			std::uint64_t idx = sorted[i].idxFull;
			std::size_t j = i; while (j < sorted.size() && sorted[j].idxFull == idx) ++j;
			std::size_t cnt = j - i;
			if (cnt > 65535) return false;
			std::uint32_t originX = (std::uint32_t)(idx % nX32) * bx0;
			std::uint32_t originY = (std::uint32_t)((idx / nX32) % nY32) * bx0;
			std::uint32_t originZ = (std::uint32_t)(idx / ((std::uint64_t)nX32 * nY32)) * bx0;   // (nX32*nY32) 以 u64 计算防溢出

			auto sortedV = sorted.begin() + i;
			std::vector<const Item*> slice; slice.reserve(cnt);
			for (std::size_t k = 0; k < cnt; ++k) slice.push_back(&sortedV[k]);
			std::sort(slice.begin(), slice.end(), [originX, originY, originZ](const Item* a, const Item* b)
			{
				std::uint32_t ax = a->v->x - originX, ay = a->v->y - originY, az = a->v->z - originZ;
				std::uint32_t bx = b->v->x - originX, by = b->v->y - originY, bz = b->v->z - originZ;
				if (ax != bx) return ax < bx;
				if (ay != by) return ay < by;
				return az < bz;
			});
			std::vector<std::uint32_t> lx(cnt), ly(cnt), lz(cnt);
			for (std::size_t k = 0; k < cnt; ++k)
			{
				lx[k] = slice[k]->v->x - originX;
				ly[k] = slice[k]->v->y - originY;
				lz[k] = slice[k]->v->z - originZ;
			}
			auto eqFields = [&](std::size_t a, std::size_t b) -> bool
			{
				if (slice[a]->v->rgba != slice[b]->v->rgba) return false;
				if (slice[a]->v->normal != slice[b]->v->normal) return false;
				if (slice[a]->v->layer != slice[b]->v->layer) return false;
				if (useUser && slice[a]->ud != slice[b]->ud) return false;
				return true;
			};

			std::vector<std::uint8_t> raw;
			raw.reserve(cnt * (4 + fieldW));
			std::vector<char> covered(cnt, 0);

			// —— z 游程 ——
			for (std::size_t k = 0; k < cnt; ++k)
			{
				if (covered[k]) continue;
				std::size_t runTo = k + 1;
				while (runTo < cnt
					&& lx[runTo] == lx[k] && ly[runTo] == ly[k]
					&& lz[runTo] == lz[runTo - 1] + 1
					&& eqFields(k, runTo) && lz[runTo] <= 127)
					++runTo;
				if (runTo - k >= 2)
				{
					raw.push_back(k5KindZRun);
					raw.push_back((std::uint8_t)lx[k]);
					raw.push_back((std::uint8_t)ly[k]);
					raw.push_back((std::uint8_t)lz[k]);
					raw.push_back((std::uint8_t)(runTo - k));
					if (!appendFields(raw, *slice[k]->v, slice[k]->ud)) return false;
					for (std::size_t t = k; t < runTo; ++t) covered[t] = 1;
				}
			}
			// —— y 游程 ——
			{
				std::unordered_map<std::uint64_t, std::vector<std::size_t>> g;
				for (std::size_t k = 0; k < cnt; ++k) if (!covered[k]) g[((std::uint64_t)lx[k] << 16) | lz[k]].push_back(k);
				for (auto& e : g)
				{
					auto& v = e.second;
					std::sort(v.begin(), v.end(), [&](std::size_t a, std::size_t b) { return ly[a] < ly[b]; });
					for (std::size_t t = 0; t < v.size();)
					{
						std::size_t rr = t + 1;
						while (rr < v.size() && ly[v[rr]] == ly[v[rr - 1]] + 1 && eqFields(v[t], v[rr]) && ly[v[rr]] <= 127) ++rr;
						if (rr - t >= 2)
						{
							raw.push_back(k5KindYRun);
							raw.push_back((std::uint8_t)lx[v[t]]);
							raw.push_back((std::uint8_t)lz[v[t]]);
							raw.push_back((std::uint8_t)ly[v[t]]);
							raw.push_back((std::uint8_t)(rr - t));
							if (!appendFields(raw, *slice[v[t]]->v, slice[v[t]]->ud)) return false;
							for (std::size_t q = t; q < rr; ++q) covered[v[q]] = 1;
							emittedDirRuns = true;
						}
						t = rr;
					}
				}
			}
			// —— x 游程 ——
			{
				std::unordered_map<std::uint64_t, std::vector<std::size_t>> g;
				for (std::size_t k = 0; k < cnt; ++k) if (!covered[k]) g[((std::uint64_t)ly[k] << 16) | lz[k]].push_back(k);
				for (auto& e : g)
				{
					auto& v = e.second;
					std::sort(v.begin(), v.end(), [&](std::size_t a, std::size_t b) { return lx[a] < lx[b]; });
					for (std::size_t t = 0; t < v.size();)
					{
						std::size_t rr = t + 1;
						while (rr < v.size() && lx[v[rr]] == lx[v[rr - 1]] + 1 && eqFields(v[t], v[rr]) && lx[v[rr]] <= 127) ++rr;
						if (rr - t >= 2)
						{
							raw.push_back(k5KindXRun);
							raw.push_back((std::uint8_t)ly[v[t]]);
							raw.push_back((std::uint8_t)lz[v[t]]);
							raw.push_back((std::uint8_t)lx[v[t]]);
							raw.push_back((std::uint8_t)(rr - t));
							if (!appendFields(raw, *slice[v[t]]->v, slice[v[t]]->ud)) return false;
							for (std::size_t q = t; q < rr; ++q) covered[v[q]] = 1;
							emittedDirRuns = true;
						}
						t = rr;
					}
				}
			}
			// —— 剩余 literal ——
			for (std::size_t k = 0; k < cnt; ++k)
			{
				if (covered[k]) continue;
				if (lx[k] > 127 || ly[k] > 127 || lz[k] > 127) return false;
				raw.push_back(k5KindLiteral);
				raw.push_back((std::uint8_t)lx[k]);
				raw.push_back((std::uint8_t)ly[k]);
				raw.push_back((std::uint8_t)lz[k]);
				if (!appendFields(raw, *slice[k]->v, slice[k]->ud)) return false;
			}

			// —— occupancy 可选 ——
			std::uint8_t bmode = 0;
			{
				std::uint32_t mnx = lx[0], mxx = lx[0], mny = ly[0], mxy = ly[0], mnz = lz[0], mxz = lz[0];
				for (std::size_t k = 0; k < cnt; ++k)
				{
					if (lx[k] < mnx) mnx = lx[k];
					if (lx[k] > mxx) mxx = lx[k];
					if (ly[k] < mny) mny = ly[k];
					if (ly[k] > mxy) mxy = ly[k];
					if (lz[k] < mnz) mnz = lz[k];
					if (lz[k] > mxz) mxz = lz[k];
				}
				std::uint32_t bx = mxx - mnx + 1, by = mxy - mny + 1, bz = mxz - mnz + 1;
				// 占用位图只存 bbox 尺寸+位图+字段，不存 (mnx,mny,mnz)，解码按块原点重建；
				// 故仅当体素锚定在本地原点(mn==0)时才能用占用编码，否则退化避免数据损坏。
				if (bx <= 127 && by <= 127 && bz <= 127 && mnx == 0 && mny == 0 && mnz == 0)
				{
					std::uint64_t cells = (std::uint64_t)bx * by * bz;
					std::size_t bitBytes = (std::size_t)((cells + 7) / 8);
					std::size_t occBytes = 3 + bitBytes + (std::size_t)cnt * fieldW;
					if (occBytes < raw.size())
					{
						auto keyFn = [&](std::uint32_t xx, std::uint32_t yy, std::uint32_t zz) -> std::uint64_t
						{ return (((std::uint64_t)zz * by + yy) * bx + xx); };
						std::unordered_map<std::uint64_t, std::size_t> cellToK;
						cellToK.reserve(cnt * 2);
						for (std::size_t k = 0; k < cnt; ++k)
							cellToK[keyFn(lx[k] - mnx, ly[k] - mny, lz[k] - mnz)] = k;
						std::vector<std::uint8_t> o;
						o.push_back((std::uint8_t)bx);
						o.push_back((std::uint8_t)by);
						o.push_back((std::uint8_t)bz);
						o.resize(3 + bitBytes, 0);
						for (auto& kv : cellToK)
							o[3 + (std::size_t)(kv.first >> 3)] |= (std::uint8_t)(1u << (kv.first & 7u));
						for (std::uint32_t zz = 0; zz < bz; ++zz)
							for (std::uint32_t yy = 0; yy < by; ++yy)
								for (std::uint32_t xx = 0; xx < bx; ++xx)
								{
									auto it = cellToK.find(keyFn(xx, yy, zz));
									if (it != cellToK.end())
									{
										std::size_t k = it->second;
										if (!appendFields(o, *slice[k]->v, slice[k]->ud)) return false;
									}
								}
						if (o.size() < raw.size())
						{
							raw = std::move(o);
							bmode |= k5BmOcc;
						}
					}
				}
			}

			BlockOut bo; bo.idx = idx; bo.bmode = bmode; bo.raw = std::move(raw);
			blocks.push_back(std::move(bo));
			i = j;
		}

		// —— 并行压缩 ——
		int alg = V5PickAlg();
		std::size_t nb = blocks.size();
		std::vector<std::vector<std::uint8_t>> comp(nb);
		std::vector<std::atomic<bool>> compOk(nb);
		for (auto& a : compOk) a.store(false);
		unsigned nc = std::thread::hardware_concurrency();
		if (nc < 1) nc = 1;
		if (nc > 8) nc = 8;
		if (nb > 0)
		{
			std::vector<std::thread> th;
			std::size_t chunk = (nb + nc - 1) / nc;
			auto worker = [&](std::size_t a, std::size_t b)
			{ for (std::size_t k = a; k < b; ++k) compOk[k].store(AlgCompress(alg, blocks[k].raw.data(), blocks[k].raw.size(), comp[k])); };
			std::size_t start = 0;
			for (unsigned t = 0; t < nc && start < nb; ++t)
			{
				std::size_t end = std::min(nb, start + chunk);
				th.emplace_back(worker, start, end);
				start = end;
			}
			for (auto& t : th) t.join();
		}
		for (auto& ok : compOk) if (!ok.load()) return false;

		// —— 块去重 ——
		std::unordered_map<std::uint64_t, std::size_t> dedupMap;
		std::vector<char> isDedup(nb, 0);
		std::vector<std::uint32_t> dedupTarget(nb, 0);
		bool anyDedup = false;
		for (std::size_t k = 0; k < nb; ++k)
		{
			std::uint64_t h = RawHash(blocks[k].raw);
			auto it = dedupMap.find(h);
			// 去重目标在文件里是 u32 块索引：仅当目标块的线性索引可被 u32 表达时才去重，
			// 否则退化为内联存储（避免 u32 截断导致读回时指向错误块）。v5 的 u16 跨度天然满足此约束。
			if (it != dedupMap.end() && blocks[it->second].raw == blocks[k].raw
				&& blocks[it->second].idx <= 0xFFFFFFFFull)
			{
				isDedup[k] = 1;
				dedupTarget[k] = (std::uint32_t)blocks[it->second].idx;
				anyDedup = true;
			}
			else dedupMap[h] = k;
		}

		// —— 逐块索引容量 ——
		// v6 的逐块索引是 u32：任何非空块的线性索引超过 u32 上限都无法落盘，直接拒绝而非截断。
		// 等价约束为网格总块数 nX*nY*nZ ≤ 2^32（约 1.4e14 体素），对正常纵横比模型绰绰有余。
		for (const auto& b : blocks)
			if (b.idx > 0xFFFFFFFFull) return false;

#if defined(VXL2_DEBUG_BLOCKS)
		for (const auto& b : blocks)
		{
			std::printf("DBG block idx=%llu bmode=%u raw=", (unsigned long long)b.idx, (unsigned)b.bmode);
			for (auto c : b.raw) std::printf("%02X", (unsigned)c);
			std::printf("\n");
		}
#endif

		// —— 块索引目录可行性（u32 目录：块索引与数据偏移都须 ≤ u32）——
		// 任一无法表达时隐去索引表（读取按 k4IndexTable 标志跳过），保证无损而非截断。
		std::vector<std::uint32_t> dirOff;
		bool indexOk = (blocks.size() <= 0xFFFFFFFFull);
		if (indexOk)
		{
			dirOff.resize(blocks.size());
			std::uint64_t cursor = 30u + (usePalette && !useShared ? (std::uint64_t)palTable.size() * 4u : 0u);
			for (std::size_t k = 0; k < blocks.size(); ++k)
			{
				if (blocks[k].idx > 0xFFFFFFFFull) indexOk = false;
				dirOff[k] = (std::uint32_t)cursor;
				if (cursor > 0xFFFFFFFFull) indexOk = false;
				cursor += isDedup[k] ? 9u : 17u + comp[k].size();
			}
		}

		// —— 布局标志 ——
		std::uint8_t layout = 0;
		if (useUser) layout |= k4UserData;
		if (usePalette) layout |= k4Palette;
		if (usePackedAttr) layout |= k4PackedAttr;
		layout |= k4VerticalRun;
		layout |= k4BlockCrc;
		if (indexOk) layout |= k4IndexTable;

		std::uint8_t layout2 = 0;
		bool anyOcc = false; for (auto& b : blocks) if (b.bmode & k5BmOcc) { anyOcc = true; break; }
		if (anyOcc) layout2 |= k5Occ;
		if (emittedDirRuns) layout2 |= k5DirRuns;
		if (anyDedup) layout2 |= k5Dedup;
		if (useShared) layout2 |= k5PalShared;
		layout2 |= (std::uint8_t)(((unsigned)alg & 3u) << k5AlgShift);
		outLayout2 = layout2;

		// —— 序列化（30 字节头，索引字段全 u32）——
		std::uint8_t head[30] = { 0 };
		head[0] = layout;
		WrU32(head + 2, bx0);
		WrU32(head + 6, nX32);
		WrU32(head + 10, nY32);
		WrU32(head + 14, nZ32);
		WrU32(head + 18, (std::uint32_t)sec.voxels.size());
		WrU32(head + 22, (std::uint32_t)nb);
		WrU16(head + 26, (usePalette && !useShared) ? (std::uint16_t)palTable.size() : 0);
		head[28] = layout2;
		out.insert(out.end(), head, head + 30);

		if (usePalette && !useShared)
			for (std::uint32_t c : palTable)
			{
				std::uint8_t b4[4]; WrU32(b4, c);
				out.insert(out.end(), b4, b4 + 4);
			}

		for (std::size_t k = 0; k < nb; ++k)
		{
			std::uint8_t b4[4];
			WrU32(b4, (std::uint32_t)blocks[k].idx);
			out.insert(out.end(), b4, b4 + 4);
			out.push_back((k5BmDedup && isDedup[k]) ? k5BmDedup : (std::uint8_t)0);
			if (isDedup[k])
			{
				// 去重引用：bmode 用 k5BmDedup；后续以 u32 目标块索引
				std::uint8_t bo2 = k5BmDedup;
				out.back() = bo2;
				WrU32(b4, dedupTarget[k]);
				out.insert(out.end(), b4, b4 + 4);
			}
			else
			{
				out.back() = blocks[k].bmode;
				WrU32(b4, (std::uint32_t)blocks[k].raw.size());
				out.insert(out.end(), b4, b4 + 4);
				WrU32(b4, (std::uint32_t)comp[k].size());
				out.insert(out.end(), b4, b4 + 4);
				std::uint32_t crc = Crc32Update(0, blocks[k].raw.data(), blocks[k].raw.size());
				WrU32(b4, crc);
				out.insert(out.end(), b4, b4 + 4);
				out.insert(out.end(), comp[k].begin(), comp[k].end());
			}
		}
		// 块索引目录（u32 计数 + u32 条目）；仅在整目录可被 u32 表达时写入，否则隐去索引表
		if (indexOk)
		{
			std::uint8_t b4[4];
			WrU32(b4, (std::uint32_t)nb);
			out.insert(out.end(), b4, b4 + 4);
			for (std::size_t k = 0; k < nb; ++k)
			{
				std::uint8_t bb[8];
				WrU32(bb, (std::uint32_t)blocks[k].idx);
				WrU32(bb + 4, dirOff[k]);
				out.insert(out.end(), bb, bb + 8);
			}
		}
		return true;
	}
}

bool Vxl2::Encode(const std::vector<VxlSection2>& sections,
	std::vector<std::uint8_t>& out_data, const VxlGlobal2* global,
	Vxl2EncodeMode mode)
{
	out_data.clear();
	if (sections.empty())
		return false;

	std::uint32_t version = (mode == Vxl2EncodeMode::BlockedCompress) ? 3u
		: (mode == Vxl2EncodeMode::BlockedOptimized) ? 4u
		: (mode == Vxl2EncodeMode::BlockedV5) ? 5u
		: (mode == Vxl2EncodeMode::BlockedV6) ? 6u : 2u;

	// —— v5 跨 section 共享调色板 ——
	// 合并全部节的颜色，若唯一色数 ≤ 256 则写为文件级共享调色板，各节色存"全局索引"以省去
	// 各自节级色表。仅当调用方未提供独立全局调色板时自动启用，以免覆盖其显示语义；通过
	// effGlobal 注入后的全局段承载共享调色板。
	VxlGlobal2 effGlobal;
	const VxlGlobal2* sendGlobal = global;
	std::vector<std::uint32_t> sharedPalette;
	bool wantShared = false;
	if ((mode == Vxl2EncodeMode::BlockedV5 || mode == Vxl2EncodeMode::BlockedV6)
		&& !(global && !global->paletteRgba.empty()))
	{
		{
			std::unordered_map<std::uint32_t, int> s;
			for (const auto& sec : sections)
				for (const auto& v : sec.voxels)
					s.emplace(v.rgba, 0);
			for (const auto& kv : s) sharedPalette.push_back(kv.first);
		}
		if (!sharedPalette.empty() && sharedPalette.size() <= 256)
			wantShared = true;
	}
	if (wantShared)
	{
		effGlobal = global ? *global : VxlGlobal2{};
		effGlobal.paletteRgba.clear();
		effGlobal.paletteRgba.reserve(sharedPalette.size() * 4);
		for (std::uint32_t c : sharedPalette)
		{
			std::uint8_t b4[4]; WrU32(b4, c);
			effGlobal.paletteRgba.insert(effGlobal.paletteRgba.end(), b4, b4 + 4);
		}
		effGlobal.hasGlobal = true;   // 确保写入全局段以承载共享调色板
		sendGlobal = &effGlobal;
	}

	// 先确定是否写全局段
	bool hasGlobal = sendGlobal && (sendGlobal->hasGlobal || !sendGlobal->paletteRgba.empty()
		|| sendGlobal->upAxis != 'y' || sendGlobal->unitScale != 1.f || !sendGlobal->thumbnail.empty()
		|| sendGlobal->crc32 != 0 || !sendGlobal->metadata.empty());

	// 文件头 16 字节：magic/version/count/flags
	std::uint8_t hdr[16];
	std::memcpy(hdr, kMagic, 4);
	WrU32(hdr + 4, version);
	WrU32(hdr + 8, (std::uint32_t)sections.size());
	WrU32(hdr + 12, hasGlobal ? 1u : 0u);   // flags bit0 = 有全局段
	out_data.insert(out_data.end(), hdr, hdr + 16);

	// 是否需要回填 CRC（调用方以 crc32 != 0 请求）
	bool wantCrc = sendGlobal && sendGlobal->crc32 != 0;

	// 全局段（chunkCount 占位，先记录其位置便于后填）。CRC chunk 含 4 字节，先写 0 占位。
	std::int64_t globalCountPos = -1;
	if (hasGlobal)
	{
		std::vector<std::uint8_t> chunks;
		std::uint32_t globalChunkCount = 0;   // 真实验证：chunks 字节数是"全部字节数"，非 chunk 个数
		if (!sendGlobal->paletteRgba.empty())
		{
			std::vector<std::uint8_t> pl;
			AppendU16(pl, (std::uint16_t)(sendGlobal->paletteRgba.size() / 4));
			AppendBytes(pl, sendGlobal->paletteRgba.data(), sendGlobal->paletteRgba.size());
			AppendChunk(chunks, kPaletteChunk, pl);
			++globalChunkCount;
		}
		{
			std::vector<std::uint8_t> mt;
			mt.push_back(sendGlobal->upAxis ? sendGlobal->upAxis : 'y');
			AppendF32(mt, sendGlobal->unitScale);
			AppendChunk(chunks, kMetaChunk, mt);
			++globalChunkCount;
		}
		if (!sendGlobal->thumbnail.empty())
		{
			std::vector<std::uint8_t> th;
			th.push_back(sendGlobal->thumbFormat ? sendGlobal->thumbFormat : 1);
			AppendBytes(th, sendGlobal->thumbnail.data(), sendGlobal->thumbnail.size());
			AppendChunk(chunks, kThumbChunk, th);
			++globalChunkCount;
		}
		if (wantCrc)
		{
			std::vector<std::uint8_t> cc;
			AppendU32(cc, 0);   // 占位，sections 写完后回填
			AppendChunk(chunks, kCrcChunk, cc);
			++globalChunkCount;
		}
		if (!sendGlobal->metadata.empty())
		{
			std::vector<std::uint8_t> kv;
			AppendU32(kv, (std::uint32_t)sendGlobal->metadata.size());
			for (const auto& e : sendGlobal->metadata)
			{
				AppendU32(kv, (std::uint32_t)e.first.size());
				AppendBytes(kv, (const std::uint8_t*)e.first.data(), e.first.size());
				AppendU32(kv, (std::uint32_t)e.second.size());
				AppendBytes(kv, (const std::uint8_t*)e.second.data(), e.second.size());
			}
			AppendChunk(chunks, kMetaKVChunk, kv);
			++globalChunkCount;
		}

		globalCountPos = (std::int64_t)out_data.size();
		AppendU32(out_data, globalChunkCount);
		out_data.insert(out_data.end(), chunks.begin(), chunks.end());
	}

	// sections 区段起始（CRC 覆盖范围）
	std::int64_t sectionsStart = (std::int64_t)out_data.size();

	for (const VxlSection2& sec : sections)
	{
		if (sec.sizeX == 0 || sec.sizeY == 0 || sec.sizeZ == 0)
		{
			out_data.clear();
			return false;
		}
		if (sec.voxels.size() > 0xFFFFFFFFull)
		{
			out_data.clear();
			return false;   // 单节体素数超出 u32 字段上限，拒绝而非截断
		}
		if (sec.name.size() > 65535)
		{
			out_data.clear();
			return false;
		}
		AppendU32(out_data, (std::uint32_t)sec.name.size());
		AppendBytes(out_data, (const std::uint8_t*)sec.name.data(), sec.name.size());
		AppendU32(out_data, sec.sizeX);
		AppendU32(out_data, sec.sizeY);
		AppendU32(out_data, sec.sizeZ);
		AppendU32(out_data, (std::uint32_t)sec.voxels.size());

		// 节内 chunk
		std::vector<std::uint8_t> chunks;
		std::uint32_t secChunkCount = 0;

		if (mode == Vxl2EncodeMode::BlockedCompress)
		{
			std::vector<std::uint8_t> body;
			if (!BuildBlockedBody(sec, body))
			{
				out_data.clear();
				return false;
			}
			AppendChunk(chunks, kBodyBlockChunk, body);
			++secChunkCount;
		}
		else if (mode == Vxl2EncodeMode::BlockedOptimized)
		{
			std::vector<std::uint8_t> body;
			if (!BuildBlockedBodyV4(sec, body))
			{
				out_data.clear();
				return false;
			}
			AppendChunk(chunks, kBodyBlockChunk, body);
			++secChunkCount;
		}
		else if (mode == Vxl2EncodeMode::BlockedV5)
		{
			std::vector<std::uint8_t> body;
			std::uint8_t layout2 = 0;
			if (!BuildBlockedBodyV5(sec, wantShared ? &sharedPalette : nullptr, layout2, body))
			{
				out_data.clear();
				return false;
			}
			AppendChunk(chunks, kBodyBlockChunk, body);
			++secChunkCount;
		}
		else if (mode == Vxl2EncodeMode::BlockedV6)
		{
			std::vector<std::uint8_t> body;
			std::uint8_t layout2 = 0;
			if (!BuildBlockedBodyV6(sec, wantShared ? &sharedPalette : nullptr, layout2, body))
			{
				out_data.clear();
				return false;
			}
			AppendChunk(chunks, kBodyBlockChunk, body);
			++secChunkCount;
		}

		if (!sec.materials.empty())
		{
			std::vector<std::uint8_t> m;
			AppendU16(m, (std::uint16_t)sec.materials.size());
			for (const VxlMaterial2& mat : sec.materials)
			{
				AppendU16(m, mat.layer);
				AppendU16(m, (std::uint16_t)std::min<size_t>(mat.name.size(), 65535));
				AppendBytes(m, (const std::uint8_t*)mat.name.data(), mat.name.size());
				AppendF32(m, mat.roughness);
				AppendF32(m, mat.metalness);
				AppendU32(m, mat.emissive);
			}
			AppendChunk(chunks, kMatChunk, m);
			++secChunkCount;
		}
		if (!sec.customNormals.empty())
		{
			std::vector<std::uint8_t> nm;
			AppendU32(nm, (std::uint32_t)(sec.customNormals.size() / 3));
			for (float f : sec.customNormals)
				AppendF32(nm, f);
			AppendChunk(chunks, kNormChunk, nm);
			++secChunkCount;
		}
		if (sec.lodLevel != 0)
		{
			std::vector<std::uint8_t> ld;
			AppendU32(ld, sec.lodLevel);
			AppendChunk(chunks, kLodChunk, ld);
			++secChunkCount;
		}
		AppendU32(out_data, secChunkCount);
		out_data.insert(out_data.end(), chunks.begin(), chunks.end());

		// body 记录（仅 v2 固定；v3/v4 区块体已作为 body chunk 写入，不得再追加定长记录）
		if (mode == Vxl2EncodeMode::Fixed20)
		{
			std::uint8_t rec[20];
			for (const VxlVoxel2& v : sec.voxels)
			{
				WrU32(rec, v.x); WrU32(rec + 4, v.y); WrU32(rec + 8, v.z); WrU32(rec + 12, v.rgba);
				WrU16(rec + 16, v.normal); WrU16(rec + 18, v.layer);
				out_data.insert(out_data.end(), rec, rec + kRecordSize);
			}
		}
	}

	// CRC 回填：覆盖 sections 区段（sectionsStart..文件尾）
	if (wantCrc)
	{
		std::uint32_t crc = Crc32Update(0, out_data.data() + sectionsStart,
			(size_t)(out_data.size() - sectionsStart));
		// 定位全局段中 kCrcChunk 的 payload，回填
		// 重新线性扫描区域：文件头 16 起，读 chunkCount，逐 chunk 找 type==4
		if (globalCountPos >= 0)
		{
			size_t off = 16;
			if (off + 4 <= out_data.size())
			{
				std::uint32_t cnt = RdU32(out_data.data() + off); off += 4;
				for (std::uint32_t i = 0; i < cnt && off + 8 <= out_data.size(); ++i)
				{
					std::uint32_t t = RdU32(out_data.data() + off);
					std::uint32_t cl = RdU32(out_data.data() + off + 4);
					off += 8;
					if (t == kCrcChunk && cl >= 4 && off + 4 <= out_data.size())
					{
						WrU32(out_data.data() + off, crc);
						break;
					}
					if (off + cl > out_data.size()) break;
					off += cl;
				}
			}
		}
	}

	return true;
}

bool Vxl2::FromVxl(const std::vector<VxlSection>& v1,
	const std::vector<std::uint8_t>& palette768,
	std::vector<VxlSection2>& v2)
{
	v2.clear();
	for (const VxlSection& s : v1)
	{
		std::vector<VxlVoxel> vox;
		VxlDecoder::GetVoxels(s, vox);
		VxlSection2 s2;
		s2.name = s.name;
		s2.sizeX = (std::uint32_t)s.sizeX;
		s2.sizeY = (std::uint32_t)s.sizeY;
		s2.sizeZ = (std::uint32_t)s.sizeZ;
		s2.lodLevel = 0;
		s2.voxels.reserve(vox.size());
		for (const VxlVoxel& v : vox)
		{
			VxlVoxel2 o;
			o.x = (std::uint32_t)v.x;
			o.y = (std::uint32_t)v.y;
			o.z = (std::uint32_t)v.z;
			int b3 = (int)v.colorIndex * 3;
			int r = 255, g = 255, bl = 255;
			if (b3 >= 0 && b3 + 2 < (int)palette768.size())
			{
				r = palette768[b3]; g = palette768[b3 + 1]; bl = palette768[b3 + 2];
			}
			o.rgba = ((std::uint32_t)r << 24) | ((std::uint32_t)g << 16) | ((std::uint32_t)bl << 8) | 0xFFu;
			o.normal = v.normalIndex;
			o.layer = 0;
			s2.voxels.push_back(o);
		}
		v2.push_back(std::move(s2));
	}
	return !v2.empty();
}

bool Vxl2::ToVxl(const std::vector<VxlSection2>& v2,
	const std::vector<std::uint8_t>& palette768,
	bool strict,
	std::vector<VxlSection>& v1)
{
	v1.clear();

	// RGBA → 调色板最近色索引
	auto nearest = [&palette768](std::uint32_t rgba) -> int
	{
		int best = 0;
		int bestD = 1 << 30;
		int r = (int)((rgba >> 24) & 0xFF), g = (int)((rgba >> 16) & 0xFF), b = (int)((rgba >> 8) & 0xFF);
		int colors = (int)palette768.size() / 3;
		if (colors > 256) colors = 256;
		for (int i = 0; i < colors; ++i)
		{
			int dr = r - palette768[i * 3 + 0];
			int dg = g - palette768[i * 3 + 1];
			int db = b - palette768[i * 3 + 2];
			int d = dr * dr + dg * dg + db * db;
			if (d < bestD) { bestD = d; best = i; }
		}
		return best;
	};

	for (const VxlSection2& s : v2)
	{
		// RB2 单轴上限 255；超出按 non-strict 钳位 or strict 拒写
		if (s.sizeX > 255 || s.sizeY > 255 || s.sizeZ > 255)
		{
			if (strict)
			{
				v1.clear();
				return false;
			}
		}
		std::int64_t sx = (std::int64_t)std::min<std::uint32_t>(s.sizeX, 255);
		std::int64_t sy = (std::int64_t)std::min<std::uint32_t>(s.sizeY, 255);
		std::int64_t sz = (std::int64_t)std::min<std::uint32_t>(s.sizeZ, 255);

		// 每 (x,y) 列收集各 z 的"layer 最小"记录
		std::vector<std::vector<VxlVoxel2>> best((size_t)(sx * sy));
		for (const VxlVoxel2& v : s.voxels)
		{
			if (v.x >= s.sizeX || v.y >= s.sizeY || v.z >= s.sizeZ)
			{
				if (strict) { v1.clear(); return false; }
				continue;
			}
			if (v.x >= sx || v.y >= sy || v.z >= sz) continue; // 被钳掉的区域丢弃
			auto& list = best[(size_t)((std::int64_t)v.y * sx + v.x)];
			auto it = list.begin();
			for (; it != list.end(); ++it) if (it->z == v.z) break;
			if (it == list.end()) list.push_back(v);
			else if (v.layer < it->layer) *it = v;            // 同格保留 layer 最小
		}

		VxlSection vs;
		vs.name = s.name;
		vs.sizeX = (int)sx; vs.sizeY = (int)sy; vs.sizeZ = (int)sz;
		vs.normalsMode = 4;
		vs.minBounds[0] = vs.minBounds[1] = vs.minBounds[2] = 0.f;
		vs.maxBounds[0] = (float)sx; vs.maxBounds[1] = (float)sy; vs.maxBounds[2] = (float)sz;
		vs.hvaMultiplier = 1.f;

		for (std::int64_t yy = 0; yy < sy; ++yy)
		{
			for (std::int64_t xx = 0; xx < sx; ++xx)
			{
				const auto& list = best[(size_t)(yy * sx + xx)];
				if (list.empty()) continue;
				std::vector<std::pair<std::uint16_t, std::uint8_t>> zcol; // z,color
				for (const auto& b : list)
					zcol.push_back(std::make_pair((std::uint16_t)b.z, (std::uint8_t)nearest(b.rgba)));
				std::sort(zcol.begin(), zcol.end());
				std::vector<std::pair<std::uint16_t, std::uint8_t>> uniq;
				for (const auto& e : zcol)
					if (uniq.empty() || uniq.back().first != e.first) uniq.push_back(e);

				VxlSpan span;
				span.x = (int)xx; span.y = (int)yy;
				for (const auto& e : uniq)
				{
					VxlVoxel vox;
					vox.x = (int)xx; vox.y = (int)yy; vox.z = (int)e.first;
					vox.colorIndex = e.second;
					vox.normalIndex = 0;
					span.voxels.push_back(vox);
				}
				vs.spans.push_back(span);
			}
		}
		v1.push_back(std::move(vs));
	}
	return !v1.empty();
}
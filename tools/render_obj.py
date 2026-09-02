#!/usr/bin/env python3
"""快速渲染大 OBJ 网格（点溅射 z-buffer，向量化）。用于 277 万面高细节网格预览。"""
import os
import time
import numpy as np
from PIL import Image

OBJ = "/workspace/vxl_tool/samples/sci_tank.obj"
OUT = "/workspace/vxl_tool/out"


def load_obj_fast(path):
    t0 = time.time()
    vrows = []
    frows = []
    with open(path) as f:
        for line in f:
            if line[0] == 'v' and line[1] == ' ':
                vrows.append(line[2:].split())
            elif line[0] == 'f' and line[1] == ' ':
                frows.append(line[2:].split())
    nv = len(vrows)
    verts = np.zeros((nv, 6), dtype=np.float32)
    for i, tok in enumerate(vrows):
        verts[i, 0] = float(tok[0]); verts[i, 1] = float(tok[1]); verts[i, 2] = float(tok[2])
        if len(tok) >= 6:
            verts[i, 3] = float(tok[3]); verts[i, 4] = float(tok[4]); verts[i, 5] = float(tok[5])
    faces = np.array(frows, dtype=np.int64) - 1
    print(f"  load: {nv} verts, {faces.shape[0]} faces, {time.time()-t0:.1f}s", flush=True)
    return verts, faces


def render(verts, faces, yaw, pitch, size=1536, bg=(24, 28, 34)):
    t0 = time.time()
    cy, sy = np.cos(yaw), np.sin(yaw)
    cp, sp = np.cos(pitch), np.sin(pitch)
    P = verts[:, :3]
    rx = P[:, 0] * cy - P[:, 1] * sy
    ry = P[:, 0] * sy + P[:, 1] * cy
    rz = P[:, 2]
    py = ry * cp - rz * sp
    pz = ry * sp + rz * cp
    sx = rx
    sy2 = -py

    minx, maxx = sx.min(), sx.max()
    miny, maxy = sy2.min(), sy2.max()
    margin = size // 20
    avail = size - 2 * margin
    scale = avail / max(maxx - minx, maxy - miny)
    ox = size * 0.5 - (minx + maxx) * 0.5 * scale
    oy = size * 0.5 - (miny + maxy) * 0.5 * scale
    px = sx * scale + ox
    py2 = sy2 * scale + oy

    # 面法线 + 光照（向量化）
    f = faces
    a = P[f[:, 0]]; b = P[f[:, 1]]; c = P[f[:, 2]]
    n = np.cross(b - a, c - a)
    ln = np.linalg.norm(n, axis=1)
    nz = ln > 1e-9
    n[nz] /= ln[nz, None]
    light = np.array([0.4, 0.6, 0.7], dtype=np.float32)
    light /= np.linalg.norm(light)
    diff = (n * light).sum(1)
    diff = np.clip(diff, 0, 1)
    lum = 0.40 + 0.60 * diff

    col = verts[:, 3:6]
    fc = (col[f[:, 0]] + col[f[:, 1]] + col[f[:, 2]]) / 3.0
    cr = np.clip(fc[:, 0] * lum * 255, 0, 255)
    cg = np.clip(fc[:, 1] * lum * 255, 0, 255)
    cb = np.clip(fc[:, 2] * lum * 255, 0, 255)

    # 三角形质心 + 深度（强制 float32，避免与 z-buffer 精度不一致）
    cx = (px[f[:, 0]] + px[f[:, 1]] + px[f[:, 2]]) / 3.0
    cy2 = (py2[f[:, 0]] + py2[f[:, 1]] + py2[f[:, 2]]) / 3.0
    cz = ((pz[f[:, 0]] + pz[f[:, 1]] + pz[f[:, 2]]) / 3.0).astype(np.float32)

    # 背面剔除（屏幕空间叉积）
    e1 = px[f[:, 1]] - px[f[:, 0]], py2[f[:, 1]] - py2[f[:, 0]]
    e2 = px[f[:, 2]] - px[f[:, 0]], py2[f[:, 2]] - py2[f[:, 0]]
    cross = e1[0] * e2[1] - e1[1] * e2[0]
    keep = cross > 1e-6
    idx = np.where(keep)[0]
    print(f"  cull: {len(idx)}/{faces.shape[0]} front, {time.time()-t0:.1f}s", flush=True)

    gx = np.round(cx[idx]).astype(np.int64)
    gy = np.round(cy2[idx]).astype(np.int64)
    gz = cz[idx]
    inb = (gx >= 0) & (gx < size) & (gy >= 0) & (gy < size)
    gx = gx[inb]; gy = gy[inb]; gz = gz[inb]
    r_ = cr[idx][inb]; g_ = cg[idx][inb]; b_ = cb[idx][inb]

    zbuf = np.full((size, size), -1e30, dtype=np.float32)
    img = np.zeros((size, size, 3), dtype=np.uint8)
    img[:, :] = bg
    np.maximum.at(zbuf, (gy, gx), gz)
    # 用 zbuf 掩码决定哪些点可见（保留最近点）
    vis = zbuf[gy, gx] == gz
    img[gy[vis], gx[vis], 0] = r_[vis].astype(np.uint8)
    img[gy[vis], gx[vis], 1] = g_[vis].astype(np.uint8)
    img[gy[vis], gx[vis], 2] = b_[vis].astype(np.uint8)
    print(f"  splat: {vis.sum()} px, {time.time()-t0:.1f}s", flush=True)
    return Image.fromarray(img)


def main():
    verts, faces = load_obj_fast(OBJ)
    views = [
        ("obj_34",    -0.6,    -0.45),
        ("obj_front",  0.0,     1.5708),
        ("obj_side",   1.5708,  1.5708),
        ("obj_top",   -0.6,     0.0),
        ("obj_rear",   3.14159, 1.5708),
    ]
    for name, yaw, pitch in views:
        im = render(verts, faces, yaw, pitch, size=1536)
        im.save(f"{OUT}/sci_tank_{name}.png")
        print(f"  saved sci_tank_{name}.png", flush=True)


if __name__ == "__main__":
    main()

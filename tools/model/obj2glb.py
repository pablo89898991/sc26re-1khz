"""Valve's Steam Controller parts (OBJ, from the low-poly set) -> one GLB with a mesh per part.
Geometry only (positions, normals, indices); placement and motion are done by web/overlay.html.
usage: obj2glb.py <dir with *.obj> <out.glb>"""
import glob, json, os, struct, sys
src, out = sys.argv[1], sys.argv[2]
bin_ = bytearray(); views = []; accs = []; meshes = []; nodes = []
def add_view(data, target):
    while len(bin_) % 4: bin_.append(0)
    views.append({"buffer": 0, "byteOffset": len(bin_), "byteLength": len(data), "target": target}); bin_.extend(data); return len(views) - 1
for f in sorted(glob.glob(os.path.join(src, "*.obj"))):
    name = os.path.basename(f)[:-4]
    V, N, key, pos, nor, idx = [], [], {}, [], [], []
    for l in open(f):
        t = l.split()
        if not t: continue
        if t[0] == "v": V.append(tuple(map(float, t[1:4])))
        elif t[0] == "vn": N.append(tuple(map(float, t[1:4])))
        elif t[0] == "f":
            poly = []
            for c in t[1:]:
                p = c.split("/"); vi = int(p[0]) - 1; ni = int(p[2]) - 1 if len(p) > 2 and p[2] else -1
                k = (vi, ni)
                if k not in key:
                    key[k] = len(pos); pos.append(V[vi]); nor.append(N[ni] if ni >= 0 else (0.0, 1.0, 0.0))
                poly.append(key[k])
            for i in range(1, len(poly) - 1): idx += [poly[0], poly[i], poly[i + 1]]
    if not idx: continue
    pb = b"".join(struct.pack("<3f", *p) for p in pos); nb = b"".join(struct.pack("<3f", *n) for n in nor)
    ib = struct.pack("<%dH" % len(idx), *idx) if len(pos) < 65536 else struct.pack("<%dI" % len(idx), *idx)
    mn = [min(p[i] for p in pos) for i in range(3)]; mx = [max(p[i] for p in pos) for i in range(3)]
    a0 = len(accs); accs.append({"bufferView": add_view(pb, 34962), "componentType": 5126, "count": len(pos), "type": "VEC3", "min": mn, "max": mx})
    accs.append({"bufferView": add_view(nb, 34962), "componentType": 5126, "count": len(pos), "type": "VEC3"})
    accs.append({"bufferView": add_view(ib, 34963), "componentType": 5123 if len(pos) < 65536 else 5125, "count": len(idx), "type": "SCALAR"})
    meshes.append({"name": name, "primitives": [{"attributes": {"POSITION": a0, "NORMAL": a0 + 1}, "indices": a0 + 2}]})
    nodes.append({"name": name, "mesh": len(meshes) - 1})
while len(bin_) % 4: bin_.append(0)
gltf = {"asset": {"version": "2.0", "generator": "sc26re obj2glb", "copyright": "Valve Corporation, CC BY-NC-SA 4.0"},
        "scene": 0, "scenes": [{"nodes": list(range(len(nodes)))}], "nodes": nodes, "meshes": meshes,
        "accessors": accs, "bufferViews": views, "buffers": [{"byteLength": len(bin_)}]}
js = json.dumps(gltf, separators=(",", ":")).encode()
while len(js) % 4: js += b" "
glb = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bin_)) + struct.pack("<II", len(js), 0x4E4F534A) + js + struct.pack("<II", len(bin_), 0x004E4942) + bin_
open(out, "wb").write(glb); print(out, len(glb), "bytes,", len(nodes), "parts")

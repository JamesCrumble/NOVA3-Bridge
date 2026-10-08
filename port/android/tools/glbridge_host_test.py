#!/usr/bin/env python3
"""PC-side stand-in for the Android GL host: checks the guest's GL bridge stream.

  glbridge_host_test.py CMD_FIFO REPLY_FIFO SECONDS

Reads the command stream (framing, draw/texture payload layouts), answers the synchronous queries with
plausible values so the game keeps going, and prints a summary: ops by count, bytes, frames, protocol errors.
Run from the repo root so that android/tools/glbridge_ops.py is importable.
"""
import collections
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glbridge_ops import OPS  # noqa: E402

EXTENSIONS = ("GL_OES_compressed_ETC1_RGB8_texture GL_EXT_texture_compression_s3tc GL_AMD_compressed_ATC_texture "
              "GL_OES_depth24 GL_OES_packed_depth_stencil GL_EXT_shader_framebuffer_fetch GL_OES_mapbuffer "
              "GL_OES_texture_3D GL_EXT_discard_framebuffer GL_OES_rgb8_rgba8 GL_FAKE_VENDOR_EXTENSION")
STRINGS = {0x1F00: "Test Vendor", 0x1F01: "Test Renderer", 0x1F02: "OpenGL ES 3.2 test",
           0x1F03: EXTENSIONS, 0x8B8C: "OpenGL ES GLSL ES 3.20"}
INTS = {0x0D33: [4096], 0x8869: [16], 0x86A2: [0], 0x0BA2: [0, 0, 1280, 590], 0x0C10: [0, 0, 1280, 590],
        0x0D3A: [4096, 4096], 0x8872: [16], 0x8B4C: [16], 0x8DFB: [224], 0x8DFC: [31], 0x8DFD: [32],
        0x84E8: [4096], 0x8B4D: [96], 0x851C: [16384], 0x0D50: [8], 0x0D52: [8], 0x0D53: [8], 0x0D54: [8],
        0x0D55: [0], 0x0D56: [24], 0x0D57: [8], 0x80A8: [0], 0x80A9: [0], 0x80E8: [3000], 0x80E9: [3000],
        0x8CA6: [0], 0x8894: [0], 0x8895: [0], 0x8B8D: [0], 0x8CDF: [8], 0x8824: [8]}


def main():
    cmd_path, rep_path, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3])
    cmd = os.open(cmd_path, os.O_RDWR)
    rep = os.open(rep_path, os.O_RDWR)
    stream = os.fdopen(cmd, "rb", buffering=1 << 20)

    ops = collections.Counter()
    sizes = collections.Counter()
    errors = []
    frames = 0
    total = 0
    attrib_loc, uniform_loc = {}, {}
    queried = {}
    shader_src, prog_shaders, reflect = {}, {}, {}

    gl_types = {"float": 0x1406, "int": 0x1404, "bool": 0x8B56, "vec2": 0x8B50, "vec3": 0x8B51, "vec4": 0x8B52,
                "ivec2": 0x8B53, "ivec3": 0x8B54, "ivec4": 0x8B55, "mat2": 0x8B5A, "mat3": 0x8B5B,
                "mat4": 0x8B5C, "sampler2D": 0x8B5E, "samplerCube": 0x8B60}
    decl = re.compile(r"^\s*(attribute|uniform)\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*;",
                      re.M)

    def preprocess(src):
        defines, out, stack = {}, [], []

        def active():
            return all(e[2] for e in stack)

        def ev(expr):
            expr = re.sub(r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)",
                          lambda m: "1" if (m.group(1) or m.group(2)) in defines else "0", expr)

            def sub(m):
                v = defines.get(m.group(0))
                if v is None:
                    return "0"
                return v if v.strip() else "1"
            expr = re.sub(r"[A-Za-z_]\w*", sub, expr)
            expr = expr.replace("&&", " and ").replace("||", " or ")
            expr = re.sub(r"!(?!=)", " not ", expr)
            try:
                return bool(eval(expr, {"__builtins__": {}}))
            except Exception:
                return False

        for line in src.split("\n"):
            s = line.strip()
            m = re.match(r"#\s*(\w+)\s*(.*)", s)
            if not m:
                if active():
                    out.append(line)
                continue
            d, rest = m.groups()
            if d in ("ifdef", "ifndef", "if"):
                parent = active()
                if d == "if":
                    c = ev(rest)
                else:
                    name = rest.split()[0] if rest.split() else ""
                    c = (name in defines) == (d == "ifdef")
                c = c and parent
                stack.append([parent, c, c])
            elif d == "elif" and stack:
                e = stack[-1]
                c = (not e[1]) and e[0] and ev(rest)
                e[2] = c
                e[1] = e[1] or c
            elif d == "else" and stack:
                e = stack[-1]
                e[2] = e[0] and not e[1]
                e[1] = True
            elif d == "endif" and stack:
                stack.pop()
            elif d == "define" and active():
                parts = rest.split(None, 1)
                if parts and "(" not in parts[0]:
                    defines[parts[0]] = parts[1] if len(parts) > 1 else ""
            elif d == "undef" and active():
                defines.pop(rest.strip(), None)
        return "\n".join(out)

    def link_program(prog):
        attrs, unis, seen = [], [], set()
        for s in prog_shaders.get(prog, []):
            text = preprocess(re.sub(r"//.*", "", shader_src.get(s, "")))
            for kind, ty, nm, cnt in decl.findall(text):
                used = len(re.findall(r"\b%s\b" % re.escape(nm), text)) > 1
                if not used or (kind, nm) in seen:
                    continue
                seen.add((kind, nm))
                entry = (nm, gl_types.get(ty, 0x1406), int(cnt) if cnt else 1)
                (attrs if kind == "attribute" else unis).append(entry)
        reflect[prog] = (attrs, unis)
    draw_client_blocks = 0
    draw_client_bytes = 0
    draws = 0
    t0 = time.time()
    last = t0

    def reply(data):
        os.write(rep, struct.pack("<I", len(data)) + data)

    while time.time() - t0 < seconds:
        head = stream.read(8)
        if len(head) < 8:
            break
        length, op = struct.unpack("<II", head)
        if length < 8 or length % 4 or length > (256 << 20):
            errors.append("bad length %d for op %d" % (length, op))
            break
        pay = stream.read(length - 8)
        if len(pay) != length - 8:
            errors.append("short payload for op %d" % op)
            break
        total += length
        name = OPS.get(op)
        if name is None:
            errors.append("unknown op %d" % op)
            continue
        ops[name] += 1
        sizes[name] += length

        if name == "OP_SWAP":
            frames += 1
            os.write(rep, struct.pack("<I", 0xFFFFFFFF))
            if time.time() - last > 5:
                last = time.time()
                print("[host] %d frames, %.1f MB, %.1f fps" % (frames, total / 1e6, frames / (last - t0)), flush=True)
        elif name == "OP_DRAW_ARRAYS":
            mode, first, count, nclient = struct.unpack_from("<4i", pay, 0)
            pos = 16
            for _ in range(nclient):
                idx, size, typ, norm, stride, dlen = struct.unpack_from("<6i", pay, pos)
                pos += 24 + ((dlen + 3) & ~3)
                draw_client_blocks += 1
                draw_client_bytes += dlen
            draws += 1
            if pos != len(pay):
                errors.append("DRAW_ARRAYS layout: parsed %d of %d" % (pos, len(pay)))
        elif name == "OP_DRAW_ELEMENTS":
            mode, count, typ, idxmode, idxval, nclient = struct.unpack_from("<6i", pay, 0)
            pos = 24
            for _ in range(nclient):
                idx, size, t2, norm, stride, dlen = struct.unpack_from("<6i", pay, pos)
                pos += 24 + ((dlen + 3) & ~3)
                draw_client_blocks += 1
                draw_client_bytes += dlen
            if idxmode == 1:
                pos += (idxval + 3) & ~3
            draws += 1
            if pos != len(pay):
                errors.append("DRAW_ELEMENTS layout: parsed %d of %d" % (pos, len(pay)))
        elif name in ("OP_TEX_IMAGE_2D", "OP_TEX_SUB_IMAGE_2D"):
            dlen = struct.unpack_from("<i", pay, 32)[0]
            if dlen >= 0 and 36 + dlen > len(pay):
                errors.append("%s data %d overruns payload %d" % (name, dlen, len(pay)))
        elif name == "OP_Q_GET_STRING":
            reply(STRINGS.get(struct.unpack_from("<i", pay, 0)[0], "").encode())
        elif name == "OP_Q_GET_INTEGERV":
            pn = struct.unpack_from("<I", pay, 0)[0]
            vals = INTS.get(pn, [0])
            if pn not in queried:
                queried[pn] = vals
                print("[host] glGetIntegerv(0x%04x) -> %s%s" % (pn, vals, "" if pn in INTS else "  (default)"),
                      flush=True)
            reply(struct.pack("<%di" % len(vals), *vals))
        elif name == "OP_Q_GET_FLOATV":
            reply(struct.pack("<f", 1.0))
        elif name == "OP_Q_GET_SHADERIV":
            pname = struct.unpack_from("<I", pay, 4)[0]
            reply(struct.pack("<i", {0x8B81: 1, 0x8B4F: 0x8B30}.get(pname, 0)))
        elif name == "OP_Q_GET_PROGRAMIV":
            prog, pname = struct.unpack_from("<iI", pay, 0)
            attrs, unis = reflect.get(prog, ([], []))
            val = {0x8B82: 1, 0x8B89: len(attrs), 0x8B86: len(unis),
                   0x8B8A: max([len(a[0]) + 4 for a in attrs] + [1]),
                   0x8B87: max([len(u[0]) + 4 for u in unis] + [1])}.get(pname, 0)
            reply(struct.pack("<i", val))
        elif name in ("OP_Q_SHADER_LOG", "OP_Q_PROGRAM_LOG", "OP_Q_SHADER_SOURCE"):
            reply(b"")
        elif name == "OP_CREATE_SHADER":
            pass
        elif name == "OP_SHADER_SOURCE":
            sid, slen = struct.unpack_from("<ii", pay, 0)
            shader_src[sid] = pay[8:8 + slen].decode("latin-1")
        elif name == "OP_glAttachShader":
            prog, sh = struct.unpack_from("<ii", pay, 0)
            prog_shaders.setdefault(prog, []).append(sh)
        elif name == "OP_glLinkProgram":
            prog = struct.unpack_from("<i", pay, 0)[0]
            link_program(prog)
        elif name in ("OP_Q_ACTIVE_ATTRIB", "OP_Q_ACTIVE_UNIFORM"):
            prog, idx = struct.unpack_from("<ii", pay, 0)
            lst = reflect.get(prog, ([], []))[0 if name == "OP_Q_ACTIVE_ATTRIB" else 1]
            if idx < len(lst):
                nm, ty, sz = lst[idx]
                reply(struct.pack("<ii", sz, ty) + (nm + ("[0]" if sz > 1 else "")).encode())
            else:
                reply(struct.pack("<ii", 0, 0))
        elif name in ("OP_Q_ATTRIB_LOC", "OP_Q_UNIFORM_LOC"):
            prog = struct.unpack_from("<i", pay, 0)[0]
            nm = pay[4:].split(b"\0")[0].decode()
            lst = reflect.get(prog, ([], []))[0 if name == "OP_Q_ATTRIB_LOC" else 1]
            base = nm.split("[")[0]
            loc = next((i for i, e in enumerate(lst) if e[0] == base), -1)
            reply(struct.pack("<i", loc))
        elif name == "OP_Q_CHECK_FB":
            reply(struct.pack("<i", 0x8CD5))
        elif name == "OP_Q_READ_PIXELS":
            x, y, w, h = struct.unpack_from("<4i", pay, 0)
            reply(bytes(max(0, w * h * 4)))

    secs = time.time() - t0
    print("[host] summary: %.1fs, %d frames (%.1f fps), %.1f MB, %d draws, client blocks %d (%.1f KB)" %
          (secs, frames, frames / max(secs, 1e-6), total / 1e6, draws, draw_client_blocks, draw_client_bytes / 1e3))
    for name, n in ops.most_common(25):
        print("[host]   %-34s %7d msgs %9.1f KB" % (name, n, sizes[name] / 1e3))
    print("[host] unique shaders: %d, programs linked: %d" % (len(shader_src), len(reflect)))
    print("[host] protocol errors: %d" % len(errors))
    for e in errors[:10]:
        print("[host]   " + e)
    sys.exit(1 if errors else 0)


main()

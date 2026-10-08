#!/usr/bin/env python3
"""Zip helper for build_apk.sh; the build host has no zip binary.

  ziptool.py dir SRC_DIR OUT.zip
      Deflate SRC_DIR into OUT.zip, entry names starting with SRC_DIR's name.
  ziptool.py append BASE.zip OUT.zip S:name=path D:name=path ...
      Copy BASE.zip as is, then add files stored (S) or deflated (D).
"""
import os
import sys
import zipfile


def pack_dir(src, out):
    parent = os.path.dirname(os.path.abspath(src))
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for base, dirs, files in os.walk(src):
            dirs.sort()
            for name in sorted(files):
                path = os.path.join(base, name)
                z.write(path, os.path.relpath(path, parent))


def append(base, out, items):
    with zipfile.ZipFile(base) as zin, zipfile.ZipFile(out, "w") as zout:
        for info in zin.infolist():
            zout.writestr(info, zin.read(info.filename), compress_type=info.compress_type)
        for item in items:
            mode, spec = item[0], item[2:]
            name, path = spec.split("=", 1)
            kind = zipfile.ZIP_STORED if mode == "S" else zipfile.ZIP_DEFLATED
            zout.write(path, name, compress_type=kind)


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "dir":
        pack_dir(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 4 and sys.argv[1] == "append":
        append(sys.argv[2], sys.argv[3], sys.argv[4:])
    else:
        sys.exit(__doc__)

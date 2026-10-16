#!/usr/bin/env python3
"""Fetch everything a Windows (MSVC, x64) build of the plugin needs besides
the SDR++ source tree.

A PE DLL cannot leave symbols unresolved the way the ELF builds do, so the
plugin must link against an import library for sdrpp_core.dll. Instead of
building all of SDR++ (and its vcpkg/PothosSDR dependency zoo) we take the
official SDR++ Windows build, read the export tables of its DLLs and write
.def files; CMake turns those into import libraries with lib.exe.

SDR++ core is built with CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS, so every core and
ImGui function is exported. The SDR++ release must come from the same commit
as the headers in external/SDRPlusPlus — the nightly build tracks master, as
does scripts/fetch-sdrpp.sh.

SDR++ headers also #include <fftw3.h> and <volk/volk.h>; those headers come
from the conda-forge win-64 packages (.conda = zip + tar.zst, which needs
Python >= 3.14 for the stdlib zstd decoder).

Output layout (default: external/sdrpp-windows):
    sdrpp/                 extracted SDR++ Windows build (for local testing)
    def/<dll>.def          export lists for sdrpp_core, volk, fftw3f
    include/fftw3.h, include/volk/...
"""
import argparse
import io
import os
import shutil
import struct
import sys
import tarfile
import urllib.request
import zipfile

SDRPP_ZIP_URL = os.environ.get(
    "SDRPP_WINDOWS_ZIP_URL",
    "https://github.com/AlexandreRouma/SDRPlusPlus/releases/download/nightly/sdrpp_windows_x64.zip")
CONDA_BASE = "https://conda.anaconda.org/conda-forge/win-64/"
CONDA_PKGS = {
    "volk": "volk-3.3.0-ha79bf38_1.conda",
    "fftw": "fftw-3.3.10-nompi_h6877c38_112.conda",
}
DLLS = ["sdrpp_core.dll", "volk.dll", "fftw3f.dll"]


def download(url):
    print(f"==> GET {url}", flush=True)
    req = urllib.request.Request(url, headers={"User-Agent": "j-alert-plugin-build"})
    with urllib.request.urlopen(req) as r:
        return r.read()


def pe_exports(path):
    """Return the exported names of a PE32+ / PE32 DLL."""
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: not a PE file")
    n_sections = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    exp_rva, _ = struct.unpack_from("<II", data, dd)
    sections = []
    sec = opt + opt_size
    for i in range(n_sections):
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, sec + i * 40 + 8)
        sections.append((va, max(vsize, rsize), raw))

    def off(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError(f"RVA {rva:#x} outside sections")

    if exp_rva == 0:
        return []
    e = off(exp_rva)
    n_names = struct.unpack_from("<I", data, e + 24)[0]
    names_rva = struct.unpack_from("<I", data, e + 32)[0]
    names = []
    for i in range(n_names):
        name_rva = struct.unpack_from("<I", data, off(names_rva) + 4 * i)[0]
        s = off(name_rva)
        names.append(data[s:data.index(b"\0", s)].decode("ascii"))
    return names


def write_def(dll_path, def_path):
    names = pe_exports(dll_path)
    lib = os.path.splitext(os.path.basename(dll_path))[0]
    with open(def_path, "w", newline="\n") as f:
        f.write(f"LIBRARY {lib}\nEXPORTS\n")
        for n in names:
            # lib.exe keeps quotes as part of the symbol, so names go in bare.
            # MSVC-decorated names never contain whitespace.
            if any(c.isspace() for c in n):
                raise ValueError(f"{dll_path}: export name with whitespace: {n!r}")
            f.write(f"    {n}\n")
    print(f"    {os.path.basename(def_path)}: {len(names)} exports")


def extract_conda_include(blob, dest_include):
    import compression.zstd  # Python >= 3.14
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        pkg = next(n for n in z.namelist() if n.startswith("pkg-") and n.endswith(".tar.zst"))
        raw = compression.zstd.decompress(z.read(pkg))
    with tarfile.open(fileobj=io.BytesIO(raw)) as t:
        for m in t.getmembers():
            if not m.isfile() or not m.name.startswith("Library/include/"):
                continue
            rel = m.name[len("Library/include/"):]
            out = os.path.join(dest_include, *rel.split("/"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as f:
                f.write(t.extractfile(m).read())


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=os.path.join(root, "external", "sdrpp-windows"))
    args = ap.parse_args()

    if sys.version_info < (3, 14):
        sys.exit("Python >= 3.14 is required (stdlib zstd for conda packages).")

    out = os.path.abspath(args.out)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(os.path.join(out, "def"))
    os.makedirs(os.path.join(out, "include"))

    with zipfile.ZipFile(io.BytesIO(download(SDRPP_ZIP_URL))) as z:
        z.extractall(os.path.join(out, "sdrpp"))
    sdrpp_dir = os.path.join(out, "sdrpp")
    # The zip has a single top-level folder (sdrpp_windows_x64/).
    entries = os.listdir(sdrpp_dir)
    if len(entries) == 1 and os.path.isdir(os.path.join(sdrpp_dir, entries[0])):
        sdrpp_dir = os.path.join(sdrpp_dir, entries[0])

    print("==> Writing .def files")
    for dll in DLLS:
        write_def(os.path.join(sdrpp_dir, dll),
                  os.path.join(out, "def", os.path.splitext(dll)[0] + ".def"))

    for name, pkg in CONDA_PKGS.items():
        extract_conda_include(download(CONDA_BASE + pkg), os.path.join(out, "include"))

    with open(os.path.join(out, "sdrpp_dir.txt"), "w", newline="\n") as f:
        f.write(os.path.relpath(sdrpp_dir, out).replace("\\", "/") + "\n")
    print(f"==> Windows dependencies ready at {out}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Build the asset baker (tools/asset-studio) for this machine and put it in
tools/packager/bin/, where the packager looks for it.

    python tools/packager/build_baker.py

Needs cmake and a C++17 compiler (MSVC, MinGW or clang on Windows; g++ or
clang on Linux). Ninja is used when it is installed.
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SRC = os.path.join(REPO, "tools", "asset-studio")
BIN = os.path.join(HERE, "bin")
NAME = "AssetStudio.exe" if os.name == "nt" else "AssetStudio"


def run(cmd, env):
    print("+ " + " ".join(cmd))
    r = subprocess.run(cmd, env=env)
    if r.returncode:
        raise SystemExit("failed: %s" % " ".join(cmd))


def main():
    if not shutil.which("cmake"):
        for d in (r"C:\msys64\mingw64\bin", r"C:\Program Files\CMake\bin"):
            if os.path.exists(os.path.join(d, "cmake.exe")):
                os.environ["PATH"] = d + os.pathsep + os.environ["PATH"]
    if not shutil.which("cmake"):
        raise SystemExit("cmake was not found. Install cmake and a C++ compiler first.")
    build = os.path.join(HERE, "build-baker")
    env = dict(os.environ)
    cfg = ["cmake", "-S", SRC, "-B", build, "-DCMAKE_BUILD_TYPE=Release"]
    if shutil.which("ninja"):
        cfg += ["-G", "Ninja"]
    if os.name == "nt" and shutil.which("g++") and not shutil.which("cl"):
        # static, so the exe needs none of MinGW's DLLs on a machine without it
        cfg += ["-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++", "-DCMAKE_EXE_LINKER_FLAGS=-static"]
    run(cfg, env)
    run(["cmake", "--build", build, "--config", "Release", "--target", "AssetStudio"], env)
    exe = None
    for dp, _, fn in os.walk(build):
        if NAME in fn:
            exe = os.path.join(dp, NAME)
            break
    if not exe:
        raise SystemExit("the build finished but %s was not found" % NAME)
    os.makedirs(BIN, exist_ok=True)
    shutil.copyfile(exe, os.path.join(BIN, NAME))
    print("baker: %s" % os.path.join(BIN, NAME))


if __name__ == "__main__":
    main()

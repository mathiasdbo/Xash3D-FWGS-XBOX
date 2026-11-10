"""The Windows version resource of the packager exe (PyInstaller --version-file).

SignPath's artifact configuration (.signpath/artifact-configuration.xml) requires the exe's product name to be
the project's name and its product version to be the release's, so both come from here and from the release
version, never typed twice.
"""
import re

PRODUCT_NAME = "Xash3D-FWGS-XBOX"
DESCRIPTION = "Xash3D-FWGS-XBOX Packager"


def parse(version):
    """'v0.1.1' -> (0, 1, 1, 0); anything that is not up to four dot-separated numbers is refused"""
    m = re.fullmatch(r"v?(\d+(?:\.\d+){0,3})", version.strip())
    if not m:
        raise ValueError("version %r: expected something like v0.1.1" % version)
    nums = [int(x) for x in m.group(1).split(".")]
    if any(n > 65535 for n in nums):
        raise ValueError("version %r: each part must be at most 65535" % version)
    return tuple(nums + [0] * (4 - len(nums)))


def product_version(version):
    """the text of the version without the leading v ('0.1.1'): what the artifact configuration compares"""
    return version.strip().lstrip("v")


def render(version, original_filename):
    v = parse(version)
    dotted = ".".join(str(n) for n in v)
    return """VSVersionInfo(
  ffi=FixedFileInfo(filevers=%(t)s, prodvers=%(t)s, mask=0x3f, flags=0x0, OS=0x40004, fileType=0x1, subtype=0x0, date=(0, 0)),
  kids=[
    StringFileInfo([StringTable('040904B0', [
      StringStruct('CompanyName', '%(prod)s'),
      StringStruct('FileDescription', '%(desc)s'),
      StringStruct('FileVersion', '%(dotted)s'),
      StringStruct('InternalName', 'Xash3D-FWGS-XBOX-Packager'),
      StringStruct('LegalCopyright', 'GPL-3.0-or-later'),
      StringStruct('OriginalFilename', '%(orig)s'),
      StringStruct('ProductName', '%(prod)s'),
      StringStruct('ProductVersion', '%(pv)s')])]),
    VarFileInfo([VarStruct('Translation', [1033, 1200])])
  ])
""" % dict(t=repr(v), prod=PRODUCT_NAME, desc=DESCRIPTION, dotted=dotted, orig=original_filename,
           pv=product_version(version))

"""Install the SessionScope JSFX and scripts into a REAPER resource folder.

    python hosts/reaper/tools/reaper_install.py              link into your REAPER (default resource folder)
    python hosts/reaper/tools/reaper_install.py --copy       copy instead of linking
    python hosts/reaper/tools/reaper_install.py --uninstall  remove what this installed
    python hosts/reaper/tools/reaper_install.py --resource D use another resource folder (portable REAPER)

Linking (a directory junction on Windows, a symlink elsewhere) keeps REAPER on the repository's
files, so edits take effect without reinstalling. Only <resource>/Effects/SessionScope and
<resource>/Scripts/SessionScope are created; anything already there that this tool did not make
is left alone.

Then in REAPER: Actions > Show action list > New action > Load ReaScript, and pick the three
"SessionScope - ..." scripts in Scripts/SessionScope.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
PARTS = (("Effects", "SessionScope"), ("Scripts", "SessionScope"))
MARKER = ".sessionscope-install"


def default_resource() -> Path:
    if sys.platform == "win32":
        return Path(os.environ.get("APPDATA", Path.home())) / "REAPER"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "REAPER"
    return Path.home() / ".config" / "REAPER"


def is_link(p: Path) -> bool:
    """A symlink, or on Windows a directory junction (a reparse point)."""
    if p.is_symlink():
        return True
    if hasattr(os.path, "isjunction"):
        return os.path.isjunction(p)
    try:
        return bool(os.lstat(p).st_file_attributes & 0x400)
    except (OSError, AttributeError):
        return False


def ours(p: Path) -> bool:
    if is_link(p):
        return Path(os.path.realpath(p)).is_relative_to(REPO.resolve())
    return (p / MARKER).exists()


def link_dir(src: Path, dst: Path):
    if sys.platform == "win32":
        import _winapi
        _winapi.CreateJunction(str(src), str(dst))
    else:
        dst.symlink_to(src, target_is_directory=True)


def remove(p: Path):
    if is_link(p):
        os.rmdir(p) if sys.platform == "win32" else p.unlink()
    else:
        shutil.rmtree(p)


def install(resource: Path, copy=False, quiet=False):
    for kind, name in PARTS:
        src = REPO / "hosts" / "reaper" / kind / name
        dst = resource / kind / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        if dst.exists() or is_link(dst):
            if not ours(dst):
                raise SystemExit(f"{dst} exists and was not installed by SessionScope; move it away first.")
            remove(dst)
        if copy:
            shutil.copytree(src, dst)
            (dst / MARKER).write_text(str(REPO), encoding="utf-8")
        else:
            link_dir(src, dst)
        if not quiet:
            print(f"{'copied' if copy else 'linked'} {dst}")


def uninstall(resource: Path):
    for kind, name in PARTS:
        dst = resource / kind / name
        if dst.exists() or is_link(dst):
            if not ours(dst):
                print(f"left {dst} alone: not installed by SessionScope")
                continue
            remove(dst)
            print(f"removed {dst}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--resource", type=Path, default=None, help="REAPER resource folder")
    ap.add_argument("--copy", action="store_true", help="copy files instead of linking")
    ap.add_argument("--uninstall", action="store_true")
    args = ap.parse_args(argv)
    resource = args.resource or default_resource()
    if not resource.is_dir():
        raise SystemExit(f"No REAPER resource folder at {resource}; pass --resource.")
    if args.uninstall:
        uninstall(resource)
    else:
        install(resource, args.copy)
        print("In REAPER: Actions > Show action list > New action > Load ReaScript, then pick the SessionScope scripts.")


if __name__ == "__main__":
    main()

r"""Install the SessionScope Sensor in every format into the folders plug-in hosts search.

    python tools/install_plugins.py           # per-user folders, then (if allowed) the shared ones
    python tools/install_plugins.py --uninstall

Windows: VST3 and CLAP. macOS: VST3, CLAP and Audio Unit. Each format has a shared folder, its
standard location, which needs administrator rights (an administrator terminal on Windows, sudo on
macOS), and a per-user folder, which needs none but not every host scans. The folders are listed in
tools/install_paths.py.
"""

import argparse
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import install_paths as install  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--uninstall", action="store_true")
    args = ap.parse_args()

    copies = install.plugin_copies()
    if not args.uninstall and not all(built.exists() for _, built, _, _ in copies):
        sys.exit("Build it first: python tools/build_native.py")

    for name, built, target, remove in copies:
        if args.uninstall:
            if remove.is_dir():
                shutil.rmtree(remove)
                print(f"removed {remove}")
            elif remove.exists():
                remove.unlink()
                print(f"removed {remove}")
            continue
        try:
            install.copy_plugin(built, target)
            print(f"{name}: installed {target}")
        except OSError as e:
            print(f"{name}: skipped {target}: {e.strerror or e}")


if __name__ == "__main__":
    main()

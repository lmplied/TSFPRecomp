"""
py -3 -m tools.input_ui [config path] [--title ID] [--print] [--write-defaults]

With no arguments it opens the binding UI on the per-user config every title
shares (or the file RECOMP_INPUT_CONFIG names). --title edits one title's own
bindings instead, the file the launcher writes and the runtime reads first
(--title 45410066 for TimeSplitters: Future Perfect). --print writes the config
that would be loaded to stdout and --write-defaults writes a default one, both
so the file can be looked at, diffed or generated on a machine with no display.
"""

import argparse
import json
import sys

from . import bindings


def main(argv=None):
    parser = argparse.ArgumentParser(prog="tools.input_ui", description=__doc__)
    parser.add_argument("path", nargs="?", help="config file to edit")
    parser.add_argument("--title", type=lambda text: int(text, 16),
                        help="a title id in hex: edit that title's own bindings")
    parser.add_argument("--print", dest="show", action="store_true",
                        help="print the config that would be loaded and exit")
    parser.add_argument("--write-defaults", action="store_true",
                        help="write a default config and exit")
    args = parser.parse_args(argv)
    path = args.path or bindings.config_path(args.title)

    if args.show:
        json.dump(bindings.load(path), sys.stdout, indent=2)
        sys.stdout.write("\n")
        return 0
    if args.write_defaults:
        print("wrote %s" % bindings.save(bindings.default_config(), path))
        return 0

    try:
        from . import app
    except ImportError as error:                       # tkinter is optional
        print("the UI needs tkinter: %s" % error, file=sys.stderr)
        return 2
    return app.main([path])


if __name__ == "__main__":
    raise SystemExit(main())

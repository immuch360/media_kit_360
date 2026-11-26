#!/usr/bin/env python3
"""Immuch360 (IMMUCH360-NOTE.md, patch 18): writes SpatialMetalShaders.swift from SpatialShaders.metal.

The Metal source of the Spatial passes is the one of the app's iOS player (mobile/ios/Runner/Spatial/
SpatialShaders.metal of github.com/freeKC/Immuch360), copied here as it is. The plugin compiles it when the passes
are first turned on (MTLDevice.makeLibrary(source:)), so that neither Swift Package Manager nor CocoaPods has to
build a .metal file. Run from this folder:

    python3 embed_shaders.py           # writes the Swift file
    python3 embed_shaders.py --check   # fails when the Swift file is not the one this script writes
"""

import hashlib
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
SOURCE = HERE / "SpatialShaders.metal"
TARGET = (
    HERE.parent / "media_kit_video" / "Sources" / "media_kit_video" / "plugin" / "spatial" / "SpatialMetalShaders.swift"
)


def swift_text() -> str:
    metal = SOURCE.read_text(encoding="utf-8")
    if '"""#' in metal:
        raise SystemExit("SpatialShaders.metal holds the end of a Swift raw string")
    digest = hashlib.sha256(metal.encode("utf-8")).hexdigest()
    return (
        "// Immuch360 (IMMUCH360-NOTE.md, patch 18): written by macos/spatial/embed_shaders.py from\n"
        "// macos/spatial/SpatialShaders.metal, the Metal source of the app's iOS Spatial player. Do not edit by hand.\n"
        f"// SHA-256 of the Metal source: {digest}\n"
        "\n"
        "enum SpatialMetalShaders {\n"
        f'  static let sha256 = "{digest}"\n'
        "\n"
        '  static let source = #"""\n'
        f"{metal}"
        '"""#\n'
        "}\n"
    )


def main() -> int:
    text = swift_text()
    if "--check" in sys.argv[1:]:
        current = TARGET.read_text(encoding="utf-8") if TARGET.exists() else ""
        if current != text:
            print(f"{TARGET.name} is not the one embed_shaders.py writes from {SOURCE.name}: run the script")
            return 1
        print(f"{TARGET.name} matches {SOURCE.name}")
        return 0
    TARGET.write_text(text, encoding="utf-8")
    print(f"wrote {TARGET}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

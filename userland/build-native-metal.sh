#!/bin/bash
# Compile and stage the tested Native Metal integration into an existing RAM root.
# Apple framework resources and the external Native/Vulkan binaries are supplied
# locally by the builder. This script never operates on a device.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
source ../xnu-env.sh
native_root=${1:-root}
native_extensions=${NATIVE_EXTENSIONS:-../prebuilt/Extensions}
native_bundle=NativeAdrenoMetal.bundle
native_sdk="${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk"
native_cc=(xnu-clang -arch arm64 -isysroot "$native_sdk" -O1 -Wall -Wextra)
mkdir -p obj "$native_root/bin" "$native_root/lib" "$native_root/etc"
# Verify the exact original plugin and Vulkan pair before staging anything.
python3 ../tools/native-metal-variant.py "$native_extensions/$native_bundle/NativeAdrenoMetal" obj/NativeAdrenoMetal.corvette
python3 - "$native_extensions/$native_bundle/libvulkan_freedreno.dylib" <<'PY'
from pathlib import Path
import hashlib,sys
if hashlib.sha256(Path(sys.argv[1]).read_bytes()).hexdigest() != '8d7b560fa2b2b8114321a5c236565776f51ee96df437220adbb3e8a2163a8725':
    raise SystemExit('unreviewed Vulkan dependency')
PY
"${native_cc[@]}" -o obj/cache_probe src/cache_probe.c
"${native_cc[@]}" -o obj/system_watch src/system_watch.c
"${native_cc[@]}" -dynamiclib -install_name /redwood/lib/libhakios.dylib \
    -framework CoreFoundation -framework IOKit -lobjc -o obj/libhakios.dylib src/inject.c
"${native_cc[@]}" -dynamiclib -install_name /redwood/lib/libhakios.sb.dylib \
    -framework CoreFoundation -framework IOKit -lobjc -o obj/libhakios.sb.dylib src/inject_sb.c
"${native_cc[@]}" -fobjc-arc -dynamiclib -install_name /redwood/lib/libhakios.metal-source.dylib \
    -framework Foundation -framework Metal -framework IOKit -o obj/libhakios.metal-source.dylib \
    src/metal_source_autoload.m src/metal_system_bridge.m
cp obj/cache_probe obj/system_watch "$native_root/bin/"
cp obj/libhakios.dylib obj/libhakios.sb.dylib obj/libhakios.metal-source.dylib "$native_root/lib/"
mkdir -p "$native_root/Extensions"
cp -a "$native_extensions/." "$native_root/Extensions/"
cp obj/NativeAdrenoMetal.corvette "$native_root/Extensions/$native_bundle/NativeAdrenoMetal"
# Keep the tested clients list. No wildcard exec-injection expansion is supported.
python3 - "$native_root/etc/inject.conf" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]); lines=p.read_text().splitlines() if p.exists() else []
prefix='/redwood/lib/libhakios.metal-source.dylib'
lines=[line for line in lines if not line.startswith(prefix)]
lines.append(prefix+' metal_rgba16 metal_vec3 metal_smoke metal_test metal_panel backboardd SpringBoard mediaserverd chronod')
p.write_text('\n'.join(lines)+'\n')
PY
# Optional original resource trees, laid out as in the RAM root. No redistribution.
if [ -n "${METAL_RESOURCES:-}" ]; then
    for native_dir in FrameworkResources PrivateFrameworkResources; do
        test -d "$METAL_RESOURCES/$native_dir"
        mkdir -p "$native_root/$native_dir"
        cp -a "$METAL_RESOURCES/$native_dir/." "$native_root/$native_dir/"
    done
fi
# Fail explicitly rather than package an accelerated UI without its real shaders.
for native_resource in QuartzCore.framework/default.metallib CoreImage.framework/default.metallib \
    MetalPerformanceShaders.framework/Frameworks/MPSCore.framework/default.metallib \
    MetalPerformanceShaders.framework/Frameworks/MPSImage.framework/default.metallib \
    MetalPerformanceShaders.framework/Frameworks/MPSFunctions.framework/default.metallib \
    MetalPerformanceShaders.framework/Frameworks/MPSMatrix.framework/default.metallib; do
    test -s "$native_root/FrameworkResources/$native_resource" || {
        echo "Native Metal needs original iOS resources: set METAL_RESOURCES (see docs/corvette-gpu.md)" >&2
        exit 1
    }
done
test -s "$native_root/PrivateFrameworkResources/RenderBox.framework/default.metallib" || {
    echo "Native Metal needs the original RenderBox default.metallib" >&2
    exit 1
}
# Hand-run diagnostics only: no launchd RunAtLoad tests or timed halt.
if [ "${METAL_TESTS:-0}" = 1 ]; then
    for native_test in metal_smoke metal_panel metal_vec3 metal_rgba16; do
        "${native_cc[@]}" -fobjc-arc -framework Foundation -framework Metal \
            -framework IOKit -framework IOSurface -o "obj/$native_test" "src/$native_test.m"
        cp "obj/$native_test" "$native_root/bin/"
    done
fi

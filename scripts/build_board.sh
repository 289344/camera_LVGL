#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TC_PREFIX="/usr/local/arm/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-"
CC="${TC_PREFIX}gcc"
CXX="${TC_PREFIX}g++"
BUILD_DIR="$ROOT/build_board"
OBJ_DIR="$BUILD_DIR/obj"
TARGET_LIB="$BUILD_DIR/target_lib"
TARGET_INC="$BUILD_DIR/target_include"

mkdir -p "$OBJ_DIR" "$BUILD_DIR" "$TARGET_LIB" "$TARGET_INC/freetype2"

# 首次构建时从板端同步依赖库，并从 PC 复制架构无关头文件。
if [[ ! -f "$TARGET_LIB/libfreetype.so.6" || ! -f "$TARGET_INC/jpeglib.h" ]]; then
    echo "SYNC target libs/headers"
    for f in libfreetype.so libfreetype.so.6 libfreetype.so.6.17.1 \
             libjpeg.so libjpeg.so.8 libjpeg.so.8.2.2 \
             libz.so libz.so.1 libz.so.1.2.11 \
             libbz2.so libbz2.so.1.0 libbz2.so.1.0.8 \
             libpng.so libpng16.so libpng16.so.16 libpng16.so.16.37.0; do
        adb pull "/usr/lib/$f" "$TARGET_LIB/$f" >/dev/null 2>&1 || true
    done
    cp -a /usr/include/freetype2/. "$TARGET_INC/freetype2/"
    cp /usr/include/jpeglib.h /usr/include/jmorecfg.h /usr/include/jerror.h /usr/include/jpegint.h "$TARGET_INC/"
    if [[ -f /usr/include/x86_64-linux-gnu/jconfig.h ]]; then
        cp /usr/include/x86_64-linux-gnu/jconfig.h "$TARGET_INC/jconfig.h"
    else
        cp /usr/include/jconfig.h "$TARGET_INC/jconfig.h"
    fi
fi

CFLAGS=(
  -std=gnu99 -O2 -Wall -Wextra -pthread -MMD -MP
  -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard
  -I"$ROOT"
  -I"$ROOT/third_party/lvgl"
  -I"$TARGET_INC/freetype2"
  -I"$TARGET_INC"
  -DLV_CONF_INCLUDE_SIMPLE
  -DCAMERA_LVGL_BOARD
)

CXXFLAGS=(
  -std=c++14 -O2 -Wall -Wextra -pthread -MMD -MP
  -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard
  -I"$ROOT/include"
  -I"$ROOT/src"
  -I"$ROOT"
  -I"$ROOT/third_party/lvgl"
  -I"$TARGET_INC/freetype2"
  -I"$TARGET_INC"
  -DLV_CONF_INCLUDE_SIMPLE
  -DCAMERA_LVGL_BOARD
)

OBJECTS=()

compile_c() {
  local src="$1"
  local rel="${src#$ROOT/}"
  local obj="$OBJ_DIR/${rel//\//_}.o"
  local dep="$obj.d"
  local needs_build=1
  # .d 只有依赖而没有 recipe；补一个实际 recipe，让 make -q 正确检测过期目标。
  if [[ -f "$obj" && -f "$dep" ]] && make -rR -q -f "$dep" --eval="$obj: ; @:" "$obj" >/dev/null 2>&1; then
    needs_build=0
  fi
  if [[ "$needs_build" -eq 1 ]]; then
    echo "CC   $rel"
    "$CC" "${CFLAGS[@]}" -MF "$dep" -c "$src" -o "$obj"
  fi
  OBJECTS+=("$obj")
}

compile_cxx() {
  local src="$1"
  local rel="${src#$ROOT/}"
  local obj="$OBJ_DIR/${rel//\//_}.o"
  local dep="$obj.d"
  local needs_build=1
  if [[ -f "$obj" && -f "$dep" ]] && make -rR -q -f "$dep" --eval="$obj: ; @:" "$obj" >/dev/null 2>&1; then
    needs_build=0
  fi
  if [[ "$needs_build" -eq 1 ]]; then
    echo "CXX  $rel"
    "$CXX" "${CXXFLAGS[@]}" -MF "$dep" -c "$src" -o "$obj"
  fi
  OBJECTS+=("$obj")
}

# LVGL C sources
while IFS= read -r src; do
  compile_c "$src"
done < <(find "$ROOT/third_party/lvgl/src" -name '*.c' | sort)

# Application C++ sources
while IFS= read -r src; do
  compile_cxx "$src"
done < <(find "$ROOT/src" -name '*.cpp' | sort)

echo "LINK build_board/camera_lvgl"
"$CXX" "${CXXFLAGS[@]}" "${OBJECTS[@]}" \
  -L"$TARGET_LIB" \
  -Wl,-rpath-link,"$TARGET_LIB" \
  -Wl,--allow-shlib-undefined \
  -lfreetype -ljpeg -lpthread -lm -ldl -lrt \
  -o "$BUILD_DIR/camera_lvgl"

echo "Built: $BUILD_DIR/camera_lvgl"
"${TC_PREFIX}strip" "$BUILD_DIR/camera_lvgl"
echo "Stripped: $BUILD_DIR/camera_lvgl"

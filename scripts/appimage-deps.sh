#!/bin/bash
#
# Build environment and third-party libraries for the Linux AppImage (Ubuntu 22.04).
#
#   scripts/appimage-deps.sh setup [debs]      install the distro build packages, reusing
#                                              and refreshing downloaded .deb files in [debs]
#   scripts/appimage-deps.sh build <stage>     build the libraries and install them
#   scripts/appimage-deps.sh restore <stage>   install a stage saved by an earlier build
#
# CI caches <stage> keyed on a hash of this file and of the libplacebo patches, so
# everything that decides what these libraries contain (refs, configure flags and
# the distro packages they detect at configure time) must live here, not in the
# workflow. Change anything below and the next run rebuilds the cache.
#
# Every library is installed into <stage> with DESTDIR (prefix /usr/local) and then
# copied into /usr/local, so the next library builds against it exactly as before.
# A cache hit copies the same files into /usr/local without rebuilding them.

set -euo pipefail

SDL3_REF=release-3.4.14
SDL2_COMPAT_REF=release-2.32.70
SDL_TTF_REF=a883e490e30fb44a5336ea3dcb990c6982c5216f
LIBVA_REF=2.24.1
LIBPLACEBO_REF=4d82c6898551068d4ae6a6b5538efcddc2c7cf64
DAV1D_VER=1.5.4
FFMPEG_REF=n9.0

SOURCE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEPS_DIR=$SOURCE_ROOT/deps

setup()
{
  local debs=${1:-}
  if [ -n "$debs" ] && compgen -G "$debs/*.deb" >/dev/null; then
    sudo cp "$debs"/*.deb /var/cache/apt/archives/
  fi
  wget -qO - https://packages.lunarg.com/lunarg-signing-key-pub.asc | sudo apt-key add -
  sudo wget -qO /etc/apt/sources.list.d/lunarg-vulkan-1.4.313-jammy.list https://packages.lunarg.com/vulkan/1.4.313/lunarg-vulkan-1.4.313-jammy.list
  sudo apt update
  sudo apt install -y qt6-base-dev qt6-declarative-dev libqt6svg6-dev qml6-module-qtquick-controls qml6-module-qtquick-templates qml6-module-qtquick-layouts \
    qml6-module-qtqml-workerscript qml6-module-qtquick-window qml6-module-qtquick python3-pip nasm libgbm-dev libdrm-dev libfreetype-dev libasound2-dev \
    libdbus-1-dev libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libglu1-mesa-dev libibus-1.0-dev libpulse-dev libudev-dev libx11-dev libxcursor-dev \
    libxext-dev libxi-dev libxinerama-dev libxkbcommon-dev libxrandr-dev libxss-dev libxt-dev libxv-dev libxxf86vm-dev libxcb-dri3-dev libx11-xcb-dev \
    libxfixes-dev libxtst-dev wayland-protocols libopus-dev libvdpau-dev libgl-dev libpipewire-0.3-dev liburing-dev vulkan-sdk cmake
  sudo pip3 install meson
  if [ -n "$debs" ]; then
    mkdir -p "$debs"
    if compgen -G "/var/cache/apt/archives/*.deb" >/dev/null; then
      cp /var/cache/apt/archives/*.deb "$debs"/
    fi
    echo "$(ls "$debs" | wc -l) downloaded packages kept in $debs"
  fi
}

# fetch <dir> <github repo> <tag or commit> [--submodules]
# Same result as actions/checkout: a depth-1 checkout where a tag ref is also
# fetched as a local tag, so version scripts that run "git describe" see it.
fetch()
{
  local dir=$DEPS_DIR/$1 url=https://github.com/$2.git ref=$3 refspec
  if [[ $ref =~ ^[0-9a-f]{40}$ ]]; then
    refspec=$ref
  else
    refspec=+refs/tags/$ref:refs/tags/$ref
  fi
  git init -q "$dir"
  git -C "$dir" fetch -q --no-tags --depth=1 "$url" "$refspec"
  git -C "$dir" -c advice.detachedHead=false checkout -q --force FETCH_HEAD
  if [ "${4:-}" = "--submodules" ]; then
    git -C "$dir" submodule sync -q --recursive
    git -C "$dir" submodule update -q --init --force --depth=1 --recursive
  fi
}

# Copy the stage into the live prefix, as "sudo make install" used to do. Files end up
# owned by root; directories that already exist (such as /usr/local/bin, which the
# runner user can write to) keep their owner and mode.
sync_stage()
{
  local stray
  stray=$(cd "$STAGE" && find . -mindepth 1 -maxdepth 3 ! -path ./usr ! -path ./usr/local ! -path './usr/local/*')
  [ -z "$stray" ] || { echo "Installed outside /usr/local: $stray" >&2; exit 1; }
  tar -C "$STAGE/usr/local" -cf - . | sudo tar -C /usr/local -xf - --no-overwrite-dir --no-same-owner
  sudo ldconfig
}

begin() { echo "::group::$1"; SECONDS=0; }
end() { echo "::endgroup::"; echo "$1 took ${SECONDS}s"; }

build()
{
  mkdir -p "$DEPS_DIR" "$STAGE"

  begin SDL3
  fetch SDL libsdl-org/SDL $SDL3_REF
  pushd "$DEPS_DIR/SDL"
  cmake -DSDL_KMSDRM=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_INSTALL_DOCS=OFF -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -S . -B build
  cmake --build build -j
  DESTDIR=$STAGE cmake --install build
  popd
  sync_stage
  end SDL3

  begin sdl2-compat
  fetch sdl2-compat libsdl-org/sdl2-compat $SDL2_COMPAT_REF
  pushd "$DEPS_DIR/sdl2-compat"
  cmake -DSDL2COMPAT_TESTS=OFF -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -S . -B build
  cmake --build build -j
  DESTDIR=$STAGE cmake --install build
  popd
  sync_stage
  end sdl2-compat

  begin SDL_ttf
  fetch SDL_ttf libsdl-org/SDL_ttf $SDL_TTF_REF --submodules
  pushd "$DEPS_DIR/SDL_ttf"
  ./autogen.sh
  ./configure
  make -j$(nproc)
  make install DESTDIR="$STAGE"
  popd
  sync_stage
  end SDL_ttf

  begin libva
  fetch libva intel/libva $LIBVA_REF
  pushd "$DEPS_DIR/libva"
  ./autogen.sh
  ./configure --enable-x11
  make -j$(nproc)
  make install DESTDIR="$STAGE"
  popd
  sync_stage
  end libva

  begin libplacebo
  fetch libplacebo haasn/libplacebo $LIBPLACEBO_REF --submodules
  pushd "$DEPS_DIR/libplacebo"
  git apply "$SOURCE_ROOT"/app/deploy/linux/appimage/*.patch
  meson setup build -Dvulkan=enabled -Dopengl=disabled -Ddemos=false
  ninja -C build
  DESTDIR=$STAGE ninja install -C build
  popd
  sync_stage
  end libplacebo

  begin dav1d
  pushd "$DEPS_DIR"
  git clone --branch $DAV1D_VER --depth 1 https://code.videolan.org/videolan/dav1d.git
  cd dav1d
  meson setup build -Ddefault_library=static -Dbuildtype=release -Denable_tools=false -Denable_tests=false
  ninja -C build
  DESTDIR=$STAGE ninja install -C build
  popd
  sync_stage
  end dav1d

  begin FFmpeg
  fetch FFmpeg FFmpeg/FFmpeg $FFMPEG_REF
  pushd "$DEPS_DIR/FFmpeg"
  ./configure --enable-pic --enable-lto --disable-static --enable-shared --disable-all --disable-autodetect \
    --enable-avcodec --enable-avformat --enable-swscale \
    --enable-decoder=h264 --enable-decoder=hevc --enable-decoder=av1 \
    --enable-vaapi --enable-hwaccel=h264_vaapi --enable-hwaccel=hevc_vaapi --enable-hwaccel=av1_vaapi \
    --enable-vdpau --enable-hwaccel=h264_vdpau  --enable-hwaccel=hevc_vdpau --enable-hwaccel=av1_vdpau \
    --enable-libdrm --enable-vulkan --enable-hwaccel=h264_vulkan --enable-hwaccel=hevc_vulkan --enable-hwaccel=av1_vulkan \
    --enable-libdav1d --enable-decoder=libdav1d
  make -j$(nproc)
  make install DESTDIR="$STAGE"
  popd
  sync_stage
  end FFmpeg
}

case "${1:-}" in
  setup)
    setup "${2:-}"
    ;;
  build)
    STAGE=$(realpath -m "${2:?stage directory required}")
    build
    ;;
  restore)
    STAGE=$(realpath -m "${2:?stage directory required}")
    [ -d "$STAGE/usr/local" ] || { echo "No staged libraries in $STAGE" >&2; exit 1; }
    sync_stage
    ;;
  *)
    echo "usage: $0 setup | build <stage> | restore <stage>" >&2
    exit 1
    ;;
esac

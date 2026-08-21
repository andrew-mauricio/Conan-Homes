#!/bin/bash
# Builds the plugin. Nothing beyond the MinGW cross-compiler is needed: nobody
# from the community needs Visual Studio or the Unreal editor.
#
# THERE IS NOTHING OF OURS TO LINK. This line used to pull in a static library
# and drag the whole runtime into the plugin's DLL. Under the table model the
# only file of ours that takes part is the header `Conan/ConanPluginApi.h`,
# which is declarations only, so all that is left here is `-I`.
#
# INCLUDES ARE SEARCHED FOR, NEVER ASSUMED. This same file runs in two places
# with different shapes: the development tree (plugins/X/) and the SDK the
# community downloads (Exemplos/X/). It used to carry only the tree's path, so
# the FIRST command in the guide died with "ConanPluginApi.h: No such file" for
# everyone who downloaded it, while passing in here.
#
# ConanPermission.h deserves its own search because it lives in a DIFFERENT
# place in each: in the SDK it sits with the other headers; in the tree it stays
# with the plugin that publishes it (plugins/Permission/include). This script was
# once rewritten forgetting that, which broke ConanHomes in the tree without
# breaking it in the package.
set -e
AQUI="$(cd "$(dirname "$0")" && pwd)"

INC=""
# First whatever the developer pointed at by hand: that outranks any heuristic.
if [ -n "${CONAN_SDK_INCLUDE:-}" ] && [ -f "$CONAN_SDK_INCLUDE/Conan/ConanPluginApi.h" ]; then
    INC="$CONAN_SDK_INCLUDE"
else
    # Walks up the tree looking for it. Covers the example's folder (Exemplos/X/),
    # a plugin copied to the SDK root (the recipe in the guide), and any depth a
    # developer invents, with no list of paths to go stale.
    d="$AQUI"
    for _ in 1 2 3 4 5 6; do
        for c in "$d/include" "$d/api/include"; do
            [ -f "$c/Conan/ConanPluginApi.h" ] && { INC="$c"; break 2; }
        done
        d="$(dirname "$d")"
        [ "$d" = "/" ] && break
    done
fi
if [ -z "$INC" ]; then
    echo "  x could not find Conan/ConanPluginApi.h. Searched upwards from: $AQUI"
    echo "    Point at it with:  CONAN_SDK_INCLUDE=/path/to/sdk/include ./compilar.sh"
    exit 1
fi

# A second -I only when Permission's header is NOT alongside the main one.
PERM=""
if [ ! -f "$INC/Conan/ConanPermission.h" ]; then
    for c in "$AQUI/../Permission/include" "$AQUI/../../plugins/Permission/include" \
             "$AQUI/../../Exemplos/Permission/include"; do
        [ -f "$c/Conan/ConanPermission.h" ] && { PERM="-I $c"; break; }
    done
fi

# --exclude-all-symbols: the DLL exports the two entry points and nothing else.
#   Without it MinGW publishes every global it feels like publishing, and another
#   plugin doing GetProcAddress could grab ours.
#
# --no-insert-timestamp: without it the linker stamps the current time into the
#   PE header, two builds of identical source produce different files, and
#   "check the hash against the release" becomes theatre. The whole trust model
#   of this project is a server owner being able to rebuild and compare.
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -shared \
    -I "$INC" $PERM \
    -o "$AQUI/ConanHomes.dll" \
    "$AQUI/ConanHomes.cpp" \
    -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup \
    -Wl,--exclude-all-symbols \
    -Wl,--no-insert-timestamp
echo "  ✅ $AQUI/ConanHomes.dll"

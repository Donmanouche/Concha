#!/bin/sh
# Regenerates resources/strudeldirt/synthdefs from third_party/strudeldirt with the installed sclang
# (SuperCollider 3.13) and the Vowel quark, with a class library of its own (not the user's).
# usage: tools/sc/compile_synthdefs.sh [path to the Vowel quark]
set -e
here=$(cd "$(dirname "$0")/../.." && pwd)
vowel=${1:-$HOME/.local/share/SuperCollider/downloaded-quarks/Vowel}
conf=$(mktemp --suffix=.yaml)
sed -e "s|@VOWEL@|$vowel|" -e "s|@STRUDELDIRT@|$here/third_party/strudeldirt|" "$here/tools/sc/sclang_conf.yaml.in" > "$conf"
out="$here/resources/strudeldirt/synthdefs"
rm -rf "$out"
# an empty config folder: sclang skips the user's startup.scd (which may boot a server and SuperDirt)
config=$(mktemp -d)
XDG_CONFIG_HOME="$config" QT_QPA_PLATFORM=offscreen sclang -l "$conf" "$here/tools/sc/compile_synthdefs.scd" "$out"
rm -rf "$conf" "$config"

#!/bin/sh
# 在开发板运行。不要在 Ubuntu 桌面执行这个启动脚本。
set -eu
BASE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
[ -x "$BASE/car-terminal" ] || { echo "缺少程序：$BASE/car-terminal"; exit 1; }
[ -r "$BASE/car-offline.ini" ] || { echo "缺少配置：$BASE/car-offline.ini"; exit 1; }
[ -r "$BASE/fonts/wqy-microhei.ttc" ] || { echo "缺少中文字体，参见 README"; exit 1; }
[ -e /dev/fb0 ] || { echo "没有 /dev/fb0"; exit 1; }
[ -e /dev/input/event1 ] || { echo "没有 event1，请检查触摸节点"; exit 1; }
command -v amixer >/dev/null
# 打开音频路由；具体百分比由 Qt 在启动后通过 amixer -M 设置。
amixer -c 0 sset 'Left Output Mixer PCM' on
amixer -c 0 sset 'Right Output Mixer PCM' on
export LANG=C.UTF-8
export LC_ALL=C.UTF-8
export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=/dev/input/event1
export QT_QPA_FONTDIR="$BASE/fonts"
export TZ=CST-8
exec "$BASE/car-terminal" "$BASE/car-offline.ini"

#!/bin/sh
# Ubuntu 执行：sh scripts/deploy.sh ARM可执行文件 新NFS根目录
set -eu
[ "$#" -eq 2 ] || { echo "用法：$0 ARM程序 NFS根目录"; exit 1; }
BIN=$1
ROOT=$2
SRC=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ -f "$BIN" ] || { echo "找不到可执行文件：$BIN"; exit 1; }
[ -d "$ROOT/etc" ] && [ -d "$ROOT/usr" ] || { echo "该目录不像已解压的根文件系统"; exit 1; }
file "$BIN" | grep -q 'ELF 32-bit.*ARM' || { echo "不是 ARM 32 位 ELF，停止部署"; exit 1; }
FONT=/usr/share/fonts/truetype/wqy/wqy-microhei.ttc
[ -r "$FONT" ] || { echo "请先在 Ubuntu 安装 fonts-wqy-microhei"; exit 1; }
DEST="$ROOT/opt/car-terminal"
# 请先在开发板退出旧程序，再执行部署。音乐文件不会被删除。
install -d "$DEST/fonts" "$DEST/music/ready"
if [ -f "$DEST/car-terminal" ]; then cp -p "$DEST/car-terminal" "$DEST/car-terminal.previous"; fi
install -m 755 "$BIN" "$DEST/car-terminal"
install -m 755 "$SRC/scripts/start.sh" "$DEST/start-car.sh"
install -m 644 "$FONT" "$DEST/fonts/wqy-microhei.ttc"
if [ ! -e "$DEST/car-offline.ini" ]; then
    install -m 644 "$SRC/config/car.ini" "$DEST/car-offline.ini"
else
    install -m 644 "$SRC/config/car.ini" "$DEST/car-offline.ini.new"
    echo "保留原 car-offline.ini；新默认配置放在 car-offline.ini.new"
fi
# 保留字体许可证，供部署产物附带。
if [ -r /usr/share/doc/fonts-wqy-microhei/copyright ]; then
    install -m 644 /usr/share/doc/fonts-wqy-microhei/copyright "$DEST/fonts/LICENSE-wqy.txt"
fi
echo "部署完成。在开发板运行 /opt/car-terminal/start-car.sh"

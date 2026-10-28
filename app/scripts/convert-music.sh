#!/bin/sh
# Ubuntu 执行；输出写入 ready，原歌曲不变。可处理常见音频格式。
set -eu
[ "$#" -eq 2 ] || { echo "用法：$0 原歌曲目录 输出ready目录"; exit 1; }
INPUT=$1
OUTPUT=$2
[ -d "$INPUT" ] || exit 1
mkdir -p "$OUTPUT"
command -v ffmpeg >/dev/null
for song in "$INPUT"/*; do
    [ -f "$song" ] || continue
    case "$song" in *.wav|*.WAV|*.mp3|*.MP3|*.flac|*.FLAC|*.m4a|*.M4A) ;; *) continue;; esac
    name=$(basename "$song")
    case "$name" in silence.wav|test.wav|test-s16-48k.wav) continue;; esac
    dest="$OUTPUT/${name%.*}.wav"
    [ ! -e "$dest" ] || { echo "已存在，跳过：$dest"; continue; }
    ffmpeg -nostdin -n -i "$song" -map 0:a:0 -vn -ar 48000 -ac 2 -c:a pcm_s16le "$dest"
done

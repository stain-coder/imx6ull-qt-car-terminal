# i.MX6ULL 触摸车载终端 v0.4

基于 v0.3 增加联网天气。保留音乐播放、暂停、切歌、拖动进度、百分比音量、AP3216C / ICM20608 采集以及系统时钟显示。

## 环境

- 开发板：i.MX6ULL，Linux 4.1.15，1024×600，fb0，event1。
- Qt：Buildroot output-car 产出的 ARM Qt 5.15.18；Widgets + ALSA。
- 音乐：PCM S16_LE、48000 Hz、双声道 WAV，默认 `/opt/car-terminal/music`。
- 天气：已部署的 `/opt/weather-tools/bin/curl`、同目录 lib 和 certs。
- 必须先完成 WiFi 连接、DHCP、DNS和系统校时。本程序不修改网络路由、不自动连接 WiFi、不设置系统时间。

## Ubuntu 编译

解压到新目录，Qt Creator 打开 `car-terminal.pro`，选择之前的 ARM Kit，运行 qmake，再构建。
也可在本工程根目录使用命令行：

```sh
mkdir -p build-arm
cd build-arm
/home/stain/linux/tools/buildroot-2025.02.17/output-car/host/bin/qmake ../car-terminal.pro
make -j4
```

qmake 生成 Makefile，make 使用 ARM 工具链编译程序。不使用 Ubuntu 自带 qmake 编译部署版本。
不需要重新编译内核或增加 Qt Network / Qt Multimedia。天气请求由异步 QProcess 调用 curl，JSON 由 Qt Core 解析。

## 部署

先在开发板“系统”页面退出旧程序，然后在 Ubuntu 工程根目录执行：

```sh
sudo sh scripts/deploy.sh build-arm/car-terminal /home/stain/linux/nfs/rootfs-car
```

脚本校验 ARM ELF，备份原程序为 `car-terminal.previous`，复制字体和启动脚本。
歌曲和已有 `car-offline.ini` 保留；新默认配置保存为 `car-offline.ini.new`。
旧配置没有 `[weather]` 时会使用默认长沙坐标和你已经部署成功的 curl 路径，直接可用。
继续使用旧配置文件名是为了兼容 v0.3，并不代表仅支持离线。
脚本不会复制 curl；你之前部署的 `/opt/weather-tools` 应保留。

在开发板执行：

```sh
/opt/car-terminal/start-car.sh
```

命令行编译得到的程序位于 `build-arm/car-terminal`。如果用 Qt Creator，请把部署命令中的程序路径换成 Creator 实际构建目录中的可执行文件。

## 天气与触摸交互

启动后请求一次，默认每 15 分钟刷新；“天气”页有手动刷新按钮，请求期间禁用按钮以防重复请求。
显示温度、相对湿度、中文天气描述和连续三天最低/最高温度。
每张预报卡标明实际日期；断网后不会继续把旧日期标成“今天”。
天气数据时间来自接口；获取时间来自开发板系统时间，均以北京时间显示。
请求失败保留本次程序运行期间最后成功的数据，并明确提示不是新数据；重启不加载旧缓存。
缺失/null/错误类型的字段不会默认显示为零；不完整的预报会被拒绝。
请求连接超时 10 秒、总超时 30 秒，Qt 另有 35 秒保护。限制输出体积，避免异常响应无限增长。
请求不使用 shell、不忽略证书验证，curl 动态库环境只用于子进程，不修改音乐线程环境。

配置可追加到 `/opt/car-terminal/car-offline.ini`（避免重复 `[weather]` 分组）：

```ini
[weather]
city=长沙
latitude=28.2282
longitude=112.9388
refresh_minutes=15
curl=/opt/weather-tools/bin/curl
library=/opt/weather-tools/lib
ca=/opt/weather-tools/certs/ca-certificates.crt
```

修改城市需同时修改经纬度，退出并重新启动程序生效。没有 GPS 自动定位。
刷新周期限制在 1～1440 分钟，建议保留 15 分钟，避免频繁访问。
天气来源为 Open-Meteo，界面附有来源与 CC BY 4.0 标注。上线商业使用前应核对服务条款。

## 验收步骤

1. 开启音乐，进入天气页，确认能显示数据和三个日期。
2. 点击刷新期间切回音乐，测试暂停、切歌、拖动进度；界面应保持响应。
3. 暂时关闭无线路由器或在板上执行 `ifconfig wlan0 down`，保持 eth0/NFS 不变；点击刷新，确认超时提示与旧数据保留。
4. 恢复 WiFi（必要时重新关联和 DHCP），点击刷新确认恢复。
5. 检查 AP3216C、ICM20608 数据及百分比音量功能。

天气异常时先检查 `date`、WiFi、DNS以及独立 curl 请求。详细 curl 错误会打印在串口中。
传感器与声卡仍使用 v0.3 的设备路径/ABI；不要同时运行独立传感器测试程序争用设备。

## 代码结构

- `src/weather.h/.cpp`：异步天气页面、JSON 验证和天气代码转换。
- `src/window.cpp`：整合首页、音乐、天气、传感器、系统页面。
- `src/audio.*`：ALSA 后台播放与进度控制（沿用 v0.3）。
- `src/sensors.*`：字符设备后台采集（沿用 v0.3）。
- `tests/weather-test.*`：模拟 curl 的异步与错误场景测试。
- `scripts/`：启动、部署、歌曲转换。

## 参考

- https://open-meteo.com/en/docs
- https://open-meteo.com/en/terms
- https://creativecommons.org/licenses/by/4.0/
- https://doc.qt.io/archives/qt-5.15/qprocess.html

本交付的主机测试范围见 `docs/verification.md`。ARM 编译和真实板端验收需在你的环境完成。

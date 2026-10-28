# 基于 i.MX6ULL 的 Qt 车载终端

基于 i.MX6ULL、Linux 4.1.15 和 Qt 5.15.18 的嵌入式车载终端演示项目，集成触摸式音乐播放、板载传感器数据显示、网络天气查询和时钟显示。

项目围绕驱动、设备树、根文件系统和 Qt 应用完成软硬件联调，用于学习和展示嵌入式 Linux 开发流程。当前不包含 GPS、视频播放或实车控制功能。

## 功能

| 功能 | 实现内容 |
| --- | --- |
| 触摸界面 | Qt Widgets 页面与控件，支持触摸操作 |
| 音乐播放 | 本地 WAV 播放、曲目切换、暂停、进度定位及百分比音量控制 |
| 环境感知 | AP3216C 红外、环境光及接近数据展示 |
| 运动感知 | ICM20608 三轴加速度、三轴角速度及芯片内部温度展示 |
| 天气查询 | 通过 HTTPS 获取 Open-Meteo 天气数据并解析 JSON |
| 时钟显示 | 显示系统时间，配合系统 NTP 校时和 RTC 时间读写 |

AP3216C 不测量温湿度；ICM20608 的内部温度不等于车内空气温度。天气页面的温湿度来自网络服务。

## 开发环境与技术栈

| 项目 | 使用环境 |
| --- | --- |
| 硬件平台 | i.MX6ULL 开发板 |
| 内核 | Linux 4.1.15 BSP |
| 根文件系统 | Buildroot 2025.02.17，独立输出目录 output-car |
| 应用框架 | Qt 5.15.18、Qt Widgets |
| 开发语言 | C、C++、Shell |
| 驱动及接口 | I²C、SPI、字符设备、input、framebuffer、ALSA、USB 网络 |
| 网络组件 | wpa_supplicant、DHCP、curl、OpenSSL、CA 证书 |
| 开发方式 | Ubuntu 交叉编译、TFTP 启动与 NFS 根文件系统 |

内核继续使用已经验证的 BSP 编译环境，Qt 应用使用 Buildroot 生成的 ARM 工具链及 ARM Qt。开发机上的桌面 Qt 与板端 ARM Qt 不可混用。

## 仓库组织

下表按 Qt 工程放在 app/ 下说明；若实际工程直接放在仓库根目录，进入 car-terminal.pro 所在目录执行构建即可。

| 路径 | 内容 |
| --- | --- |
| app/ | Qt 工程、应用配置、部署及启动脚本 |
| drivers/ap3216/ | AP3216C 驱动及相关文件 |
| drivers/icm20608/ | ICM20608 驱动及相关文件 |
| drivers/ft5426/ | 用于替换 BSP 原有实现的触摸驱动 |
| drivers/realtek/ | 四种 Realtek 无线网卡驱动源码 |
| config/ | Buildroot、BusyBox、内核等构建配置及说明 |
| dts/ | 项目最终使用的 DTB 及说明 |
| docs/ | 架构、构建部署、硬件驱动说明及展示图片 |
| LICENSE | 原创部分的 MIT 许可与第三方代码适用范围 |
| .gitignore | 排除构建产物、本机配置和私密文件 |

本仓库不提供真实 Wi-Fi 认证配置。构建配置的实际文件名以 config 目录为准。

## 硬件适配与工作范围

- AP3216C：通过 I²C 接入，以字符设备接口向应用提供数据。
- ICM20608：通过 ECSPI3 接入，使用 GPIO1_IO20 作为片选，处理与 UART2 的引脚复用冲突。
- FT5426：替换 BSP 内核中对应的触摸驱动，经 input 子系统向 Qt 提供触摸事件。
- Realtek：将相应驱动集成到内核构建，当前项目验证使用 8192cu 模块的 USB 网卡。
- WM8960 与 LCD：复用 BSP 中已有驱动，完成设备树适配与应用联调。

收录四种 Realtek 驱动不代表四种网卡均已完成验证。第三方源码保留原有作者、版权和许可信息，项目适配维护由 gongting 完成。

## 构建和部署

### 1. 准备系统

先确认目标内核与设备树匹配，显示、触摸、音频及传感器可以独立工作。驱动编为模块时，使用与当前内核匹配的 .ko，并按依赖顺序使用 insmod 加载。

根文件系统需提供 ARM Qt、ALSA 及应用依赖的运行库。网络天气还需要可用的网络连接、curl、TLS 库和 CA 证书。

当前 dts 目录保存最终 DTB，不包含完整 DTS/DTSI 源码。因此它可以提供匹配环境下的启动配置，但还不足以独立重建原始设备树工程。

### 2. 构建应用

在 car-terminal.pro 所在目录执行：

~~~sh
mkdir -p build-arm
cd build-arm
/home/stain/linux/tools/buildroot-2025.02.17/output-car/host/bin/qmake ../car-terminal.pro
make -j4
~~~

qmake 生成 ARM 构建规则，make 编译并链接程序。请将路径替换为本机实际的 Buildroot 路径。

### 3. 安装到 NFS 根文件系统

返回 Qt 工程根目录执行：

~~~sh
sudo sh scripts/deploy.sh \
    build-arm/car-terminal \
    /home/stain/linux/nfs/rootfs-car
~~~

部署脚本将程序及配套文件安装到目标根文件系统的 /opt/car-terminal。开发板挂载该 NFS 根目录后，即可访问这些文件。

部署前先退出旧程序。脚本不负责自动安装全部驱动、连接 Wi-Fi 或配置 NFS 服务；具体行为见应用目录内的脚本及说明。

### 4. 启动

在开发板执行：

~~~sh
sh /opt/car-terminal/start-car.sh
~~~

当前运行环境使用 /dev/fb0 和 /dev/input/event1，音频使用 WM8960 ALSA 声卡。更换内核后需重新确认设备编号。

音乐放入应用配置指定的目录，当前使用 /opt/car-terminal/music。已验证的播放路径使用 16 位、48 kHz、双声道 PCM WAV；不要假定应用支持所有音频编码格式。

## 网络与时间

音乐、触摸和本地传感器功能可离线使用。天气需要先完成以下准备：

1. 加载匹配的无线驱动并启用 wlan0。
2. 使用 wpa_supplicant 完成热点认证。
3. 通过 DHCP 获取地址，确认路由和 DNS。
4. 校准系统时间，保证 HTTPS 证书验证具备合理的时间基础。
5. 确认 curl、动态库及 CA 证书路径与应用配置一致。

项目调试使用的独立工具目录为 /opt/weather-tools 和 /opt/net-tools。若改用根文件系统自带程序，需要同步更新调用路径。

启动 Qt 脚本不等于自动完成无线连接和校时。RTC 断电保持能力还取决于后备供电与硬件验证。

## 项目文档

- [驱动集成说明](drivers/README.md)
- [构建配置说明](config/README.md)
- [设备树说明](dts/README.md)
- [系统架构](docs/architecture.md)
- [构建与部署](docs/build-and-deploy.md)
- [硬件与驱动适配](docs/hardware-and-drivers.md)

项目展示图片可放在 docs/images/。本仓库不预先填写未经测量的性能指标，也不把单次功能跑通描述为长期稳定性验证完成。

## 天气数据与许可证

天气数据来源：[Open-Meteo](https://open-meteo.com/)。使用天气数据时需遵循数据提供方的使用条件及署名要求。

本仓库是包含原创代码和第三方组件的集合。gongting 有权授权的原创应用、脚本及文档部分采用 MIT 许可证，详见 LICENSE。Linux/BSP 派生驱动、Realtek 驱动及其他第三方内容保留其原有许可证，根目录 MIT 许可不覆盖或替换这些许可。

Qt、ALSA 等依赖遵循各自许可证；发布包含这些依赖的二进制或根文件系统时，需要另外满足相应分发条件。


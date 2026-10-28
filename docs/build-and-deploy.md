# 构建与部署

本文记录本项目采用的 Ubuntu 交叉编译、Buildroot 用户空间和 NFS 部署方式。命令中的路径来自开发环境，其他电脑需要替换为实际路径。

## 环境

| 项目 | 当前使用环境 |
| --- | --- |
| 开发板 | i.MX6ULL |
| 内核 | Linux 4.1.15 BSP |
| 根文件系统构建 | Buildroot 2025.02.17，独立输出目录 output-car |
| 应用框架 | ARM Qt 5.15.18 |
| 开发机 | Ubuntu |
| NFS 根目录 | /home/stain/linux/nfs/rootfs-car |

内核使用已经验证的 BSP 交叉编译环境；Qt 应用使用 Buildroot 生成的 ARM 工具链和 Qt。两者职责不同，不需要为了构建 Qt 而更换原有内核编译器。

## 1. 准备根文件系统

Buildroot 配置记录在仓库 config 目录。需要的运行能力包括 Qt Widgets、ALSA，以及天气请求和无线连接对应的工具、库及证书。

Buildroot 的 host 目录包含在 Ubuntu 上运行的交叉构建工具；target 目录包含目标系统文件的构建中间结果；images 目录提供用于部署的最终镜像或文件系统归档。

首次部署完整根文件系统，应使用最终生成的文件系统产物，并保留权限、符号链接及必要设备文件。不要将 target 目录简单当成可直接部署的完整根文件系统。

更换正在使用的 NFS 根目录前，应备份并停机操作。新系统还需要保留项目应用、内核模块、字体和本机配置。

## 2. 构建 Qt 应用

在 Qt 工程根目录，即 car-terminal.pro 所在目录执行：

~~~sh
mkdir -p build-arm
cd build-arm

/home/stain/linux/tools/buildroot-2025.02.17/output-car/host/bin/qmake ../car-terminal.pro

make -j4
~~~

qmake 根据 .pro 文件生成适用于该 ARM Qt 环境的 Makefile；make 再按照规则编译并链接程序。

这里必须使用 Buildroot 提供的 ARM Qt qmake。Ubuntu 桌面 Qt 的 qmake 通常生成面向开发机的程序，不能据此得到开发板可运行的 ARM 应用。

构建完成后可检查：

~~~sh
file car-terminal
~~~

输出应表明它是 ARM ELF 程序。该检查只能确认架构等基本信息，不能替代板端运行验证。

Qt Creator 中也应选择配置了同一 qmake 和交叉编译器的 ARM Kit。

## 3. 部署程序

返回 Qt 工程根目录，使用项目现有脚本：

~~~sh
sudo sh scripts/deploy.sh \
    build-arm/car-terminal \
    /home/stain/linux/nfs/rootfs-car
~~~

| 参数 | 含义 |
| --- | --- |
| sudo | 允许写入由 root 管理的根文件系统目录 |
| sh scripts/deploy.sh | 在 Ubuntu 执行部署脚本 |
| build-arm/car-terminal | 刚构建完成的 ARM 可执行程序 |
| /home/stain/linux/nfs/rootfs-car | 开发板挂载的 NFS 根文件系统在 Ubuntu 上的目录 |

脚本将程序及配套文件安装到该根文件系统的 /opt/car-terminal 下。于是 Ubuntu 上的 /home/stain/linux/nfs/rootfs-car/opt/car-terminal，对应开发板看到的 /opt/car-terminal。

这不是通过网络把程序另存一份到开发板本地磁盘；在 NFS 启动方式下，开发板直接访问 Ubuntu 导出的文件系统内容。

现有部署脚本保留已存在的应用配置，新的默认配置另存为 .new 时，需要自行比较并合并。它也不负责安装全部内核驱动、建立无线连接或配置 NFS 服务。

## 4. 启动应用

确认显示、触摸、声音和传感器设备已就绪，并退出旧的应用进程。按当前应用包的部署方式，在开发板执行：

~~~sh
sh /opt/car-terminal/start-car.sh
~~~

启动脚本用于配置 Qt 平台、触摸和字体等运行环境，并启动程序。实际文件名以所部署脚本为准。

音乐文件放入应用配置指定的目录。中文显示需要可用的中文字体；正确编码的文件名与字体缺字是两类不同问题。

## 5. 网络天气的运行依赖

本项目调试过程中使用了独立目录：

- /opt/weather-tools/bin/curl：请求程序。
- /opt/weather-tools/lib：curl 所需的配套动态库。
- /opt/weather-tools/certs/ca-certificates.crt：CA 证书。
- /opt/net-tools/busybox：提供已启用的网络校时功能。

如果最终完整根文件系统已经提供同类工具，可修改应用配置使用系统路径。不要只改变文件位置而忘记更新调用路径。

天气请求需要先具备可用的网络连接、DNS、合理的系统时间和证书。不要通过关闭证书验证来代替正确配置。

本仓库不提供真实网络配置。部署者自行设置热点认证信息，并确认默认路由和 DNS。使用 NFS 时应保留通往 NFS 服务器的以太网连接和路由。

## 6. 部署后的基本确认

确认界面和触摸正常、音乐可以播放和定位、传感器读数更新、天气可刷新、时钟合理即可完成一次基本检查。硬件数据、原始日志和截图可按需要保存在 docs/images 或项目展示资料中，本文不预先填写性能数据。


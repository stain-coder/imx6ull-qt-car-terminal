# v0.4 验证范围

本次在主机 Qt 5.15.13 上使用 C++14、-Wall、-Wextra 编译整个 v0.4 工程通过。
新增天气测试使用模拟 curl 进程，不依赖互联网或开发板：

- 有效 JSON 解析与中文天气代码转换。
- 拒绝 HTML、空对象、API 错误、null 温度、日期不连续的预报。
- 模拟慢请求期间，GUI 定时器持续运行，确认事件循环不被阻塞。
- 请求成功后显示温度；随后超时/无效数据仍保留旧结果并报告失败。
- 恢复有效响应后能重新更新；curl 缺失时显示明确错误。
- 部署和启动脚本通过 sh -n 语法检查。

本次未进行 ARM 交叉编译、真实 HTTPS 请求、WM8960 播放、实际触摸或板上运行测试。
音乐与传感器实现文件保持 v0.3 原样，不能用主机模拟测试代替板端验收。

桌面 Qt 测试命令（独立构建目录，不使用 ARM Kit）：

```sh
mkdir -p build-weather-test
cd build-weather-test
qmake ../tests/weather-test.pro
make -j4
QT_QPA_PLATFORM=offscreen ./weather-test
```

以下为历史 v0.3 的验证记录，不代表本次重新执行：

# v0.3 验证范围

已在 Ubuntu 主机 Qt 5.15.13 / ALSA 上编译通过（-Wall -Wextra）。
音频测试使用 ALSA null 输出，不发声：验证 WAV 总时长、中文路径、暂停位置保持、暂停中 seek、继续、定位末尾、播放完成后 seek、停止清零、错误 WAV 拒绝。
另以模拟 amixer 和 ALSA null 做 GUI 集成检查：改变音量后百分比显示更新，播放状态继续，无暂停命令。
没有连接用户开发板，尚未验证 WM8960 真实输出、触摸操作、实际音量曲线、真实 NFS 长时间播放。
传感器代码沿用 v0.2，数据格式和此前的桩测试见 docs/sensor-abi.md、tests/sensor-test.cpp。

桌面测试（使用桌面 Qt，不要与 ARM 构建目录混用）：

```sh
mkdir -p build-audio-test
cd build-audio-test
qmake ../tests/audio-test.pro
make
./audio-test
```

板上验收：播放同时拖动进度；暂停时拖动后继续；音量60→30→0→60；观察播放状态和时间是否继续；同时打开环境页采集。

参考：
- ALSA PCM API：https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html
- amixer -M 听感映射：https://manpages.debian.org/trixie/alsa-utils/amixer.1.en.html

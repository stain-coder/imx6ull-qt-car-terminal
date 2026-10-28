#include "sensors.h"
#include <QFile>
#include <QDateTime>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

SensorThread::SensorThread(QString ap, QString imu, QObject *parent)
    : QThread(parent), apPath(ap), imuPath(imu) {}
SensorThread::~SensorThread() { requestInterruption(); wait(); }

namespace {
QString errorText(int e) {
    switch(e) {
    case ENOENT: return "设备不存在";
    case ENODEV: return "设备未就绪";
    case EBUSY: return "设备占用，请退出 test";
    case EACCES: return "无读取权限";
    case EOVERFLOW: return "本帧无效/溢出";
    default: return QString("读取失败 errno=%1").arg(e);
    }
}
// 一个 read 必须返回完整的一帧。禁止拼接多次 read 得到不同采样时刻的数据。
QString readFrame(const QString &path, int &fd, void *data, size_t length) {
    if(fd < 0) {
        fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if(fd < 0) return errorText(errno);
        struct stat st;
        if(::fstat(fd, &st) || !S_ISCHR(st.st_mode)) {
            ::close(fd); fd=-1; return "需要字符设备节点";
        }
    }
    const ssize_t n = ::read(fd, data, length);
    if(n == ssize_t(length)) return {};
    const int saved = errno;
    // 暂态无效帧保留句柄；其余错误关闭后在下一周期重新打开。
    if(n < 0 && (saved==EOVERFLOW || saved==EINTR || saved==EAGAIN))
        return errorText(saved);
    ::close(fd); fd=-1;
    return n < 0 ? errorText(saved) : QString("帧长错误：%1/%2").arg(n).arg(length);
}
}
void SensorThread::run() {
    int ap=-1, imu=-1;
    while(!isInterruptionRequested()) {
        QStringList values;
        for(int i=0;i<10;++i) values << "—";
        uint16_t light[3] = {};
        int32_t motion[7] = {};
        const QString ae=readFrame(apPath,ap,light,sizeof light);
        const QString ie=readFrame(imuPath,imu,motion,sizeof motion);
        if(ae.isEmpty()) {
            values[0]=QString::number(light[1])+" raw";
            values[1]=QString::number(light[2])+" raw";
            values[2]=QString::number(light[0])+" raw";
        }
        if(ie.isEmpty()) {
            // 对应已交付驱动：GYRO_CONFIG=0x18，ACCEL_CONFIG=0x18。
            // ±16g: 2048 LSB/g；±2000°/s: 16.4 LSB/(°/s)。
            for(int a=0;a<3;++a) {
                values[3+a]=QString::number(motion[3+a]/2048.0,'f',3)+" g";
                values[6+a]=QString::number(motion[a]/16.4,'f',2)+" °/s";
            }
            // 暂不把内部温度换算成环境温度。
            values[9]=QString::number(motion[6])+" raw";
        }
        const QString status=QString("AP3216C：%1  |  ICM20608：%2\n最近采集：%3")
            .arg(ae.isEmpty()?"正常":ae, ie.isEmpty()?"正常":ie,
                 QDateTime::currentDateTime().toString("HH:mm:ss"));
        emit sampled(values,status);
        // 分段等待，以便正常退出时尽快响应停止请求。
        for(int i=0;i<10&&!isInterruptionRequested();++i) msleep(50);
    }
    if(ap>=0) ::close(ap);
    if(imu>=0) ::close(imu);
}

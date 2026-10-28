// 仅桌面主机测试：链接 --wrap 模拟驱动系统调用，不用于板端部署。
#include "sensors.h"
#include <QCoreApplication>
#include <QTimer>
#include <cassert>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
static int phase=0, closed=0;
extern "C" int __real_open(const char*,int,...);
extern "C" int __real_close(int);
extern "C" ssize_t __real_read(int,void*,size_t);
extern "C" int __real_fstat(int,struct stat*);
extern "C" int __wrap_open(const char *p,int f,...) {
    if(!strcmp(p,"/test/ap"))return 10001;
    if(!strcmp(p,"/test/imu"))return 10002;
    return __real_open(p,f);
}
extern "C" int __wrap_close(int fd) {
    if(fd==10001||fd==10002){++closed;return 0;}return __real_close(fd);
}
extern "C" int __wrap_fstat(int fd,struct stat *st) {
    if(fd==10001||fd==10002){memset(st,0,sizeof *st);st->st_mode=S_IFCHR;return 0;}
    return __real_fstat(fd,st);
}
extern "C" ssize_t __wrap_read(int fd,void *p,size_t n) {
    if(fd==10001) {
        if(phase==1){errno=EOVERFLOW;return -1;}
        uint16_t a[]={12,345,678};assert(n==sizeof a);memcpy(p,a,n);return n;
    }
    if(fd==10002) {
        if(phase==1)return 0;
        int32_t a[]={164,-164,0,2048,-2048,0,-10};assert(n==sizeof a);memcpy(p,a,n);return n;
    }
    return __real_read(fd,p,n);
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    SensorThread t("/test/ap","/test/imu");int frames=0;
    // DirectConnection：断言和 phase 都在工作线程运行，不存在跨线程数据竞争。
    QObject::connect(&t,&SensorThread::sampled,&t,[&](QStringList v,QString s){
        assert(v.size()==10);
        if(phase==0) {
            assert(v[0]=="345 raw"&&v[1]=="678 raw"&&v[2]=="12 raw");
            assert(v[3]=="1.000 g"&&v[4]=="-1.000 g");
            assert(v[6]=="10.00 °/s"&&v[7]=="-10.00 °/s");
            assert(v[9]=="-10 raw");
        } else {
            for(const auto &x:v)assert(x=="—");
            assert(s.contains("溢出")&&s.contains("帧长错误"));
        }
        ++frames;
        if(frames==1)phase=1;
        else {t.requestInterruption();QMetaObject::invokeMethod(&app,"quit",Qt::QueuedConnection);}
    },Qt::DirectConnection);
    QTimer::singleShot(4000,&app,[&]{assert(false && "test timed out");});
    t.start();app.exec();t.wait();assert(frames==2);assert(closed==2);
    return 0;
}

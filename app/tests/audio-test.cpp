// Linux 桌面测试；ALSA null 不发声，不代表硬件通过。
#include "audio.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QTimer>
#include <QtEndian>
#include <QDebug>
#include <cstdlib>

static void require(bool ok,const char *text){if(!ok){qCritical()<<text;std::abort();}}
static void wav(const QString &path,int seconds) {
    QByteArray h(44,'\0');h.replace(0,4,"RIFF");h.replace(8,8,"WAVEfmt ");h.replace(36,4,"data");
    auto put16=[&](int at,quint16 x){qToLittleEndian(x,reinterpret_cast<uchar*>(h.data()+at));};
    auto put32=[&](int at,quint32 x){qToLittleEndian(x,reinterpret_cast<uchar*>(h.data()+at));};
    put32(4,36+seconds*192000);put32(16,16);put16(20,1);put16(22,2);put32(24,48000);put32(28,192000);put16(32,4);put16(34,16);put32(40,seconds*192000);
    QFile f(path);require(f.open(QIODevice::WriteOnly),"create WAV");f.write(h);f.write(QByteArray(seconds*192000,'\0'));
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);QTemporaryDir dir;
    QString path=dir.path()+"/测试音乐.wav";wav(path,20);
    QString bad=dir.path()+"/bad.wav";{QFile f(bad);f.open(QIODevice::WriteOnly);f.write("bad");}
    AudioThread a("null");int stage=0; qint64 held=-1;
    QObject::connect(&a,&AudioThread::playback,&app,[&](QString p,QString state,qint64 pos,qint64 duration,QString detail){
        if(state=="error"&&stage!=7){qCritical()<<detail;std::abort();}
        if(stage==0&&state=="playing") {require(p==path&&duration==20000,"load/duration");stage=1;a.togglePause();}
        else if(stage==1&&state=="paused") {stage=2;held=pos;QTimer::singleShot(200,&app,[&]{a.seekMs(10000);});}
        else if(stage==2&&state=="paused") {
            require(pos==held||pos==10000,"paused moved");
            if(pos==10000){stage=3;a.togglePause();}
        }
        else if(stage==3&&state=="playing") {require(pos>=10000,"resume position");stage=4;a.seekMs(20000);}
        else if(stage==4&&state=="finished") {require(pos==duration,"EOF");stage=5;a.seekMs(5000);}
        else if(stage==5&&state=="paused") {require(pos==5000,"seek after EOF");stage=6;a.stopPlayback();}
        else if(stage==6&&state=="stopped") {require(pos==0&&duration==0,"stop reset");stage=7;a.playFile(bad);}
        else if(stage==7&&state=="error") {require(!detail.isEmpty(),"invalid WAV error");stage=8;app.quit();}
    });
    QTimer::singleShot(8000,&app,[&]{require(false,"test timeout");});
    a.start();a.playFile(path);app.exec();a.requestInterruption();a.wait();require(stage==8,"complete");qInfo()<<"audio tests passed";
}

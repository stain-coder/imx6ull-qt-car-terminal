#include "audio.h"
#include <QFile>
#include <QMutexLocker>
#include <QElapsedTimer>
#include <QtEndian>
#include <alsa/asoundlib.h>
#include <cerrno>

namespace {
// 仅接收已验证的 S16_LE / 48000Hz / stereo PCM WAV。
QString waveInfo(QFile &f, qint64 &offset, qint64 &frames) {
    const auto h=f.read(12);
    if(h.size()!=12 || h.left(4)!="RIFF" || h.mid(8,4)!="WAVE") return "不是 RIFF WAV 文件";
    const qint64 limit=8+qint64(qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(h.constData()+4)));
    if(limit>f.size() || limit<12) return "WAV 长度无效";
    bool fmt=false, data=false;
    while(f.pos()+8<=limit) {
        const auto c=f.read(8);
        if(c.size()!=8) return "WAV 读取失败";
        const quint32 n=qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(c.constData()+4));
        const qint64 end=f.pos()+n;
        if(end>limit) return "WAV 数据不完整";
        if(c.left(4)=="fmt ") {
            const auto b=f.read(qMin<quint32>(n,40));
            if(b.size()<16) return "WAV 格式头不完整";
            auto u16=[&](int i){return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(b.constData()+i));};
            auto u32=[&](int i){return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(b.constData()+i));};
            if(u16(0)!=1 || u16(2)!=2 || u32(4)!=48000 || u16(14)!=16 || u16(12)!=4 || u32(8)!=192000)
                return "请转换为 PCM 16位 / 48kHz / 双声道 WAV";
            fmt=true;
        }
        if(c.left(4)=="data") {if(!n || n%4) return "WAV 音频长度无效"; offset=f.pos();frames=n/4;data=true;}
        if(fmt&&data) return f.seek(offset)?QString():QString("WAV 定位失败");
        if(!f.seek(end+(n&1))) return "WAV 定位失败";
    }
    return "缺少 fmt 或 data 块";
}
}
AudioThread::AudioThread(QString d,QObject *parent):QThread(parent),device(d) {}
AudioThread::~AudioThread(){requestInterruption();wait();}
void AudioThread::enqueue(Command c) {
    QMutexLocker lock(&mutex);
    if(c.kind==Load || c.kind==Stop) commands.clear();
    if(c.kind==Seek && !commands.isEmpty() && commands.back().kind==Seek) commands.removeLast();
    commands.enqueue(c);
}
void AudioThread::playFile(const QString &p){enqueue({Load,p,0});}
void AudioThread::stopPlayback(){enqueue({Stop,{},0});}
void AudioThread::togglePause(){enqueue({Toggle,{},0});}
void AudioThread::seekMs(qint64 ms){enqueue({Seek,{},ms});}
void AudioThread::run() {
    QFile file;
    snd_pcm_t *pcm=nullptr;
    QString path,state="stopped",detail;
    qint64 offset=0,total=0,sent=0;
    QByteArray block;
    bool dirty=true;
    QElapsedTimer tick;tick.start();
    auto close=[&]{if(pcm){snd_pcm_drop(pcm);snd_pcm_close(pcm);pcm=nullptr;}file.close();block.clear();};
    auto fail=[&](const QString &e){detail=e;state="error";close();dirty=true;};
    auto position=[&]() -> qint64 {
        snd_pcm_sframes_t delay=0;
        if(pcm && state=="playing" && snd_pcm_delay(pcm,&delay)>=0)
            return qBound<qint64>(0,sent-qMax<snd_pcm_sframes_t>(0,delay),total);
        return qBound<qint64>(0,sent,total);
    };
    auto relocate=[&](qint64 frame) -> bool {
        frame=qBound<qint64>(0,frame,total);
        int rc=snd_pcm_drop(pcm);
        if(rc<0){fail(QString("停止音频缓冲失败：%1").arg(snd_strerror(rc)));return false;}
        rc=snd_pcm_prepare(pcm);
        if(rc<0){fail(QString("准备声卡失败：%1").arg(snd_strerror(rc)));return false;}
        block.clear();sent=frame;
        if(!file.seek(offset+frame*4)){fail("歌曲定位失败");return false;}
        dirty=true;return true;
    };
    while(!isInterruptionRequested()) {
        QQueue<Command> batch;
        {QMutexLocker lock(&mutex);batch.swap(commands);}
        for(const auto &c:batch) {
            dirty=true;
            if(c.kind==Load) {
                close();path=c.path;sent=total=offset=0;detail.clear();state="stopped";
                file.setFileName(path);
                if(!file.open(QIODevice::ReadOnly)){fail("无法打开歌曲："+file.errorString());continue;}
                const auto e=waveInfo(file,offset,total);
                if(!e.isEmpty()){fail(e);continue;}
                int rc=snd_pcm_open(&pcm,device.toLocal8Bit().constData(),SND_PCM_STREAM_PLAYBACK,SND_PCM_NONBLOCK);
                if(rc<0){pcm=nullptr;fail(QString("打开声卡失败：%1").arg(snd_strerror(rc)));continue;}
                rc=snd_pcm_set_params(pcm,SND_PCM_FORMAT_S16_LE,SND_PCM_ACCESS_RW_INTERLEAVED,2,48000,0,100000);
                if(rc<0){fail(QString("声卡参数失败：%1").arg(snd_strerror(rc)));continue;}
                state="playing";
            } else if(c.kind==Stop) {
                close();state="stopped";sent=total=0;path.clear();detail.clear();
            } else if(pcm && c.kind==Seek) {
                if(state!="playing")state="paused";
                relocate(qMin(c.ms,total*1000/48000)*48);
            } else if(pcm && c.kind==Toggle) {
                if(state=="playing") {
                    const auto p=position();
                    if(relocate(p))state="paused";
                } else if(state=="paused" || state=="finished") {
                    if(relocate(sent>=total?0:sent))state="playing";
                }
            }
        }
        if(pcm && state=="playing") {
            if(block.isEmpty() && sent<total) {
                const qint64 bytes=qMin<qint64>(960,total-sent)*4;
                block=file.read(bytes);
                if(block.size()!=bytes){fail("歌曲读取失败或文件被修改");}
            }
            if(pcm && !block.isEmpty()) {
                const auto n=snd_pcm_writei(pcm,block.constData(),block.size()/4);
                if(n>0){sent+=n;block.remove(0,int(n)*4);}
                else if(n<0 && n!=-EAGAIN && n!=-EINTR) {
                    // 不用可能循环等待的 resume；暂停恢复失败直接提示。
                    const int rc=(n==-EPIPE)?snd_pcm_prepare(pcm):int(n);
                    if(rc<0)fail(QString("音频输出失败：%1").arg(snd_strerror(rc)));
                    else {detail="发生欠载并已恢复，请检查 NFS / CPU 负载";dirty=true;}
                }
            }
            if(pcm && sent==total && block.isEmpty()) {
                const int rc=snd_pcm_drain(pcm); // 非阻塞；保留最后一段音频直到播放完。
                if(rc==0){state="finished";dirty=true;}
                else if(rc!=-EAGAIN && rc!=-EINTR)fail(QString("音频尾段输出失败：%1").arg(snd_strerror(rc)));
            }
        }
        if(dirty || tick.elapsed()>=100) {
            emit playback(path,state,position()*1000/48000,total*1000/48000,detail);
            dirty=false;tick.restart();
        }
        msleep(5);
    }
    close();
}

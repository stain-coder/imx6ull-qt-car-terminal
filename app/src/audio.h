#pragma once
#include <QThread>
#include <QMutex>
#include <QQueue>
#include <QString>

// 音频文件和 ALSA PCM 句柄全部归工作线程所有，GUI 仅提交命令。
class AudioThread : public QThread {
    Q_OBJECT
public:
    explicit AudioThread(QString device, QObject *parent=nullptr);
    ~AudioThread() override;
    void playFile(const QString &path);
    void stopPlayback();
    void togglePause();
    void seekMs(qint64 ms);
signals:
    void playback(QString path, QString state, qint64 positionMs,
                  qint64 durationMs, QString detail);
protected:
    void run() override;
private:
    enum Kind { Load, Stop, Toggle, Seek };
    struct Command { Kind kind; QString path; qint64 ms; };
    void enqueue(Command c);
    const QString device;
    QMutex mutex;
    QQueue<Command> commands;
};

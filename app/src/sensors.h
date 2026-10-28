#pragma once
#include <QThread>
#include <QStringList>

// 文件描述符只在 run() 所在线程中创建、读取和关闭。
class SensorThread : public QThread {
    Q_OBJECT
public:
    SensorThread(QString ap, QString imu, QObject *parent = nullptr);
    ~SensorThread() override;
signals:
    void sampled(QStringList values, QString status);
protected:
    void run() override;
private:
    const QString apPath, imuPath;
};

#pragma once
#include <QWidget>
#include <QSettings>
#include <QProcess>
#include <QTimer>
#include <QLabel>
#include <QPushButton>
#include <QDateTime>
#include <QVector>

struct WeatherData {
    double temperature = 0, humidity = 0;
    int code = 0;
    QString time;
    QStringList dates;
    QVector<double> low, high;
};
bool parseWeather(const QByteArray &json, WeatherData &data, QString &error);
QString weatherDescription(int code);

// 无需额外网络库：通过异步 QProcess 使用板上已验证的 curl。
class WeatherPage : public QWidget {
public:
    explicit WeatherPage(QSettings &config, QWidget *parent = nullptr);
    ~WeatherPage() override;
    void refresh();
private:
    QProcess process;
    QTimer refreshTimer, deadline;
    QString city, latitude, longitude, curl, library, ca;
    QLabel *current, *status, *updated;
    QVector<QLabel*> days;
    QPushButton *refreshButton;
    QByteArray output, errors;
    QString forcedError;
    QDateTime lastSuccess;
    bool active = false;
    void collect();
    void fail(const QString &reason);
    void finish(int code, QProcess::ExitStatus state);
};

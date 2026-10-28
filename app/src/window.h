#pragma once
#include <QMainWindow>
#include <QSettings>
#include <QProcess>
#include <QTimer>
#include "sensors.h"
#include "audio.h"
#include <QElapsedTimer>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QStackedWidget>
#include <QVector>

class Window : public QMainWindow {
public:
    explicit Window(const QString &config);
    ~Window() override;
private:
    QSettings cfg;
    AudioThread *audio = nullptr;
    QProcess mixer;
    QTimer volumeDebounce, mixerTimeout;
    int volumeRequest = 60, volumeApplying = 60;
    bool volumePending = false;
    QString audioState = "stopped";
    qint64 durationMs = 0;
    SensorThread *sensorThread = nullptr;
    QLabel *sensorStatus;
    QElapsedTimer sensorAge;
    QStackedWidget *pages;
    QListWidget *songs;
    QLabel *clock, *musicStatus, *volumeText, *positionText;
    QPushButton *pauseButton;
    QSlider *volume, *progress;
    QVector<QLabel *> sensorValues;
    QStringList paths;
    QString current;
    QWidget *homePage();
    QWidget *musicPage();
    QWidget *weatherPage();
    QWidget *sensorPage();
    QWidget *aboutPage();
    void scanSongs();
    void requestPlay(const QString &path);
    void stopPlayback();
    void stepTrack(int delta);
    void togglePause();

    void applyVolume();
};

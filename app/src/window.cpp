#include "window.h"
#include "weather.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QScrollArea>
#include <QScroller>
#include <QtEndian>
#include <QDebug>
#include <QSignalBlocker>
#include <QRegularExpression>
#include <cmath>
#include <csignal>
#include <sys/stat.h>
#include <unistd.h>

namespace {
QString timeText(qint64 ms) {
    const qint64 sec=qMax<qint64>(0,ms)/1000;
    return QString("%1:%2").arg(sec/60,2,10,QChar('0')).arg(sec%60,2,10,QChar('0'));
}
QPushButton *button(const QString &text) {
    auto *b = new QPushButton(text); b->setMinimumHeight(58); return b;
}
QLabel *label(const QString &text) {
    auto *l = new QLabel(text); l->setWordWrap(true); l->setTextFormat(Qt::PlainText); return l;
}

}

Window::Window(const QString &config) : cfg(config, QSettings::IniFormat) {
    cfg.setIniCodec("UTF-8");
    setWindowTitle("i.MX6ULL 车载终端");
    setStyleSheet(R"(
        QMainWindow, QWidget { background:#111d2c; color:#edf4ff; }
        QLabel { font-size:22px; }
        QPushButton { background:#24394f; border:1px solid #3b526b; border-radius:10px;
                      padding:8px 14px; font-size:22px; }
        QPushButton:pressed { background:#146c84; }
        QPushButton:checked { background:#126e88; border-color:#3dd8de; }
        QPushButton:disabled { color:#7e8a99; background:#1a2838; }
        QListWidget { background:#17283a; border:0; font-size:24px; }
        QListWidget::item { padding:14px; border-bottom:1px solid #263e55; }
        QListWidget::item:selected { background:#126e88; }
        QComboBox { font-size:24px; padding:10px; background:#24394f; }
        QComboBox QAbstractItemView { min-height:220px; selection-background-color:#126e88; }
        QSlider { min-height:48px; }
        QSlider::groove:horizontal { height:10px; background:#324d65; border-radius:5px; }
        QSlider::handle:horizontal { width:32px; margin:-12px 0; background:#41d9d2; border-radius:12px; }
        QScrollBar:vertical { width:22px; background:#203247; }
    )");
    auto *root = new QWidget; setCentralWidget(root);
    auto *outer = new QVBoxLayout(root); outer->setContentsMargins(16,12,16,12);
    auto *top = new QHBoxLayout;
    top->addWidget(label("车载终端  /  i.MX6ULL"));
    clock=label(""); clock->setAlignment(Qt::AlignRight|Qt::AlignVCenter); top->addWidget(clock);
    outer->addLayout(top);
    pages=new QStackedWidget;
    pages->addWidget(homePage()); pages->addWidget(musicPage());
    pages->addWidget(weatherPage()); pages->addWidget(sensorPage()); pages->addWidget(aboutPage());
    outer->addWidget(pages,1);
    auto *nav=new QHBoxLayout;
    const QStringList titles={"首页","音乐","天气","环境","系统"};
    QVector<QPushButton*> navButtons;
    for (int i=0;i<titles.size();++i) {
        auto *b=button(titles[i]); b->setCheckable(true); navButtons.append(b); nav->addWidget(b);
        connect(b,&QPushButton::clicked,this,[this,i]{pages->setCurrentIndex(i);});
    }
    connect(pages,&QStackedWidget::currentChanged,this,[navButtons](int index){
        for(int i=0;i<navButtons.size();++i) navButtons[i]->setChecked(i==index);
    }); navButtons[0]->setChecked(true); outer->addLayout(nav);
    auto updateClock=[this]{
        const auto now=QDateTime::currentDateTime();
        clock->setText(now.date().year()<2024 ? "时间未校准" : now.toString("yyyy-MM-dd  HH:mm:ss"));
    };
    auto *timer=new QTimer(this); connect(timer,&QTimer::timeout,this,updateClock); timer->start(1000); updateClock();

    audio=new AudioThread(cfg.value("audio/device","hw:0,0").toString(),this);
    connect(audio,&AudioThread::playback,this,[this](QString path,QString state,qint64 pos,qint64 length,QString detail){
        current=path;audioState=state;durationMs=length;
        if(!progress->isSliderDown()) {
            QSignalBlocker block(progress);
            progress->setRange(0,int(length));progress->setValue(int(pos));
            positionText->setText(timeText(pos)+" / "+timeText(length));
        }
        progress->setEnabled(length>0 && state!="error" && state!="stopped");
        pauseButton->setEnabled(state=="playing"||state=="paused"||state=="finished");
        pauseButton->setText(state=="playing"?"暂停":"继续");
        QString title=state=="playing"?"播放中":state=="paused"?"已暂停":state=="finished"?"播放完成":state=="error"?"播放失败":"已停止";
        musicStatus->setText(title+(path.isEmpty()?QString():"："+QFileInfo(path).fileName())+(detail.isEmpty()?QString():"\n"+detail));
    },Qt::QueuedConnection);
    audio->start();
    volumeDebounce.setSingleShot(true);volumeDebounce.setInterval(150);
    connect(&volumeDebounce,&QTimer::timeout,this,[this]{applyVolume();});
    mixerTimeout.setSingleShot(true);mixerTimeout.setInterval(2500);
    connect(&mixerTimeout,&QTimer::timeout,this,[this]{
        if(mixer.state()!=QProcess::NotRunning){volumeText->setText("音量设置超时");mixer.kill();}
    });
    connect(&mixer,&QProcess::errorOccurred,this,[this](QProcess::ProcessError e){
        if(e==QProcess::FailedToStart){mixerTimeout.stop();volumeText->setText("找不到 amixer，音量未修改");}
    });
    connect(&mixer,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,
            [this](int code,QProcess::ExitStatus status){
        mixerTimeout.stop();
        const QString output=QString::fromLocal8Bit(mixer.readAllStandardOutput());
        const QString error=QString::fromLocal8Bit(mixer.readAllStandardError());
        if(code || status!=QProcess::NormalExit) {
            volumeText->setText("音量设置失败");qWarning().noquote()<<error;
        } else {
            auto m=QRegularExpression("\\[(\\d+)%\\]").match(output);
            const int actual=m.hasMatch()?m.captured(1).toInt():volumeApplying;
            if(!volumePending) {
                QSignalBlocker block(volume);volume->setValue(actual);
                volumeText->setText(QString("音量：%1%（%2）").arg(actual).arg(actual==0?"静音":"听感映射"));
            }
            cfg.setValue("audio/volume_percent",actual);
            qInfo().noquote()<<"Speaker mixer:"<<output;
        }
        if(volumePending)applyVolume();
    });
    // 应用启动时统一设置百分比，不继承旧寄存器值。
    volumePending=true;volumeRequest=volume->value();applyVolume();
    scanSongs();
    sensorThread=new SensorThread(cfg.value("sensors/ap_device","/dev/ap3216c").toString(),
                                  cfg.value("sensors/imu_device","/dev/icm20608").toString(),this);
    connect(sensorThread,&SensorThread::sampled,this,[this](const QStringList &values,const QString &status){
        for(int i=0;i<sensorValues.size() && i<values.size();++i) sensorValues[i]->setText(values[i]);
        sensorStatus->setText(status); sensorAge.restart();
    },Qt::QueuedConnection);
    sensorAge.start(); sensorThread->start();
    auto *freshness=new QTimer(this);
    connect(freshness,&QTimer::timeout,this,[this]{
        if(sensorAge.elapsed()>3000) {
            for(auto *v:sensorValues) v->setText("—");
            sensorStatus->setText("采集超过 3 秒未更新，请检查驱动/总线");
        }
    }); freshness->start(1000);
}
Window::~Window() {
    sensorThread->requestInterruption();
    sensorThread->wait();
    audio->requestInterruption();audio->wait();
    if(mixer.state()!=QProcess::NotRunning) {mixer.kill(); mixer.waitForFinished(1000);}
}
QWidget *Window::homePage() {
    auto *w=new QWidget; auto *l=new QVBoxLayout(w);
    auto *title=label("本地音乐 · 环境感知 · 运动监测");
    title->setStyleSheet("font-size:30px; color:#43dcd2;"); l->addWidget(title); l->addStretch();
    auto *grid=new QGridLayout;
    const QStringList names={"音乐\n本地歌曲 · 暂停 · 切歌", "天气\n城市天气 · 三天预报", "环境\n光照 · 接近 · 运动数据", "系统\n设备配置 · 运行说明"};
    for(int i=0;i<4;++i){auto *b=button(names[i]);b->setMinimumHeight(120);
        grid->addWidget(b,i/2,i%2);connect(b,&QPushButton::clicked,this,[this,i]{pages->setCurrentIndex(i+1);});}
    l->addLayout(grid);l->addStretch();l->addWidget(label("触摸操作  /  本地音乐无需联网  /  GPS 与视频暂不启用"));return w;
}
QWidget *Window::musicPage() {
    auto *w=new QWidget; auto *l=new QVBoxLayout(w);
    auto *row=new QHBoxLayout; row->addWidget(label("本地音乐  ·  PCM 16 位 / 48 kHz / 双声道"),1);
    auto *scan=button("刷新列表");row->addWidget(scan);l->addLayout(row);
    songs=new QListWidget;songs->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    QScroller::grabGesture(songs->viewport(),QScroller::LeftMouseButtonGesture);l->addWidget(songs,1);
    musicStatus=label("请选择歌曲，再点击播放");l->addWidget(musicStatus);
    auto *controls=new QHBoxLayout;
    auto *prev=button("上一首"), *play=button("播放"), *next=button("下一首"), *stop=button("停止");
    pauseButton=button("暂停");pauseButton->setEnabled(false);
    for(auto *b:{prev,play,pauseButton,stop,next}) controls->addWidget(b);
    l->addLayout(controls);
    auto *pr=new QHBoxLayout;positionText=label("00:00 / 00:00");positionText->setMinimumWidth(175);
    progress=new QSlider(Qt::Horizontal);progress->setObjectName("progress");progress->setRange(0,0);progress->setEnabled(false);
    pr->addWidget(progress,1);pr->addWidget(positionText);l->addLayout(pr);
    connect(progress,&QSlider::sliderMoved,this,[this](int ms){positionText->setText(timeText(ms)+" / "+timeText(durationMs));});
    connect(progress,&QSlider::sliderReleased,this,[this]{audio->seekMs(progress->value());});
    auto *vr=new QHBoxLayout;volumeText=label("音量：准备设置");vr->addWidget(volumeText);
    volume=new QSlider(Qt::Horizontal);volume->setObjectName("volume");volume->setRange(0,100);
    volume->setValue(qBound(0,cfg.value("audio/volume_percent",60).toInt(),100));vr->addWidget(volume,1);l->addLayout(vr);
    connect(volume,&QSlider::valueChanged,this,[this](int n){
        volumeRequest=n;volumePending=true;
        volumeText->setText(QString("音量：%1%（设置中）").arg(n));volumeDebounce.start();
    });
    connect(scan,&QPushButton::clicked,this,[this]{if(audioState=="stopped"||audioState=="finished"||audioState=="error")scanSongs();else musicStatus->setText("请先停止播放再刷新列表");});
    connect(play,&QPushButton::clicked,this,[this]{int i=songs->currentRow();if(i>=0&&i<paths.size())requestPlay(paths[i]);});
    connect(songs,&QListWidget::itemDoubleClicked,this,[this](QListWidgetItem*){int i=songs->currentRow();if(i>=0&&i<paths.size())requestPlay(paths[i]);});
    connect(prev,&QPushButton::clicked,this,[this]{stepTrack(-1);});connect(next,&QPushButton::clicked,this,[this]{stepTrack(1);});
    connect(stop,&QPushButton::clicked,this,[this]{stopPlayback();});connect(pauseButton,&QPushButton::clicked,this,[this]{togglePause();});return w;
}
void Window::scanSongs() {
    songs->clear();paths.clear();QDir dir(cfg.value("music/directory","/opt/car-terminal/music/ready").toString());
    if(!dir.exists()){musicStatus->setText("歌曲目录不存在："+dir.path());return;}
    for(const auto &f:dir.entryInfoList(QDir::Files|QDir::Readable,QDir::Name))
        if(f.suffix().compare("wav",Qt::CaseInsensitive)==0){paths<<f.absoluteFilePath();songs->addItem(f.fileName());}
    if(!paths.isEmpty())songs->setCurrentRow(0);
    musicStatus->setText(QString("找到 %1 首歌曲；单击选中，再点击播放").arg(paths.size()));
}
void Window::requestPlay(const QString &path) {
    musicStatus->setText("正在打开歌曲……");audio->playFile(path);
}
void Window::stopPlayback() { audio->stopPlayback(); }
void Window::applyVolume() {
    if(!volumePending || mixer.state()!=QProcess::NotRunning)return;
    volumePending=false;volumeApplying=volumeRequest;
    mixer.start("amixer",QStringList()<<"-M"<<"-c"<<cfg.value("audio/card","0").toString()
                <<"sset"<<"Speaker"<<QString::number(volumeApplying)+"%");
    mixerTimeout.start();
}
void Window::stepTrack(int delta) {
    if(paths.isEmpty()) return;
    int i=songs->currentRow();if(i<0)i=0;else i=(i+delta+paths.size())%paths.size();
    songs->setCurrentRow(i);requestPlay(paths[i]);
}
void Window::togglePause() { audio->togglePause(); }

QWidget *Window::weatherPage() {
    return new WeatherPage(cfg);
}
QWidget *Window::sensorPage() {
    auto *w=new QWidget;auto *l=new QVBoxLayout(w);
    l->addWidget(label("板载传感器  /  后台每 500 ms 采集"));
    auto *grid=new QGridLayout;
    const QStringList names={"光照 ALS","接近 PS","红外 IR","加速度 X","加速度 Y","加速度 Z","角速度 X","角速度 Y","角速度 Z","芯片温度原始值"};
    for(int i=0;i<names.size();++i) {
        auto *name=label(names[i]);name->setStyleSheet("font-size:20px;");
        grid->addWidget(name,i/2,(i%2)*2);
        auto *v=label("—");v->setStyleSheet("color:#43dcd2;font-size:22px;");
        v->setObjectName(QString("sensorValue%1").arg(i));
        sensorValues.append(v);grid->addWidget(v,i/2,(i%2)*2+1);
    }
    for(int i=0;i<4;++i)grid->setColumnStretch(i,1);
    l->addLayout(grid,1);
    sensorStatus=label("正在打开设备……");sensorStatus->setStyleSheet("font-size:18px;");l->addWidget(sensorStatus);
    auto *note=label("ALS / PS / IR 为原始计数；芯片温度不代表车内温度。\n运动量程：±16 g / ±2000 °/s；静止时加速度包含重力，陀螺仪可能有零偏。");
    note->setStyleSheet("font-size:17px;color:#b4c7da;");l->addWidget(note);return w;
}
QWidget *Window::aboutPage() {
    auto *w=new QWidget;auto *l=new QVBoxLayout(w);
    l->addWidget(label("i.MX6ULL 触摸车载终端 · v0.4 联网天气版\nQt Widgets / Linux framebuffer / ALSA"));
    l->addWidget(label("音乐：ALSA 线程播放；拖动进度定位；音量为听感百分比。\n天气：异步 HTTPS 查询；手动与定时刷新。\n传感器：读取字符设备；启动前请退出独立传感器测试程序。\n时间：按北京时间显示系统时间；准确性取决于 RTC/人工校时。"));
    l->addWidget(label("配置文件："+cfg.fileName()+"\n音乐目录："+cfg.value("music/directory").toString()));l->addStretch();
    auto *exit=button("退出界面");l->addWidget(exit);connect(exit,&QPushButton::clicked,this,&QWidget::close);return w;
}

#include "../src/weather.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QFontDatabase>
#include <cstdlib>
static void check(bool ok, const char *message) { if(!ok) { qCritical()<<message; std::exit(1); } }
static void wait(int ms) { QEventLoop loop; QTimer::singleShot(ms,&loop,&QEventLoop::quit); loop.exec(); }
static void write(const QString &path,const QByteArray &bytes) { QFile f(path); check(f.open(QIODevice::WriteOnly),"open fixture");check(f.write(bytes)==bytes.size(),"write fixture"); }
int main(int argc,char **argv) {
    QApplication app(argc,argv);
    const QString font=qEnvironmentVariable("WEATHER_TEST_FONT");
    if(!font.isEmpty()) { const int id=QFontDatabase::addApplicationFont(font);if(id>=0)app.setFont(QFont(QFontDatabase::applicationFontFamilies(id).first(),15)); }
    const QByteArray good=R"({"current":{"time":"2026-10-25T14:00","temperature_2m":22.5,"relative_humidity_2m":60,"weather_code":2},"daily":{"time":["2026-10-25","2026-10-26","2026-10-27"],"temperature_2m_min":[12,13,14],"temperature_2m_max":[23,24,25]}})";
    WeatherData data; QString error;
    check(parseWeather(good,data,error)&&data.temperature==22.5&&data.low.size()==3,"parse success");
    check(!parseWeather("<html>bad gateway</html>",data,error),"reject HTML");
    check(!parseWeather("{}",data,error),"reject missing values");
    check(!parseWeather(R"({"error":true,"reason":"bad latitude"})",data,error),"reject API errors");
    auto obj=QJsonDocument::fromJson(good).object(); auto c=obj["current"].toObject();c["temperature_2m"]=QJsonValue();obj["current"]=c;
    check(!parseWeather(QJsonDocument(obj).toJson(),data,error),"reject null temperature rather than zero");
    obj=QJsonDocument::fromJson(good).object();auto daily=obj["daily"].toObject();daily["time"]=QJsonArray{"2026-10-25","2026-10-27","2026-10-28"};obj["daily"]=daily;
    check(!parseWeather(QJsonDocument(obj).toJson(),data,error),"reject nonconsecutive dates");
    check(weatherDescription(2)==QString::fromUtf8("多云"),"weather translation");
    QTemporaryDir dir;check(dir.isValid(),"temporary directory");
    const QString fake=dir.path()+"/curl";
    write(dir.path()+"/ca","test");write(dir.path()+"/payload",good);
    // 模拟外部进程慢响应，验证 GUI 事件循环仍持续工作。
    write(fake,("#!/bin/sh\nsleep 1\ncat '"+dir.path()+"/payload'\n").toUtf8());
    QFile::setPermissions(fake,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
    QSettings cfg(dir.path()+"/test.ini",QSettings::IniFormat);
    cfg.setValue("weather/curl",fake);cfg.setValue("weather/ca",dir.path()+"/ca");cfg.setValue("weather/library","");
    WeatherPage page(cfg);page.resize(980,440);page.show();
    int ticks=0;QTimer tick;QObject::connect(&tick,&QTimer::timeout,[&]{++ticks;});tick.start(20);
    wait(1900);
    auto *current=page.findChild<QLabel*>("weatherCurrent");auto *status=page.findChild<QLabel*>("weatherStatus");
    check(current&&current->text().contains("22.5"),"process result displayed");check(ticks>30,"UI remains responsive");
    const auto previous=current->text();
    write(fake,"#!/bin/sh\nexit 28\n");page.refresh();wait(200);
    check(status->text().contains(QString::fromUtf8("超时")),"timeout explanation");
    check(current->text()==previous,"failed refresh retains previous result");
    write(fake,("#!/bin/sh\ncat '"+dir.path()+"/payload'\n").toUtf8());write(dir.path()+"/payload","{}");page.refresh();wait(200);
    check(status->text().contains(QString::fromUtf8("失败"))&&current->text()==previous,"invalid JSON preserves result");
    write(dir.path()+"/payload",good);page.refresh();wait(200);
    check(status->text().contains(QString::fromUtf8("更新成功")),"recovery succeeds");
    if(qEnvironmentVariableIsSet("WEATHER_TEST_SCREENSHOT"))page.grab().save(qEnvironmentVariable("WEATHER_TEST_SCREENSHOT"));
    QFile::remove(fake);page.refresh();check(status->text().contains(QString::fromUtf8("curl")),"missing curl detected");
    qInfo()<<"PASS: JSON validation, weather codes, async responsiveness, failure retention, missing curl";
}

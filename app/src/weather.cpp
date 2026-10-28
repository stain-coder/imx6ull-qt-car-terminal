#include "weather.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QUrl>
#include <QUrlQuery>
#include <QDebug>
#include <cmath>

namespace {
QLabel *textLabel(const QString &text, const QString &name = QString()) {
    auto *l = new QLabel(text); l->setWordWrap(true); l->setTextFormat(Qt::PlainText);
    l->setObjectName(name); return l;
}
bool number(const QJsonValue &v) { return v.isDouble() && std::isfinite(v.toDouble()); }
}
QString weatherDescription(int code) {
    switch(code) {
    case 0: return "晴"; case 1: return "大部晴朗"; case 2: return "多云"; case 3: return "阴";
    case 45: return "雾"; case 48: return "雾凇";
    case 51: return "轻毛毛雨"; case 53: return "毛毛雨"; case 55: return "强毛毛雨";
    case 56: case 57: return "冻毛毛雨";
    case 61: return "小雨"; case 63: return "中雨"; case 65: return "大雨";
    case 66: case 67: return "冻雨";
    case 71: return "小雪"; case 73: return "中雪"; case 75: return "大雪"; case 77: return "雪粒";
    case 80: case 81: case 82: return "阵雨"; case 85: case 86: return "阵雪";
    case 95: return "雷暴"; case 96: case 99: return "雷暴伴冰雹";
    default: return QString("未知天气（%1）").arg(code);
    }
}
bool parseWeather(const QByteArray &json, WeatherData &data, QString &error) {
    QJsonParseError e;
    const auto doc = QJsonDocument::fromJson(json, &e);
    if(e.error != QJsonParseError::NoError || !doc.isObject()) {
        error = "天气响应不是有效的 JSON 对象"; return false;
    }
    const auto root = doc.object();
    if(root.value("error").toBool()) { error = "天气接口拒绝请求：" + root.value("reason").toString().left(160); return false; }
    const auto c = root.value("current").toObject();
    const auto d = root.value("daily").toObject();
    if(!number(c.value("temperature_2m")) || !number(c.value("relative_humidity_2m")) ||
       !number(c.value("weather_code")) || !QDateTime::fromString(c.value("time").toString(), Qt::ISODate).isValid()) {
        error = "当前天气字段缺失或无效"; return false;
    }
    WeatherData candidate;
    candidate.temperature = c.value("temperature_2m").toDouble();
    candidate.humidity = c.value("relative_humidity_2m").toDouble();
    const double rawCode = c.value("weather_code").toDouble();
    if(candidate.humidity < 0 || candidate.humidity > 100 || rawCode < 0 || rawCode > 999 || std::floor(rawCode) != rawCode) {
        error = "湿度或天气代码无效"; return false;
    }
    candidate.code = int(rawCode); candidate.time = c.value("time").toString();
    const auto dates = d.value("time").toArray();
    const auto low = d.value("temperature_2m_min").toArray();
    const auto high = d.value("temperature_2m_max").toArray();
    if(dates.size() < 3 || low.size() != dates.size() || high.size() != dates.size()) {
        error = "三天预报数组不完整"; return false;
    }
    QDate previous;
    for(int i=0;i<3;++i) {
        const auto date = QDate::fromString(dates.at(i).toString(), Qt::ISODate);
        if(!date.isValid() || (i && date != previous.addDays(1)) || !number(low.at(i)) || !number(high.at(i)) || low.at(i).toDouble() > high.at(i).toDouble()) {
            error = "预报日期或温度数据无效"; return false;
        }
        previous = date; candidate.dates.append(date.toString("MM-dd"));
        candidate.low.append(low.at(i).toDouble()); candidate.high.append(high.at(i).toDouble());
    }
    data = candidate; error.clear(); return true;
}
WeatherPage::WeatherPage(QSettings &cfg, QWidget *parent) : QWidget(parent) {
    city = cfg.value("weather/city", "长沙").toString();
    latitude = cfg.value("weather/latitude", "28.2282").toString();
    longitude = cfg.value("weather/longitude", "112.9388").toString();
    curl = cfg.value("weather/curl", "/opt/weather-tools/bin/curl").toString();
    library = cfg.value("weather/library", "/opt/weather-tools/lib").toString();
    ca = cfg.value("weather/ca", "/opt/weather-tools/certs/ca-certificates.crt").toString();
    auto *layout = new QVBoxLayout(this);
    auto *top = new QHBoxLayout;
    auto *title = textLabel(city + " · 天气预报"); title->setStyleSheet("font-size:30px;color:#43dcd2;");
    top->addWidget(title, 1);
    refreshButton = new QPushButton("刷新天气"); refreshButton->setObjectName("weatherRefresh"); refreshButton->setMinimumHeight(58);
    top->addWidget(refreshButton); layout->addLayout(top);
    current = textLabel("温度 — °C    湿度 — %    天气 —", "weatherCurrent");
    current->setStyleSheet("font-size:28px;"); layout->addWidget(current);
    auto *row = new QHBoxLayout;
    for(int i=0;i<3;++i) {
        auto *day = textLabel("等待天气数据", QString("weatherDay%1").arg(i));
        day->setAlignment(Qt::AlignCenter);
        day->setStyleSheet("background:#17283a;border-radius:10px;padding:12px;font-size:23px;");
        row->addWidget(day,1); days.append(day);
    }
    layout->addLayout(row,1);
    updated = textLabel("尚无成功更新记录", "weatherUpdated"); updated->setStyleSheet("font-size:17px;"); layout->addWidget(updated);
    status = textLabel("等待首次请求", "weatherStatus"); status->setStyleSheet("font-size:18px;"); layout->addWidget(status);
    auto *source = textLabel("数据来源：Open-Meteo.com · CC BY 4.0\n城市坐标为手动配置；温湿度为城市天气数据，不是车内传感器测量。");
    source->setStyleSheet("font-size:16px;color:#b4c7da;"); layout->addWidget(source);
    connect(refreshButton, &QPushButton::clicked, this, [this]{ refresh(); });
    connect(&process, &QProcess::readyReadStandardOutput, this, [this]{ collect(); });
    connect(&process, &QProcess::readyReadStandardError, this, [this]{ collect(); });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e){
        if(e == QProcess::FailedToStart) { active=false; deadline.stop(); fail("无法启动 curl，请检查程序路径及依赖库"); }
    });
    connect(&process, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus state){ finish(code,state); });
    deadline.setSingleShot(true); deadline.setInterval(35000);
    connect(&deadline, &QTimer::timeout, this, [this]{ forcedError="请求超过 35 秒，已取消"; process.kill(); });
    const int minutes=qBound(1,cfg.value("weather/refresh_minutes",15).toInt(),1440);
    refreshTimer.setInterval(minutes*60000);
    connect(&refreshTimer, &QTimer::timeout, this, [this]{ refresh(); });
    refreshTimer.start();
    QTimer::singleShot(500,this,[this]{ refresh(); });
}
WeatherPage::~WeatherPage() {
    process.disconnect(this);
    if(process.state()!=QProcess::NotRunning) { process.kill(); process.waitForFinished(1000); }
}
void WeatherPage::fail(const QString &reason) {
    refreshButton->setEnabled(true);
    status->setText("更新失败：" + reason + (lastSuccess.isValid()?"\n当前保留上次成功数据，请留意日期和时间。":"\n请检查网络与系统时间，再点击刷新。"));
    status->setStyleSheet("font-size:18px;color:#ffba75;");
}
void WeatherPage::refresh() {
    if(active || process.state()!=QProcess::NotRunning) return;
    if(!QFileInfo(curl).isExecutable()) { fail("找不到可执行的 curl："+curl); return; }
    if(!QFileInfo(ca).isReadable()) { fail("找不到可读的 CA 证书"); return; }
    if(QDate::currentDate().year()<2024) { fail("系统时间未校准，HTTPS 证书无法正常验证"); return; }
    bool latOK=false, lonOK=false;
    const double lat=latitude.toDouble(&latOK), lon=longitude.toDouble(&lonOK);
    if(!latOK || !lonOK || !std::isfinite(lat) || !std::isfinite(lon) || std::abs(lat)>90 || std::abs(lon)>180) {
        fail("配置文件中的经纬度无效"); return;
    }
    QUrl url("https://api.open-meteo.com/v1/forecast"); QUrlQuery query;
    query.addQueryItem("latitude",latitude); query.addQueryItem("longitude",longitude);
    query.addQueryItem("current","temperature_2m,relative_humidity_2m,weather_code");
    query.addQueryItem("daily","temperature_2m_max,temperature_2m_min");
    query.addQueryItem("forecast_days","3"); query.addQueryItem("timezone","Asia/Shanghai");
    query.addQueryItem("temperature_unit","celsius"); url.setQuery(query);
    auto env=QProcessEnvironment::systemEnvironment();
    if(!library.isEmpty()) env.insert("LD_LIBRARY_PATH", library);
    process.setProcessEnvironment(env);
    // -q 必须位于首个参数，忽略用户 curlrc；不通过 shell 执行任何配置值。
    QStringList args={"-q","--fail","--silent","--show-error","--connect-timeout","10","--max-time","30",
                      "--proto","=https","--cacert",ca,url.toString(QUrl::FullyEncoded)};
    output.clear(); errors.clear(); forcedError.clear(); active=true;
    refreshButton->setEnabled(false);
    status->setStyleSheet("font-size:18px;color:#b4c7da;");
    status->setText("正在更新天气……音乐与触摸操作可继续使用。");
    deadline.start(); process.start(curl,args);
}
void WeatherPage::collect() {
    output+=process.readAllStandardOutput(); errors+=process.readAllStandardError();
    if(output.size()>256*1024 || errors.size()>32*1024) {
        forcedError="服务器响应过大"; output.truncate(256*1024); errors.truncate(32*1024);
        process.kill();
    }
}
void WeatherPage::finish(int code, QProcess::ExitStatus state) {
    deadline.stop(); collect(); active=false; refreshButton->setEnabled(true);
    if(!forcedError.isEmpty()) { fail(forcedError); return; }
    if(state!=QProcess::NormalExit || code!=0) {
        qWarning().noquote()<<"Weather curl:"<<code<<QString::fromUtf8(errors);
        QString reason;
        switch(code) {
        case 6: reason="域名解析失败，请检查 DNS"; break;
        case 7: reason="无法连接天气服务器"; break;
        case 22: reason="天气服务器返回 HTTP 错误"; break;
        case 28: reason="网络请求超时"; break;
        case 60: reason="证书验证失败，请检查系统日期与 CA 证书"; break;
        case 77: reason="无法加载 CA 证书文件"; break;
        default: reason=QString("curl 执行失败（代码 %1），详情见串口日志").arg(code); break;
        }
        fail(reason); return;
    }
    WeatherData data; QString error;
    if(!parseWeather(output,data,error)) { fail(error); return; }
    current->setText(QString("%1 °C    湿度 %2%    %3").arg(data.temperature,0,'f',1).arg(data.humidity,0,'f',0).arg(weatherDescription(data.code)));
    for(int i=0;i<3;++i) days[i]->setText(QString("%1\n最低 %2 °C\n最高 %3 °C").arg(data.dates[i]).arg(data.low[i],0,'f',1).arg(data.high[i],0,'f',1));
    lastSuccess=QDateTime::currentDateTime();
    updated->setText("天气数据时间："+data.time.replace('T',' ')+"（北京时间）\n获取时间："+lastSuccess.toString("yyyy-MM-dd HH:mm:ss"));
    status->setStyleSheet("font-size:18px;color:#43dcd2;");
    status->setText(QString("更新成功 · 每 %1 分钟自动刷新，也可手动刷新").arg(refreshTimer.interval()/60000));
}

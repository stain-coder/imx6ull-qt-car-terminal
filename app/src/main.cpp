#include "window.h"
#include <QApplication>
#include <QFontDatabase>
#include <QFont>
#include <QDebug>
#include <QLockFile>
#include <QDir>
#include <ctime>
#include <QTextCodec>
int main(int argc, char **argv) {
    qputenv("TZ", "CST-8");
    tzset();
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("car-terminal");
    // 避免重复打开界面并争用声卡。锁随正常退出释放。
    QLockFile lock(QDir::tempPath() + "/car-terminal.lock");
    if (!lock.tryLock(0)) { qCritical() << "Another instance is running"; return 1; }
    const QString config = app.arguments().size() > 1 ? app.arguments().at(1)
                                                    : "/opt/car-terminal/car-offline.ini";
    QSettings settings(config, QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    const int id = QFontDatabase::addApplicationFont(settings.value("ui/font",
                  "/opt/car-terminal/fonts/wqy-microhei.ttc").toString());
    if (id >= 0) {
        const auto families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty()) app.setFont(QFont(families.first(), 15));
    } else qWarning() << "Chinese font missing; see README";
    Window window(config);
    if (qEnvironmentVariableIsSet("CAR_WINDOWED")) { window.resize(1024,600); window.show(); }
    else window.showFullScreen();
    return app.exec();
}

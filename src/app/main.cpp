#include "loreforge/core/application_config.h"
#include "loreforge/core/build_info.h"
#include "loreforge/core/logging.h"
#include "main_window.h"

#include <QApplication>
#include <QTimer>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    loreforge::core::ApplicationConfig::applyMetadata();
    QApplication::setApplicationVersion(loreforge::core::BuildInfo::version());

    qCInfo(loreforgeApp) << "Starting LoreForge" << QApplication::applicationVersion();

    loreforge::app::MainWindow window;
    window.show();

    if (QApplication::arguments().contains(QStringLiteral("--smoke-test"))) {
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
    }

    return application.exec();
}

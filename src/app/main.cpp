#include "loreforge/core/application_config.h"
#include "loreforge/core/build_info.h"
#include "loreforge/core/logging.h"
#include "main_window.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    loreforge::core::ApplicationConfig::applyMetadata();
    QApplication::setApplicationVersion(loreforge::core::BuildInfo::version());

    qCInfo(loreforgeApp) << "Starting LoreForge" << QApplication::applicationVersion();

    loreforge::app::MainWindow window;
    if (QApplication::arguments().contains(QStringLiteral("--smoke-test"))) {
        return 0;
    }

    window.show();
    return application.exec();
}

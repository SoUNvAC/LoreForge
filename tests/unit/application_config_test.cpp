#include "loreforge/core/application_config.h"

#include <QCoreApplication>
#include <QtTest>

class ApplicationConfigTest final : public QObject {
    Q_OBJECT

  private slots:
    void appliesStableMetadata();
};

void ApplicationConfigTest::appliesStableMetadata() {
    loreforge::core::ApplicationConfig::applyMetadata();

    QCOMPARE(QCoreApplication::applicationName(), QStringLiteral("LoreForge"));
    QCOMPARE(QCoreApplication::organizationName(), QStringLiteral("LoreForge"));
    QCOMPARE(QCoreApplication::organizationDomain(), QStringLiteral("loreforge.local"));
}

QTEST_APPLESS_MAIN(ApplicationConfigTest)

#include "application_config_test.moc"

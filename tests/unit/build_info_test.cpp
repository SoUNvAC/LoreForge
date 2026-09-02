#include "loreforge/core/build_info.h"

#include <QtTest>

class BuildInfoTest final : public QObject {
    Q_OBJECT

  private slots:
    void exposesProjectVersion();
};

void BuildInfoTest::exposesProjectVersion() {
    QCOMPARE(loreforge::core::BuildInfo::version(), QStringLiteral("0.1.0"));
}

QTEST_APPLESS_MAIN(BuildInfoTest)

#include "build_info_test.moc"

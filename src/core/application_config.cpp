#include "loreforge/core/application_config.h"

#include <QCoreApplication>

namespace loreforge::core {

void ApplicationConfig::applyMetadata() {
    QCoreApplication::setOrganizationName(QString::fromUtf8(organizationName()));
    QCoreApplication::setOrganizationDomain(QString::fromUtf8(organizationDomain()));
    QCoreApplication::setApplicationName(QString::fromUtf8(applicationName()));
}

} // namespace loreforge::core

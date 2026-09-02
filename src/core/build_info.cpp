#include "loreforge/core/build_info.h"

namespace loreforge::core {

QString BuildInfo::version() {
    return QStringLiteral(LOREFORGE_VERSION);
}

} // namespace loreforge::core

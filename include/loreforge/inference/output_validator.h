#pragma once

#include "loreforge/inference/inference_types.h"

#include <QJsonValue>

namespace loreforge::inference {

class OutputValidator final {
  public:
    [[nodiscard]] static QStringList validateSchema(const QJsonObject& schema);
    [[nodiscard]] static ValidationReport validate(const QJsonObject& schema,
                                                   const QJsonValue& instance);
};

} // namespace loreforge::inference

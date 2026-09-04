#include "loreforge/inference/output_validator.h"

#include <QJsonArray>
#include <QSet>

#include <cmath>
#include <utility>

namespace loreforge::inference {
namespace {

const QSet<QString> supportedKeywords{
    QStringLiteral("$schema"),     QStringLiteral("title"),
    QStringLiteral("description"), QStringLiteral("type"),
    QStringLiteral("enum"),        QStringLiteral("required"),
    QStringLiteral("properties"),  QStringLiteral("additionalProperties"),
    QStringLiteral("items"),
};

bool isSupportedType(QStringView type) {
    return type == QStringLiteral("null") || type == QStringLiteral("boolean") ||
           type == QStringLiteral("object") || type == QStringLiteral("array") ||
           type == QStringLiteral("number") || type == QStringLiteral("integer") ||
           type == QStringLiteral("string");
}

void validateSchemaObject(const QJsonObject& schema, QStringView path, QStringList& errors) {
    for (auto iterator = schema.begin(); iterator != schema.end(); ++iterator) {
        if (!supportedKeywords.contains(iterator.key())) {
            errors.append(
                QStringLiteral("%1: unsupported schema keyword '%2'.").arg(path, iterator.key()));
        }
    }

    const auto type = schema.value(QStringLiteral("type"));
    if (!type.isUndefined()) {
        if (type.isString()) {
            if (!isSupportedType(type.toString())) {
                errors.append(QStringLiteral("%1.type: unsupported JSON type.").arg(path));
            }
        } else if (type.isArray()) {
            QSet<QString> seen;
            const auto types = type.toArray();
            if (types.isEmpty()) {
                errors.append(
                    QStringLiteral("%1.type: expected at least one supported type.").arg(path));
            }
            for (const auto& value : types) {
                if (!value.isString() || !isSupportedType(value.toString()) ||
                    seen.contains(value.toString())) {
                    errors.append(
                        QStringLiteral("%1.type: expected unique supported types.").arg(path));
                    break;
                }
                seen.insert(value.toString());
            }
        } else {
            errors.append(QStringLiteral("%1.type: expected a string or array.").arg(path));
        }
    }

    const auto enumValues = schema.value(QStringLiteral("enum"));
    if (!enumValues.isUndefined() && (!enumValues.isArray() || enumValues.toArray().isEmpty())) {
        errors.append(QStringLiteral("%1.enum: expected a non-empty array.").arg(path));
    } else if (enumValues.isArray()) {
        const auto values = enumValues.toArray();
        for (qsizetype index = 0; index < values.size(); ++index) {
            for (qsizetype candidate = index + 1; candidate < values.size(); ++candidate) {
                if (values.at(index) == values.at(candidate)) {
                    errors.append(QStringLiteral("%1.enum: expected unique values.").arg(path));
                    index = values.size();
                    break;
                }
            }
        }
    }
    for (const auto& annotation :
         {QStringLiteral("$schema"), QStringLiteral("title"), QStringLiteral("description")}) {
        const auto value = schema.value(annotation);
        if (!value.isUndefined() && !value.isString()) {
            errors.append(QStringLiteral("%1.%2: expected a string.").arg(path, annotation));
        }
    }
    const auto required = schema.value(QStringLiteral("required"));
    if (!required.isUndefined()) {
        if (!required.isArray()) {
            errors.append(QStringLiteral("%1.required: expected an array.").arg(path));
        } else {
            QSet<QString> seen;
            for (const auto& value : required.toArray()) {
                if (!value.isString() || seen.contains(value.toString())) {
                    errors.append(
                        QStringLiteral("%1.required: expected unique strings.").arg(path));
                    break;
                }
                seen.insert(value.toString());
            }
        }
    }

    const auto properties = schema.value(QStringLiteral("properties"));
    if (!properties.isUndefined()) {
        if (!properties.isObject()) {
            errors.append(QStringLiteral("%1.properties: expected an object.").arg(path));
        } else {
            const auto object = properties.toObject();
            for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
                if (!iterator.value().isObject()) {
                    errors.append(QStringLiteral("%1.properties.%2: expected a schema object.")
                                      .arg(path, iterator.key()));
                    continue;
                }
                validateSchemaObject(iterator.value().toObject(),
                                     QStringLiteral("%1.properties.%2").arg(path, iterator.key()),
                                     errors);
            }
        }
    }

    const auto additional = schema.value(QStringLiteral("additionalProperties"));
    if (!additional.isUndefined() && !additional.isBool()) {
        errors.append(
            QStringLiteral("%1.additionalProperties: only booleans are supported.").arg(path));
    }
    const auto items = schema.value(QStringLiteral("items"));
    if (!items.isUndefined()) {
        if (!items.isObject()) {
            errors.append(QStringLiteral("%1.items: expected a schema object.").arg(path));
        } else {
            validateSchemaObject(items.toObject(), QStringLiteral("%1.items").arg(path), errors);
        }
    }
}

bool matchesType(QStringView type, const QJsonValue& value) {
    if (type == QStringLiteral("null")) {
        return value.isNull();
    }
    if (type == QStringLiteral("boolean")) {
        return value.isBool();
    }
    if (type == QStringLiteral("object")) {
        return value.isObject();
    }
    if (type == QStringLiteral("array")) {
        return value.isArray();
    }
    if (type == QStringLiteral("number")) {
        return value.isDouble();
    }
    if (type == QStringLiteral("integer")) {
        return value.isDouble() && std::floor(value.toDouble()) == value.toDouble();
    }
    if (type == QStringLiteral("string")) {
        return value.isString();
    }
    return false;
}

void validateInstance(const QJsonObject& schema, const QJsonValue& value, QStringView path,
                      QStringList& errors) {
    const auto type = schema.value(QStringLiteral("type"));
    bool typeMatches = true;
    if (type.isString()) {
        typeMatches = matchesType(type.toString(), value);
    } else if (type.isArray()) {
        typeMatches = false;
        for (const auto& candidate : type.toArray()) {
            typeMatches = typeMatches || matchesType(candidate.toString(), value);
        }
    }
    if (!typeMatches) {
        errors.append(QStringLiteral("%1: value does not match schema type.").arg(path));
        return;
    }

    const auto enumValues = schema.value(QStringLiteral("enum"));
    if (enumValues.isArray() && !enumValues.toArray().contains(value)) {
        errors.append(QStringLiteral("%1: value is not in the allowed enum.").arg(path));
    }

    if (value.isObject()) {
        const auto object = value.toObject();
        const auto required = schema.value(QStringLiteral("required")).toArray();
        for (const auto& requiredValue : required) {
            const auto name = requiredValue.toString();
            if (!object.contains(name)) {
                errors.append(
                    QStringLiteral("%1.%2: required property is missing.").arg(path, name));
            }
        }
        const auto properties = schema.value(QStringLiteral("properties")).toObject();
        for (auto iterator = properties.begin(); iterator != properties.end(); ++iterator) {
            if (object.contains(iterator.key())) {
                validateInstance(iterator.value().toObject(), object.value(iterator.key()),
                                 QStringLiteral("%1.%2").arg(path, iterator.key()), errors);
            }
        }
        if (schema.value(QStringLiteral("additionalProperties")).isBool() &&
            !schema.value(QStringLiteral("additionalProperties")).toBool()) {
            for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
                if (!properties.contains(iterator.key())) {
                    errors.append(QStringLiteral("%1.%2: additional property is not allowed.")
                                      .arg(path, iterator.key()));
                }
            }
        }
    }

    if (value.isArray() && schema.value(QStringLiteral("items")).isObject()) {
        const auto items = schema.value(QStringLiteral("items")).toObject();
        const auto array = value.toArray();
        for (qsizetype index = 0; index < array.size(); ++index) {
            validateInstance(items, array.at(index), QStringLiteral("%1[%2]").arg(path).arg(index),
                             errors);
        }
    }
}

} // namespace

ValidationReport OutputValidator::validate(const QJsonObject& schema, const QJsonValue& instance) {
    auto errors = validateSchema(schema);
    if (!errors.isEmpty()) {
        return {ValidationStatus::Invalid, std::move(errors)};
    }
    validateInstance(schema, instance, QStringLiteral("$"), errors);
    return {errors.isEmpty() ? ValidationStatus::Valid : ValidationStatus::Invalid,
            std::move(errors)};
}

QStringList OutputValidator::validateSchema(const QJsonObject& schema) {
    QStringList errors;
    validateSchemaObject(schema, QStringLiteral("$schema"), errors);
    return errors;
}

} // namespace loreforge::inference

#include "chapter_evidence.h"

#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QStringDecoder>

namespace loreforge::app {
namespace {
QJsonValue transformSchema(const QJsonValue& value) {
    if (value.isArray()) {
        QJsonArray result;
        for (const auto& child : value.toArray())
            result.append(transformSchema(child));
        return result;
    }
    if (!value.isObject())
        return value;
    auto object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it)
        it.value() = transformSchema(it.value());
    auto properties = object.value(QStringLiteral("properties")).toObject();
    if (properties.contains(QStringLiteral("source_start")) &&
        properties.contains(QStringLiteral("source_end"))) {
        properties.remove(QStringLiteral("source_start"));
        properties.remove(QStringLiteral("source_end"));
        properties.insert(QStringLiteral("block_id"),
                          QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
        object.insert(QStringLiteral("properties"), properties);
        QJsonArray required;
        for (const auto& item : object.value(QStringLiteral("required")).toArray())
            if (item.toString() != QStringLiteral("source_start") &&
                item.toString() != QStringLiteral("source_end"))
                required.append(item);
        required.append(QStringLiteral("block_id"));
        object.insert(QStringLiteral("required"), required);
    }
    return object;
}
bool validUtf8(const QByteArray& bytes) {
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const auto decoded = QString(decoder.decode(bytes));
    Q_UNUSED(decoded);
    return !decoder.hasError();
}
} // namespace

QJsonObject blockReferenceSchema(const QJsonObject& schema) {
    return transformSchema(schema).toObject();
}

QJsonObject chapterAnalysisWireSchema(const QJsonObject& schema) {
    auto result = blockReferenceSchema(schema);
    auto properties = result.value(QStringLiteral("properties")).toObject();
    const QJsonObject stringType{{QStringLiteral("type"), QStringLiteral("string")}};
    const QJsonObject kindType{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("enum"), QJsonArray{QStringLiteral("dialogue"), QStringLiteral("narration"),
                                            QStringLiteral("unknown")}}};
    const QJsonObject part{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("text"), QStringLiteral("kind"), QStringLiteral("speaker")}},
        {QStringLiteral("properties"), QJsonObject{{QStringLiteral("text"), stringType},
                                                   {QStringLiteral("speaker"), stringType},
                                                   {QStringLiteral("kind"), kindType}}}};
    properties.insert(QStringLiteral("segments"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                  {QStringLiteral("items"), part}});
    result.insert(QStringLiteral("properties"), properties);
    auto required = result.value(QStringLiteral("required")).toArray();
    required.append(QStringLiteral("segments"));
    result.insert(QStringLiteral("required"), required);
    return result;
}

QJsonObject narrativeOutput(QJsonObject normalized) {
    normalized.remove(QStringLiteral("segments"));
    return normalized;
}

ResolvedChapterEvidence resolveChapterEvidence(const QJsonObject& output,
                                               const QJsonObject& snapshot, bool review) {
    ResolvedChapterEvidence result;
    if (!output.value(QStringLiteral("chapter_id")).isString() ||
        output.value(QStringLiteral("chapter_id")) !=
            snapshot.value(QStringLiteral("chapter_id"))) {
        result.errors.append(QStringLiteral("$.chapter_id: 证据编号只能用于其所属章节。"));
        return result;
    }
    const auto source = snapshot.value(QStringLiteral("source")).toObject();
    const auto bytes = source.value(QStringLiteral("utf8")).toString().toUtf8();
    const auto base = source.value(QStringLiteral("start_byte")).toInteger(-1);
    const auto end = source.value(QStringLiteral("end_byte")).toInteger(-1);
    QMap<QString, QJsonObject> ranges;
    const auto blocks = snapshot.value(QStringLiteral("blocks")).toArray();
    if (base < 0 || end < base || end - base != bytes.size() || !validUtf8(bytes) ||
        blocks.isEmpty()) {
        result.errors.append(QStringLiteral("证据快照来源或段落表无效，未换算证据。"));
        return result;
    }
    qint64 previousEnd = base;
    for (qsizetype index = 0; index < blocks.size(); ++index) {
        const auto block = blocks[index].toObject();
        const auto id = block.value(QStringLiteral("block_id")).toString();
        const auto startValue = block.value(QStringLiteral("start_byte"));
        const auto endValue = block.value(QStringLiteral("end_byte"));
        const auto start = startValue.toInteger(-1);
        const auto stop = endValue.toInteger(-1);
        if (id != QStringLiteral("b%1").arg(index + 1, 4, 10, QLatin1Char('0')) ||
            ranges.contains(id) || !startValue.isDouble() || !endValue.isDouble() ||
            startValue.toDouble() != static_cast<double>(start) ||
            endValue.toDouble() != static_cast<double>(stop) || start < previousEnd ||
            (index == 0 && start != base) || stop <= start || stop > end ||
            (index == blocks.size() - 1 && stop != end) ||
            !validUtf8(bytes.mid(start - base, stop - start))) {
            result.errors.append(
                QStringLiteral("证据段落表范围、编号或 UTF-8 边界无效，未换算证据。"));
            return result;
        }
        previousEnd = stop;
        ranges.insert(
            id, {{QStringLiteral("source_start"), start}, {QStringLiteral("source_end"), stop}});
    }
    const auto resolve = [&](const QJsonObject& reference, const QString& path, bool issue) {
        auto normalized = reference;
        const auto id = reference.value(QStringLiteral("block_id"));
        const auto range = ranges.constFind(id.toString());
        const auto validShape = reference.size() == (issue ? 2 : 1) &&
                                (!issue || reference.value(QStringLiteral("reason")).isString());
        if (!validShape || !id.isString() || range == ranges.cend()) {
            result.errors.append(
                path +
                QStringLiteral(": 证据必须引用当前章段落表中的有效 block_id；不能提供字节偏移。"));
            return QJsonObject{};
        }
        normalized.remove(QStringLiteral("block_id"));
        normalized.insert(QStringLiteral("source_start"),
                          range->value(QStringLiteral("source_start")));
        normalized.insert(QStringLiteral("source_end"), range->value(QStringLiteral("source_end")));
        return normalized;
    };
    const auto walk = [&](auto&& self, const QJsonValue& value, const QString& path) -> QJsonValue {
        if (value.isArray()) {
            QJsonArray array;
            qsizetype index = 0;
            for (const auto& child : value.toArray())
                array.append(self(self, child, path + QStringLiteral("[%1]").arg(index++)));
            return array;
        }
        if (!value.isObject())
            return value;
        auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            const auto childPath = path + QLatin1Char('.') + it.key();
            if (it.key() == QStringLiteral("evidence")) {
                if (!it.value().isArray()) {
                    result.errors.append(childPath + QStringLiteral(": evidence 必须是数组。"));
                    continue;
                }
                QJsonArray array;
                QSet<QString> seen;
                qsizetype index = 0;
                for (const auto& reference : it.value().toArray()) {
                    const auto id =
                        reference.toObject().value(QStringLiteral("block_id")).toString();
                    const auto evidencePath = childPath + QStringLiteral("[%1]").arg(index++);
                    if (seen.contains(id))
                        result.errors.append(evidencePath +
                                             QStringLiteral(": 同一结论不能重复引用同一段落。"));
                    seen.insert(id);
                    array.append(resolve(reference.toObject(), evidencePath, false));
                }
                it.value() = array;
            } else
                it.value() = self(self, it.value(), childPath);
        }
        return object;
    };
    auto normalized = output;
    if (review) {
        if (!output.value(QStringLiteral("issues")).isArray())
            result.errors.append(QStringLiteral("$.issues 必须是数组。"));
        QJsonArray issues;
        qsizetype index = 0;
        for (const auto& issue : output.value(QStringLiteral("issues")).toArray())
            issues.append(
                resolve(issue.toObject(), QStringLiteral("$.issues[%1]").arg(index++), true));
        normalized.insert(QStringLiteral("issues"), issues);
    } else
        normalized = walk(walk, output, QStringLiteral("$")).toObject();
    if (!review && snapshot.value(QStringLiteral("schema_version")).toInt() >= 3) {
        const auto segments = output.value(QStringLiteral("segments"));
        QSet<QString> names;
        for (const auto& character : output.value(QStringLiteral("characters")).toArray())
            names.insert(character.toObject().value(QStringLiteral("name")).toString());
        QJsonArray resolved;
        qint64 offset = 0;
        if (!segments.isArray() || segments.toArray().isEmpty())
            result.errors.append(QStringLiteral("$.segments: 缺少完整正文分割结果。"));
        for (const auto& item : segments.toArray()) {
            auto segment = item.toObject();
            const auto text = segment.value(QStringLiteral("text")).toString().toUtf8();
            const auto kind = segment.value(QStringLiteral("kind")).toString();
            const auto speaker = segment.value(QStringLiteral("speaker")).toString();
            if (segment.size() != 3 || !segment.value(QStringLiteral("text")).isString() ||
                !segment.value(QStringLiteral("speaker")).isString() || text.isEmpty() ||
                (kind != QStringLiteral("dialogue") && kind != QStringLiteral("narration") &&
                 kind != QStringLiteral("unknown")) ||
                (kind != QStringLiteral("dialogue") && !speaker.isEmpty()) ||
                (!speaker.isEmpty() && !names.contains(speaker)) || offset > bytes.size() ||
                text.size() > bytes.size() - offset || bytes.mid(offset, text.size()) != text) {
                result.errors.append(
                    QStringLiteral(
                        "$.segments[%1]: "
                        "分割必须按顺序逐字覆盖原文；类型或说话人无效。不可把叙述归给人物。")
                        .arg(resolved.size()));
                break;
            }
            segment.insert(QStringLiteral("source_start"), base + offset);
            offset += text.size();
            segment.insert(QStringLiteral("source_end"), base + offset);
            resolved.append(segment);
        }
        if (offset != bytes.size())
            result.errors.append(QStringLiteral(
                "$.segments: 分割未完整覆盖原文（包括标点、标题和换行），未接受结果。"));
        normalized.insert(QStringLiteral("segments"), resolved);
    }
    if (result.errors.isEmpty())
        result.output = normalized;
    return result;
}
} // namespace loreforge::app

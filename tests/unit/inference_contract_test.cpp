#include "loreforge/inference/inference_types.h"
#include "loreforge/inference/output_validator.h"

#include <QJsonArray>
#include <QtTest>

using namespace Qt::StringLiterals;

class InferenceContractTest final : public QObject {
    Q_OBJECT

  private slots:
    void hashesCanonicalVersionedContent();
    void validatesTheSupportedSchemaSubset();
    void reportsInvalidStructuredOutput();
    void rejectsUnsupportedSchemaKeywords();
};

namespace {

QJsonObject extractionSchema() {
    const QJsonObject kindSchema{
        {u"type"_s, u"string"_s},
        {u"enum"_s, QJsonArray{u"narration"_s, u"dialogue"_s}},
    };
    const QJsonObject segmentSchema{
        {u"type"_s, u"object"_s},
        {u"required"_s, QJsonArray{u"kind"_s, u"text"_s}},
        {u"properties"_s,
         QJsonObject{
             {u"kind"_s, kindSchema},
             {u"text"_s, QJsonObject{{u"type"_s, u"string"_s}}},
             {u"speaker"_s, QJsonObject{{u"type"_s, QJsonArray{u"string"_s, u"null"_s}}}},
         }},
        {u"additionalProperties"_s, false},
    };
    return {
        {u"$schema"_s, u"https://json-schema.org/draft/2020-12/schema"_s},
        {u"type"_s, u"object"_s},
        {u"required"_s, QJsonArray{u"segments"_s}},
        {u"properties"_s, QJsonObject{{u"segments"_s, QJsonObject{{u"type"_s, u"array"_s},
                                                                  {u"items"_s, segmentSchema}}}}},
        {u"additionalProperties"_s, false},
    };
}

} // namespace

void InferenceContractTest::hashesCanonicalVersionedContent() {
    const auto createdAt = QDateTime::fromString(u"2026-09-04T02:00:00.000Z"_s, Qt::ISODateWithMs);
    const loreforge::inference::PromptTemplate prompt{
        loreforge::core::PromptTemplateId::fromStableKey(u"extract-v1"_s), u"Extraction"_s};
    const auto first = loreforge::inference::makePromptVersion(
        prompt, 1, u"Extract structured facts."_s, createdAt);
    const auto second = loreforge::inference::makePromptVersion(
        prompt, 1, u"Extract structured facts."_s, createdAt);
    QCOMPARE(first, second);
    QVERIFY(first.contentHash.isValid());

    const QJsonObject ordered{{u"alpha"_s, 1}, {u"beta"_s, QJsonArray{2, 3}}};
    QJsonObject reversed;
    reversed.insert(u"beta"_s, QJsonArray{2, 3});
    reversed.insert(u"alpha"_s, 1);
    QCOMPARE(loreforge::inference::canonicalJson(ordered),
             loreforge::inference::canonicalJson(reversed));

    const auto projectId = loreforge::core::ProjectId::fromStableKey(u"project"_s);
    const auto snapshotId = loreforge::core::ContextSnapshotId::fromStableKey(u"snapshot"_s);
    const auto snapshot =
        loreforge::inference::makeContextSnapshot(snapshotId, projectId, ordered, createdAt);
    QCOMPARE(snapshot.contentHash,
             loreforge::core::ContentHash::sha256(loreforge::inference::canonicalJson(reversed)));
}

void InferenceContractTest::validatesTheSupportedSchemaSubset() {
    const QJsonObject output{
        {u"segments"_s,
         QJsonArray{QJsonObject{
             {u"kind"_s, u"dialogue"_s}, {u"text"_s, u"Hello."_s}, {u"speaker"_s, u"Ada"_s}}}},
    };
    const auto report = loreforge::inference::OutputValidator::validate(extractionSchema(), output);
    QVERIFY(report.isValid());
    QVERIFY(report.errors.isEmpty());
}

void InferenceContractTest::reportsInvalidStructuredOutput() {
    const QJsonObject output{
        {u"segments"_s,
         QJsonArray{QJsonObject{
             {u"kind"_s, u"stage-direction"_s}, {u"text"_s, 42}, {u"unexpected"_s, true}}}},
        {u"extra"_s, true},
    };
    const auto report = loreforge::inference::OutputValidator::validate(extractionSchema(), output);
    QCOMPARE(report.status, loreforge::inference::ValidationStatus::Invalid);
    QCOMPARE(report.errors.size(), 4);
    QVERIFY(report.errors.join(QLatin1Char('\n')).contains(u"allowed enum"_s));
    QVERIFY(report.errors.join(QLatin1Char('\n')).contains(u"schema type"_s));
    QVERIFY(report.errors.join(QLatin1Char('\n')).contains(u"unexpected"_s));
    QVERIFY(report.errors.join(QLatin1Char('\n')).contains(u"$.extra"_s));
}

void InferenceContractTest::rejectsUnsupportedSchemaKeywords() {
    auto schema = extractionSchema();
    schema.insert(u"oneOf"_s, QJsonArray{});
    const auto errors = loreforge::inference::OutputValidator::validateSchema(schema);
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors.front().contains(u"unsupported schema keyword 'oneOf'"_s));

    const auto report = loreforge::inference::OutputValidator::validate(schema, QJsonObject{});
    QCOMPARE(report.status, loreforge::inference::ValidationStatus::Invalid);
    QCOMPARE(report.errors, errors);
}

QTEST_GUILESS_MAIN(InferenceContractTest)

#include "inference_contract_test.moc"

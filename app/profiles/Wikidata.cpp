#include "Wikidata.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

namespace kiebitz::app {

namespace {

/// The Wikipedias whose articles are kept: English, and the app's languages.
constexpr auto kWikis = {"en", "de", "no"};

} // namespace

QString WikiPerson::article(const QString& language) const {
    // Norwegian Bokmål's Wikipedia is "no".
    const QString wiki =
        language.startsWith(QStringLiteral("nb")) ? QStringLiteral("no") : language.left(2);
    if (const auto found = articles.find(wiki); found != articles.end()) {
        return found->second;
    }
    const auto english = articles.find(QStringLiteral("en"));
    return english != articles.end() ? english->second : QString();
}

QString wikidataQuery(int fideId) {
    QString wikis;
    for (const char* wiki : kWikis) {
        wikis += QStringLiteral("<https://%1.wikipedia.org/> ").arg(QLatin1String(wiki));
    }
    return QStringLiteral("SELECT ?item ?lichess ?photo ?article WHERE { ?item wdt:P1440 \"%1\" . "
                          "OPTIONAL { ?item wdt:P8976 ?lichess } OPTIONAL { ?item wdt:P18 ?photo } "
                          "OPTIONAL { ?article schema:about ?item ; schema:isPartOf ?wiki . "
                          "VALUES ?wiki { %2} } } LIMIT 50")
        .arg(fideId)
        .arg(wikis);
}

WikiPerson parseWikidata(const QByteArray& json) {
    WikiPerson person;
    const QJsonArray rows = QJsonDocument::fromJson(json)
                                .object()[QStringLiteral("results")]
                                .toObject()[QStringLiteral("bindings")]
                                .toArray();
    const auto value = [](const QJsonObject& row, const char* name) {
        return row[QLatin1String(name)].toObject()[QStringLiteral("value")].toString();
    };
    for (const auto& entry : rows) {
        const QJsonObject row = entry.toObject();
        const QString item = value(row, "item").section(u'/', -1);
        if (item.isEmpty()) {
            continue;
        }
        if (!person.item.isEmpty() && person.item != item) {
            return {}; // two people with one FIDE id: neither is sure
        }
        person.item = item;
        if (const QString name = value(row, "lichess");
            !name.isEmpty() && !person.lichess.contains(name)) {
            person.lichess << name;
        }
        if (const QString photo = value(row, "photo"); !photo.isEmpty() && person.photo.isEmpty()) {
            // ".../wiki/Special:FilePath/Magnus%20Carlsen%20in%202025.jpg"
            person.photo = QUrl::fromPercentEncoding(photo.section(u'/', -1).toUtf8());
        }
        if (const QString article = value(row, "article"); !article.isEmpty()) {
            // "https://de.wikipedia.org/wiki/..."
            const QString wiki = QUrl(article).host().section(u'.', 0, 0);
            person.articles.emplace(wiki, article);
        }
    }
    return person;
}

QString commonsPhotoPath(const QString& file, int width) {
    return QStringLiteral("/wiki/Special:FilePath/") +
           QString::fromUtf8(QUrl::toPercentEncoding(file)) +
           QStringLiteral("?width=%1").arg(width);
}

} // namespace kiebitz::app

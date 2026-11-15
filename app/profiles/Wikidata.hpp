#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <map>

namespace kiebitz::app {

/// What Wikidata knows about a FIDE player (well-known players only; most club players
/// aren't there): their Lichess accounts, a photo on Wikimedia Commons, Wikipedia articles.
struct WikiPerson {
    QString item;                        ///< "Q106807", or empty: not on Wikidata
    QStringList lichess;                 ///< Lichess usernames (property P8976)
    QString photo;                       ///< the photo's file name on Commons (P18), or empty
    std::map<QString, QString> articles; ///< Wikipedia articles by language ("en", "de", "no")

    /// The article in `language` (the app's: "de", "nb"...), else in English, else empty.
    [[nodiscard]] QString article(const QString& language) const;
    friend bool operator==(const WikiPerson&, const WikiPerson&) = default;
};

/// The SPARQL query for the player with this FIDE id (Wikidata's query service,
/// query.wikidata.org/sparql): the item, its Lichess usernames, photo and Wikipedia articles.
[[nodiscard]] QString wikidataQuery(int fideId);

/// The answer to that query (SPARQL results as JSON); an empty item when there is none, or
/// when several items share the id (then which one is unclear).
[[nodiscard]] WikiPerson parseWikidata(const QByteArray& json);

/// Commons' address of the photo at this width ("/wiki/Special:FilePath/<file>?width=160"),
/// relative to the Commons server.
[[nodiscard]] QString commonsPhotoPath(const QString& file, int width);

} // namespace kiebitz::app

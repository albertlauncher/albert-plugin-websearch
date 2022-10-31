// Copyright (c) 2022 Manuel Schneider

#include "configwidget.h"
#include "enginesmodel.h"
#include "plugin.h"
#include <QDesktopServices>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <array>
#include <vector>
using namespace std;
ALBERT_LOGGING

const char * ENGINES_FILE_NAME = "engines.json";

vector<SearchEngine> defaultSearchEngines = {
    {"Google",        "gg ",  ":google",    "https://www.google.com/search?q=%s"},
    {"Youtube",       "yt ",  ":youtube",   "https://www.youtube.com/results?search_query=%s"},
    {"Amazon",        "ama ", ":amazon",    "http://www.amazon.com/s/?field-keywords=%s"},
    {"Ebay",          "eb ",  ":ebay",      "http://www.ebay.com/sch/i.html?_nkw=%s"},
    {"GitHub",        "gh ",  ":github",    "https://github.com/search?utf8=✓&q=%s"},
    {"Wolfram Alpha", "=",    ":wolfram",   "https://www.wolframalpha.com/input/?i=%s"},
    {"DuckDuckGo",    "dd ",  ":duckduckgo","https://duckduckgo.com/?q=%s"},
};


Plugin::Plugin() : engines_json(configDir().filePath(ENGINES_FILE_NAME))
{
    // Deserialize engines
    QFile file(engines_json);
    if (file.open(QIODevice::ReadOnly)) {
        QJsonArray array = QJsonDocument::fromJson(file.readAll()).array();
        SearchEngine searchEngine;
        for (const auto &value : array) {
            QJsonObject object = value.toObject();
            searchEngine.name     = object["name"].toString();
            searchEngine.trigger  = object["trigger"].toString();
            searchEngine.iconPath = object["iconPath"].toString();
            searchEngine.url      = object["url"].toString();
            searchEngines_.push_back(searchEngine);
        }
    } else {
        INFO << qPrintable(QString("No engines file found. Using defaults. (%1).").arg(engines_json));
        setEngines(defaultSearchEngines);
    }
}


QWidget *Plugin::buildConfigWidget()
{
    return new ConfigWidget(const_cast<Plugin*>(this));
}


const vector<SearchEngine> &Plugin::engines() const
{
    return searchEngines_;
}

void Plugin::setEngines(const vector<SearchEngine> &engines)
{
    searchEngines_ = engines;
    emit enginesChanged(searchEngines_);

    // Serialize the engines
    QFile file(engines_json);
    if (file.open(QIODevice::WriteOnly)) {
        QJsonArray array;
        for ( const SearchEngine& searchEngine : searchEngines_ ) {
            QJsonObject object;
            object["name"]     = searchEngine.name;
            object["url"]      = searchEngine.url;
            object["trigger"]  = searchEngine.trigger;
            object["iconPath"] = searchEngine.iconPath;
            array.append(object);
        }
        file.write(QJsonDocument(array).toJson());
    } else
        CRIT << qPrintable(QString("Could not write to file: '%1'.").arg(file.fileName()));
}


void ::Plugin::restoreDefaultEngines()
{
    setEngines(defaultSearchEngines);
}

static shared_ptr<albert::StandardItem> buildItem(const SearchEngine &se, const QString &search_term)
{
    QString url = QString(se.url).replace("%s", QUrl::toPercentEncoding(search_term));
    return albert::StandardItem::make(
            se.name,
            se.name,
            QString("Search %1 for '%2'").arg(se.name, search_term),
            QString("%1 %2").arg(se.name, search_term),
            {QString("xdg:%1").arg(se.name.toLower()), se.iconPath},
            {{"run", "Run websearch", [url](){ albert::openUrl(url); }}}
    );
}

vector<albert::RankItem> Plugin::rankItems(const QString &string, const bool& isValid) const
{
    vector<albert::RankItem> results;

    if (!string.isEmpty())
        for (const SearchEngine &se: searchEngines_)
            for (const auto &keyword : {se.trigger.toLower(), QString("%1 ").arg(se.name.toLower())})
                if (auto prefix = string.toLower().left(keyword.size()); keyword.startsWith(prefix)){
                    results.emplace_back(buildItem(se, string.mid(prefix.size())),
                                         (albert::Score) ((double) prefix.length() / (double) keyword.size() *
                                                          albert::MAX_SCORE));
                    break;  // max one of these icons, assumption: tigger is shorter and yields higer scores

                }
    applyUsageScores(results);
    return results;
}

vector<shared_ptr<albert::Item>> Plugin::fallbacks(const QString &query) const
{
    vector<shared_ptr<albert::Item>> results;
    if (!query.isEmpty())
        for (const SearchEngine &se: searchEngines_)
            results.emplace_back(buildItem(se, query.isEmpty()?"…":query));
    return results;
}

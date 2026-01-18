// Copyright (c) 2022-2023 Manuel Schneider

#include "configwidget.h"
#include "plugin.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <albert/icon.h>
#include <albert/logging.h>
#include <albert/matcher.h>
#include <albert/networkutil.h>
#include <albert/standarditem.h>
#include <albert/systemutil.h>
#include <array>
#include <vector>
ALBERT_LOGGING_CATEGORY("websearch")
using namespace Qt::StringLiterals;
using namespace albert;
using namespace std;

namespace {
static const auto &ENGINES_FILE_NAME  = u"engines.json"_s;
static const auto &CK_ENGINE_ID       = u"id"_s;
static const auto &CK_ENGINE_GUID     = u"guid"_s;  // To be removed in future releases
static const auto &CK_ENGINE_NAME     = u"name"_s;
static const auto &CK_ENGINE_URL      = u"url"_s;
static const auto &CK_ENGINE_TRIGGER  = u"trigger"_s;
static const auto &CK_ENGINE_ICON     = u"iconPath"_s;
static const auto &CK_ENGINE_FALLBACK = u"fallback"_s;
}

static QByteArray serializeEngines(const vector<SearchEngine> &engines)
{
    QJsonArray a;
    for (const SearchEngine& e : engines)
    {
        QJsonObject o;
        o[CK_ENGINE_ID] = e.id;
        o[CK_ENGINE_NAME] = e.name;
        o[CK_ENGINE_URL] = e.url;
        o[CK_ENGINE_TRIGGER] = e.trigger;
        o[CK_ENGINE_ICON] = e.icon_path;
        o[CK_ENGINE_FALLBACK] = e.fallback;
        a.append(o);
    }
    return QJsonDocument(a).toJson();
}

static vector<SearchEngine> deserializeEngines(const QByteArray &json)
{
    vector<SearchEngine> searchEngines;
    const auto a = QJsonDocument::fromJson(json).array();
    for (const auto &v : a)
    {
        QJsonObject o = v.toObject();
        SearchEngine e;

        // Todo remove this in future releasea
        if (o.contains(CK_ENGINE_ID))
            e.id = o[CK_ENGINE_ID].toString();
        else if (o.contains(CK_ENGINE_GUID))
            e.id = o[CK_ENGINE_GUID].toString();

        if (e.id.isEmpty())
            e.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

        e.name = o[CK_ENGINE_NAME].toString();
        e.trigger = o[CK_ENGINE_TRIGGER].toString().trimmed();
        e.icon_path = o[CK_ENGINE_ICON].toString();
        e.url = o[CK_ENGINE_URL].toString();
        // change this to false in future releases
        // For now while users configs do not have the fallback key,
        // we assume that all engines are fallbacks
        e.fallback = o[CK_ENGINE_FALLBACK].toBool(true);
        searchEngines.push_back(e);
    }
    return searchEngines;
}

Plugin::Plugin()
{
    filesystem::create_directories(dataLocation());
    filesystem::create_directories(configLocation());

    QFile f(QDir(configLocation()).filePath(ENGINES_FILE_NAME));
    if (f.open(QIODevice::ReadOnly))
        setEngines(deserializeEngines(f.readAll()));
    else
        restoreDefaultEngines();
}

const vector<SearchEngine> &Plugin::engines() const
{ return searchEngines_; }

void Plugin::setEngines(vector<SearchEngine> engines)
{
    sort(begin(engines), end(engines),
         [](auto a, auto b){ return a.name < b.name; });

    searchEngines_ = ::move(engines);

    QFile f(QDir(configLocation()).filePath(ENGINES_FILE_NAME));
    if (f.open(QIODevice::WriteOnly))
        f.write(serializeEngines(searchEngines_));
    else
        CRIT << u"Could not write to file: '%1' %2."_s.arg(f.fileName(), f.errorString());

    emit enginesChanged(searchEngines_);
}

void Plugin::restoreDefaultEngines()
{
    vector<SearchEngine> searchEngines;
    QFile f(u':' + ENGINES_FILE_NAME);
    if (f.open(QIODevice::ReadOnly))
    {
        const auto a = QJsonDocument::fromJson(f.readAll()).array();
        for (const auto &v : a)
        {
            QJsonObject o = v.toObject();
            SearchEngine e;
            e.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
            e.name = o[CK_ENGINE_NAME].toString();
            e.trigger = o[CK_ENGINE_TRIGGER].toString();
            e.icon_path = o[CK_ENGINE_ICON].toString();
            e.url = o[CK_ENGINE_URL].toString();
            e.fallback = o[CK_ENGINE_FALLBACK].toBool(false);
            searchEngines.push_back(e);
        }
    }
    else
        CRIT << "Failed reading default engines.";
    setEngines(searchEngines);
}

static shared_ptr<StandardItem> buildItem(const SearchEngine &se, const QString &search_term)
{
    QString url = QString(se.url).replace(u"%s"_s, percentEncoded(search_term));

    return StandardItem::make(
        se.id,
        se.name,
        Plugin::tr("Search %1 for '%2'").arg(se.name, search_term),
        [p=se.icon_path]{ return Icon::image(p); },
        {{u"run"_s, Plugin::tr("Run websearch"), [url]{ openUrl(url); }}},
        u"%1 %2"_s.arg(se.trigger, search_term)
    );
}

vector<RankItem> Plugin::rankItems(QueryContext &ctx)
{
    vector<RankItem> results;

    for (const SearchEngine &e: searchEngines_)
    {
        vector<QString> S{ e.trigger, e.name };

        // sort shortest first (yield higher scores) (*)
        sort(S.begin(), S.end(), [](auto &a, auto &b){ return a.length() < b.length(); });

        for (const auto &s : S)
        {
            auto keyword = u"%1 "_s.arg(s.toLower());
            auto prefix = ctx.query().toLower().left(keyword.size());
            Matcher matcher(prefix, {});
            Match m = matcher.match(keyword);
            if (m)
            {
                results.emplace_back(buildItem(e, ctx.query().mid(prefix.size())), m);
                // max one of these icons, assumption: following cant yield higher scores (*)
                break;
            }
        }
    }


    return results;
}

vector<shared_ptr<Item>> Plugin::fallbacks(const QString &query) const
{
    vector<shared_ptr<Item>> results;
    if (!query.isEmpty())
        for (const SearchEngine &e: searchEngines_)
            if (e.fallback)
                results.emplace_back(buildItem(e, query.isEmpty() ? u"…"_s : query));
    return results;
}

QWidget *Plugin::buildConfigWidget()
{ return new ConfigWidget(this); }

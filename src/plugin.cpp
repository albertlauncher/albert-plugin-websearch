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
using namespace std::filesystem;
using namespace std;

namespace {

static const auto *ENGINES_FILE_NAME  = "engines.json";
static const auto &CK_ENGINE_ID       = u"id"_s;
static const auto &CK_ENGINE_NAME     = u"name"_s;
static const auto &CK_ENGINE_URL      = u"url"_s;
static const auto &CK_ENGINE_TRIGGER  = u"trigger"_s;
static const auto &CK_ENGINE_ICON     = u"iconPath"_s;
static const auto &CK_ENGINE_FALLBACK = u"fallback"_s;

static QString createUuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces).left(8); }

static vector<SearchEngine> defaultEngines()
{
    vector<SearchEngine> search_engines;

    if (QFile f(u":%1"_s.arg(ENGINES_FILE_NAME));
        f.open(QIODevice::ReadOnly))
        for (const auto &v : QJsonDocument::fromJson(f.readAll()).array())
        {
            QJsonObject o = v.toObject();
            SearchEngine e;
            e.id = createUuid();
            e.name = o[CK_ENGINE_NAME].toString();
            e.trigger = o[CK_ENGINE_TRIGGER].toString();
            e.icon_path = o[CK_ENGINE_ICON].toString();
            e.url = o[CK_ENGINE_URL].toString();
            e.fallback = o[CK_ENGINE_FALLBACK].toBool(false);
            search_engines.push_back(e);
        }
    else
        CRIT << "Failed reading default engines.";

    ranges::sort(search_engines, less(), &SearchEngine::name);

    return search_engines;
}

}

Plugin::Plugin()
{
    create_directories(dataLocation());
    create_directories(configLocation());

    if (QFile f(configLocation() / ENGINES_FILE_NAME);
        f.open(QIODevice::ReadOnly))
        search_engines_ = deserializeEngines(f.readAll());
    else
        search_engines_ = defaultEngines();
}

const vector<SearchEngine> &Plugin::engines() const { return search_engines_; }

void Plugin::setEngines(vector<SearchEngine> engines)
{
    sort(begin(engines), end(engines),
         [](auto a, auto b){ return a.name < b.name; });

    search_engines_ = ::move(engines);

    if (QFile f(configLocation() / ENGINES_FILE_NAME);
        f.open(QIODevice::WriteOnly))
        f.write(serializeEngines(search_engines_));
    else
        CRIT << u"Could not write to file: '%1' %2."_s.arg(f.fileName(), f.errorString());

    emit enginesChanged(search_engines_);
}

void Plugin::restoreDefaultEngines() { setEngines(defaultEngines()); }

QByteArray Plugin::serializeEngines(const vector<SearchEngine> &engines)
{
    QJsonArray a;
    for (const SearchEngine& e : engines)
    {
        QJsonObject o;
        o[CK_ENGINE_ID] = e.id;
        o[CK_ENGINE_NAME] = e.name;
        o[CK_ENGINE_URL] = e.url;
        o[CK_ENGINE_TRIGGER] = e.trigger;
        if (e.icon_path.startsWith(u':'))
            o[CK_ENGINE_ICON] = e.icon_path;
        else
            o[CK_ENGINE_ICON] = toQString(relative(e.icon_path.toStdString(), dataLocation()));
        o[CK_ENGINE_FALLBACK] = e.fallback;
        a.append(o);
    }
    return QJsonDocument(a).toJson();
}

vector<SearchEngine> Plugin::deserializeEngines(const QByteArray &json)
{
    vector<SearchEngine> search_engines;
    for (const auto &v : QJsonDocument::fromJson(json).array())
    {
        QJsonObject o = v.toObject();
        SearchEngine e;

        e.id = o[CK_ENGINE_ID].toString(createUuid());

        e.name = o[CK_ENGINE_NAME].toString();

        e.trigger = o[CK_ENGINE_TRIGGER].toString().trimmed();

        if (auto icon_path = o[CK_ENGINE_ICON].toString();
            icon_path.startsWith(u':'))
            e.icon_path = icon_path;

        // Porting code. TODO: (2026-08-08) remove in future.
        else if (path fs_icon_path = icon_path.toStdString();
                 fs_icon_path.is_absolute())
            e.icon_path = icon_path;

        else
            e.icon_path = toQString(dataLocation() / icon_path.toStdString());

        e.url = o[CK_ENGINE_URL].toString();

        e.fallback = o[CK_ENGINE_FALLBACK].toBool();

        search_engines.push_back(e);
    }

    ranges::sort(search_engines, less(), &SearchEngine::name);

    return search_engines;
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

    for (const SearchEngine &e: search_engines_)
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
        for (const SearchEngine &e: search_engines_)
            if (e.fallback)
                results.emplace_back(buildItem(e, query.isEmpty() ? u"…"_s : query));
    return results;
}

QWidget *Plugin::buildConfigWidget() { return new ConfigWidget(this); }

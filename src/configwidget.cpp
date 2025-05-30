// Copyright (c) 2023 Manuel Schneider

#include "configwidget.h"
#include "plugin.h"
#include "searchengineeditor.h"
#include <QAbstractTableModel>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QMimeData>
#include <QSortFilterProxyModel>
#include <QUuid>
#include <albert/logging.h>
enum class Section{ Name, Trigger, Fallback, URL} ;
static const int sectionCount = 4;
using namespace Qt::StringLiterals;
using namespace std;

class EnginesModel final : public QAbstractTableModel
{
    Plugin *plugin_;
    mutable std::map<QString,QIcon> iconCache;

public:
    EnginesModel(Plugin *plugin, QObject *parent):
        QAbstractTableModel(parent),
        plugin_(plugin)
    {
        connect(plugin, &Plugin::enginesChanged, this, [this](){
            beginResetModel();
            iconCache.clear();
            endResetModel();
        });
    }

    int rowCount(const QModelIndex&) const override
    { return static_cast<int>(plugin_->engines().size()); }

    int columnCount(const QModelIndex&) const override
    { return sectionCount; }

    Qt::ItemFlags flags(const QModelIndex & index) const override
    {
        auto f =  Qt::ItemIsSelectable | Qt::ItemIsEnabled;
        switch ((Section)index.column()) {
        case Section::Fallback: return f | Qt::ItemIsUserCheckable;
        case Section::Trigger: return f | Qt::ItemIsEditable;
        default: return f;
        }
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
    {
        if (section < 0 || sectionCount <= section )
            return {};

        if (orientation == Qt::Horizontal)
            switch ((Section)section) {
            case Section::Name:
                switch (role) {
                case Qt::DisplayRole: return ConfigWidget::tr("Name");
                case Qt::ToolTipRole: return ConfigWidget::tr("Name of the search engine.");
                default: return {};
                }
            case Section::Trigger:
                switch (role) {
                case Qt::DisplayRole: return ConfigWidget::tr("Short");
                case Qt::ToolTipRole: return ConfigWidget::tr("Short name you can utilize for quick access.");
                default: return {};
                }
            case Section::URL:
                switch (role) {
                case Qt::DisplayRole: return ConfigWidget::tr("URL");
                case Qt::ToolTipRole: return ConfigWidget::tr("The URL of this search engine. %s will be replaced by your search term.");
                default: return {};
                }
            case Section::Fallback:
                switch (role) {
                case Qt::DisplayRole: return ConfigWidget::tr("F", "Short for Fallback");
                case Qt::ToolTipRole: return ConfigWidget::tr("Enable as fallback.");
                default: return {};
                }
            }
        return {};
    }

    QVariant data(const QModelIndex & index, int role = Qt::DisplayRole) const override
    {
        if ( !index.isValid() ||
            index.row() >= static_cast<int>(plugin_->engines().size()) ||
            index.column() >= sectionCount )
            return QVariant();

        const auto &se = plugin_->engines()[static_cast<ulong>(index.row())];

        switch (role) {
        case Qt::DisplayRole:
        case Qt::EditRole:
        {
            switch ((Section)index.column()) {
            case Section::Name:
                return se.name;
            case Section::Trigger:{
                auto trigger = se.trigger;
                return trigger.replace(u' ', u'•');
            }
            case Section::URL:
                return se.url;
            default: break;
            }
            break;
        }
        case Qt::DecorationRole:
        {
            if ((Section)index.column() == Section::Name) {
                // Resizing request thounsands of repaints. Creating an icon for
                // ever paint event is to expensive. Therefor maintain an icon cache
                auto icon_url = se.iconUrl;
                try {
                    return iconCache.at(icon_url);
                } catch (const out_of_range &) {
                    if (QUrl url(icon_url); url.isLocalFile())
                        return iconCache.emplace(icon_url, QIcon(url.toLocalFile())).first->second;
                    else
                        return iconCache.emplace(icon_url, QIcon(icon_url)).first->second;
                }
            }
            break;
        }
        case Qt::ToolTipRole:
            return ConfigWidget::tr("Double click to edit.");
        case Qt::CheckStateRole:
            if ((Section)index.column() == Section::Fallback)
                return se.fallback ? Qt::Checked : Qt::Unchecked;
        }
        return {};
    }

    bool setData(const QModelIndex &index, const QVariant &value, int role) override
    {
        if (!index.isValid())
            return false;

        switch ((Section)index.column())
        {
        case Section::Trigger:
            if (role == Qt::EditRole)
            {
                try {
                    auto engines = plugin_->engines();
                    auto &engine = engines[index.row()];
                    engine.trigger = value.toString();
                    plugin_->setEngines(engines);
                    return true;
                }
                catch (std::out_of_range &e){}
            }
            break;

        case Section::Fallback:
            if (role == Qt::CheckStateRole)
            {
                try {
                    auto engines = plugin_->engines();
                    auto &engine = engines[index.row()];
                    engine.fallback = value == Qt::Checked;
                    plugin_->setEngines(engines);
                    return true;
                }
                catch (std::out_of_range &e){}
            }
            break;

        default: return false;
        }
        return false;
    }
};


ConfigWidget::ConfigWidget(Plugin *plugin, QWidget *parent)
    : QWidget(parent), plugin_(plugin)
{
    ui.setupUi(this);

    ui.tableView_searches->setModel(new EnginesModel(plugin, ui.tableView_searches));
    ui.tableView_searches->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);  // requires a model!
    ui.tableView_searches->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);  // requires a model!
    ui.tableView_searches->horizontalHeader()->setStretchLastSection(true);

    connect(ui.pushButton_new, &QPushButton::clicked,
            this, &ConfigWidget::onButton_new);

    connect(ui.pushButton_remove, &QPushButton::clicked,
            this, &ConfigWidget::onButton_remove);

    connect(ui.pushButton_restoreDefaults, &QPushButton::clicked,
            this, &ConfigWidget::onButton_restoreDefaults);

    connect(ui.tableView_searches, &QTableView::activated,
            this, &ConfigWidget::onActivated);

}

static void handleAcceptedEditor(const SearchEngineEditor &editor, SearchEngine &engine, const Plugin &plugin)
{
    if (editor.icon_image){  // If icon changed copy the file

        // If there has been a user icon remove it
        if (QUrl url(engine.iconUrl); url.isLocalFile())
            QFile::moveToTrash(url.toLocalFile());

        auto image = editor.icon_image->scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);

        auto dst = QDir(plugin.dataLocation()).filePath(engine.id) + u".png"_s;
        if (!image.save(dst)){
            auto msg = ConfigWidget::tr("Could not save image to '%1'.").arg(dst);
            WARN << msg;
            QMessageBox::warning(nullptr, qApp->applicationDisplayName(), msg);
            return;
        }

        // set url
        engine.iconUrl = u"file:"_s + dst;
    }

    engine.name = editor.name();
    engine.trigger = editor.trigger();
    engine.url = editor.url();
    engine.fallback = editor.fallback();
}

void ConfigWidget::onActivated(QModelIndex index)
{
    if ((Section)index.column() == Section::Trigger)
    {
        ui.tableView_searches->edit(index);
        return;
    }

    auto engines = plugin_->engines();
    auto &engine = engines[index.row()];

    SearchEngineEditor editor(engine.iconUrl,
                              engine.name,
                              engine.trigger,
                              engine.url,
                              engine.fallback,
                              this);

    if (editor.exec()){
        handleAcceptedEditor(editor, engine, *plugin_);
        plugin_->setEngines(engines);
    }
}

void ConfigWidget::onButton_new()
{
    if (SearchEngineEditor editor(u":default"_s, {}, {}, {}, false, this); editor.exec()){
        SearchEngine engine;
        engine.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        engine.iconUrl = u":default"_s;
        handleAcceptedEditor(editor, engine, *plugin_);
        auto engines = plugin_->engines();
        engines.emplace_back(engine);
        plugin_->setEngines(engines);
    }
}

void ConfigWidget::onButton_remove()
{
    auto index = ui.tableView_searches->currentIndex();
    if (!index.isValid())
        return;

    auto reply = QMessageBox::question(
        this, qApp->applicationDisplayName(),
        tr("Do you really want to remove '%1' from the search engines?")
            .arg(plugin_->engines()[index.row()].name),
        QMessageBox::Yes|QMessageBox::No);
    if (reply == QMessageBox::Yes){
        auto engines = plugin_->engines();
        auto &engine = engines[index.row()];

        if (QUrl url(engine.iconUrl); url.isLocalFile())
            QFile::moveToTrash(url.toLocalFile());

        engines.erase(engines.begin() + index.row());
        plugin_->setEngines(engines);
    }
}

void ConfigWidget::onButton_restoreDefaults()
{
    auto reply = QMessageBox::question(
        this, qApp->applicationDisplayName(),
        tr("Do you really want to restore the default search engines?"),
        QMessageBox::Yes|QMessageBox::No);
    if (reply == QMessageBox::Yes)
        plugin_->restoreDefaultEngines();
}

#include "TemplateStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

using namespace movix::core;

namespace {

// Nombre de archivo a partir del nombre visible (sin caracteres que Windows no admite).
QString fileNameFor(const QString& name)
{
    QString out;
    for (const QChar c : name.trimmed())
        out += QStringLiteral("<>:\"/\\|?*").contains(c) || c.unicode() < 32 ? QLatin1Char('_') : c;
    return out;
}

} // namespace

TemplateStore::TemplateStore(const QString& root)
    : m_root(!root.isEmpty()                               ? root
             : qEnvironmentVariableIsSet("MOVIX_TEMPLATES_DIR") ? qEnvironmentVariable("MOVIX_TEMPLATES_DIR")
                                                                : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                                                      + QStringLiteral("/plantillas"))
{
    loadTitles();
}

// --- Títulos ---

void TemplateStore::loadTitles()
{
    m_titles.clear();
    QFile file(m_root + QStringLiteral("/titulos.json"));
    if (!file.open(QIODevice::ReadOnly))
        return;
    for (const QJsonValue& v : QJsonDocument::fromJson(file.readAll()).object()["titles"].toArray()) {
        const QJsonObject o = v.toObject();
        Title t;
        t.name = o["name"].toString();
        if (t.name.isEmpty())
            continue;
        t.style = titleFromJson(o["style"].toObject());
        t.x = o["x"].toDouble(0.0);
        t.y = o["y"].toDouble(0.0);
        t.scale = o["scale"].toDouble(1.0);
        t.rotation = o["rotation"].toDouble(0.0);
        t.opacity = o["opacity"].toDouble(1.0);
        t.seconds = std::clamp(o["seconds"].toDouble(5.0), 0.1, 3600.0);
        t.fadeIn = std::max(0.0, o["fadeIn"].toDouble());
        t.fadeOut = std::max(0.0, o["fadeOut"].toDouble());
        m_titles.push_back(std::move(t));
    }
}

bool TemplateStore::saveTitles(QString* error)
{
    QJsonArray list;
    for (const Title& t : m_titles)
        list.append(QJsonObject{{"name", t.name}, {"style", titleToJson(t.style)}, {"x", t.x}, {"y", t.y}, {"scale", t.scale},
                                {"rotation", t.rotation}, {"opacity", t.opacity}, {"seconds", t.seconds},
                                {"fadeIn", t.fadeIn}, {"fadeOut", t.fadeOut}});
    QDir().mkpath(m_root);
    QSaveFile file(m_root + QStringLiteral("/titulos.json"));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(QJsonObject{{"format", "movixstudio-title-templates"}, {"version", 1}, {"titles", list}}).toJson());
        if (file.commit())
            return true;
    }
    if (error)
        *error = file.errorString();
    return false;
}

bool TemplateStore::addTitle(const Title& title, QString* error)
{
    Title t = title;
    t.name = t.name.trimmed();
    if (t.name.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("TemplateStore", "La plantilla necesita un nombre.");
        return false;
    }
    auto it = std::find_if(m_titles.begin(), m_titles.end(), [&](const Title& x) { return x.name.compare(t.name, Qt::CaseInsensitive) == 0; });
    if (it != m_titles.end())
        *it = t;
    else
        m_titles.push_back(t);
    return saveTitles(error);
}

bool TemplateStore::removeTitle(int index)
{
    if (index < 0 || index >= int(m_titles.size()))
        return false;
    m_titles.erase(m_titles.begin() + index);
    return saveTitles(nullptr);
}

// --- Proyectos ---

QString TemplateStore::projectPath(const QString& name) const
{
    return m_root + QStringLiteral("/proyectos/") + fileNameFor(name) + QLatin1Char('.') + QLatin1String(kProjectExtension);
}

std::vector<TemplateStore::ProjectEntry> TemplateStore::projects() const
{
    std::vector<ProjectEntry> out;
    const QDir dir(m_root + QStringLiteral("/proyectos"));
    for (const QFileInfo& f : dir.entryInfoList({QStringLiteral("*.") + QLatin1String(kProjectExtension)}, QDir::Files, QDir::Name | QDir::IgnoreCase))
        out.push_back({f.completeBaseName(), f.absoluteFilePath()});
    return out;
}

bool TemplateStore::saveProject(const Project& project, const QString& name, QString* error)
{
    if (fileNameFor(name).isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("TemplateStore", "La plantilla necesita un nombre.");
        return false;
    }
    QDir().mkpath(m_root + QStringLiteral("/proyectos"));
    return saveProjectFile(project, projectPath(name), error);
}

bool TemplateStore::removeProject(const QString& name)
{
    return !fileNameFor(name).isEmpty() && QFile::remove(projectPath(name));
}

LoadResult TemplateStore::loadProject(const QString& name) const
{
    return loadProjectFile(projectPath(name));
}

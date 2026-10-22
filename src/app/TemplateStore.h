#pragma once

#include "core/ProjectFile.h"

#include <QString>

#include <vector>

// Plantillas propias del usuario: títulos (estilo, posición y duración) y proyectos completos
// (ajustes, pistas, títulos, marcadores y medios: por ejemplo, una cabecera con logotipo). Viven en
// la carpeta de datos de la aplicación: plantillas/titulos.json y plantillas/proyectos/*.mxproj.
class TemplateStore {
public:
    struct Title {
        QString name;
        movix::core::TitleStyle style;
        double x = 0.0, y = 0.0, scale = 1.0, rotation = 0.0, opacity = 1.0; // transformación fija
        double seconds = 5.0;
        double fadeIn = 0.0, fadeOut = 0.0; // segundos
    };
    struct ProjectEntry {
        QString name;
        QString path;
    };

    // `root` vacío: la variable MOVIX_TEMPLATES_DIR o, si no está, la carpeta de la aplicación.
    explicit TemplateStore(const QString& root = {});
    QString root() const { return m_root; }

    const std::vector<Title>& titles() const { return m_titles; }
    bool addTitle(const Title& title, QString* error = nullptr); // un nombre repetido la sustituye
    bool removeTitle(int index);

    std::vector<ProjectEntry> projects() const; // ordenadas por nombre
    bool saveProject(const movix::core::Project& project, const QString& name, QString* error = nullptr);
    bool removeProject(const QString& name);
    movix::core::LoadResult loadProject(const QString& name) const;

private:
    QString projectPath(const QString& name) const;
    void loadTitles();
    bool saveTitles(QString* error);

    QString m_root;
    std::vector<Title> m_titles;
};

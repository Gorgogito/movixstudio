#include "ProjectController.h"
#include "TemplateStore.h"
#include "core/Edits.h"

#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>

using namespace movix;
using namespace movix::core;

TEST_CASE("Almacén de plantillas: títulos y proyectos", "[templates]")
{
    QTemporaryDir dir;
    TemplateStore::Title t;
    t.name = "Rótulo de marca";
    t.style.text = "Movix";
    t.style.color = 0xFF00FF00;
    t.y = 0.3;
    t.seconds = 3.5;
    t.fadeIn = 0.5;
    {
        TemplateStore store(dir.path());
        CHECK(store.titles().empty());
        REQUIRE(store.addTitle(t));
        TemplateStore::Title other = t;
        other.name = "Otro";
        REQUIRE(store.addTitle(other));
        t.style.text = "Movix 2"; // mismo nombre (sin distinguir mayúsculas): se sustituye
        t.name = "rótulo de marca";
        REQUIRE(store.addTitle(t));
        CHECK(store.titles().size() == 2);
        CHECK_FALSE(store.addTitle(TemplateStore::Title{})); // sin nombre
    }
    TemplateStore store(dir.path()); // vuelve a leer del disco
    REQUIRE(store.titles().size() == 2);
    CHECK(store.titles()[0].style.text == "Movix 2");
    CHECK(store.titles()[0].style.color == 0xFF00FF00);
    CHECK(store.titles()[0].y == 0.3);
    CHECK(store.titles()[0].seconds == 3.5);
    CHECK(store.titles()[0].fadeIn == 0.5);
    REQUIRE(store.removeTitle(1));
    CHECK(TemplateStore(dir.path()).titles().size() == 1);

    Project p = Project::createDefault();
    p.settings.width = 1080;
    p.settings.height = 1920;
    REQUIRE(edit::addMarkers(p, {10}, "Gancho") == 1);
    REQUIRE(store.saveProject(p, "Vertical: redes/sociales")); // caracteres no válidos en un archivo
    REQUIRE(store.projects().size() == 1);
    CHECK(store.projects()[0].name == "Vertical_ redes_sociales");
    const auto loaded = store.loadProject("Vertical: redes/sociales");
    REQUIRE(loaded.project);
    CHECK(loaded.project->settings == p.settings);
    CHECK(loaded.project->markers == p.markers);
    CHECK_FALSE(store.saveProject(p, "  "));
    REQUIRE(store.removeProject("Vertical: redes/sociales"));
    CHECK(store.projects().empty());
}

TEST_CASE("Plantillas desde la aplicación", "[templates][app]")
{
    QTemporaryDir dir;
    qputenv("MOVIX_TEMPLATES_DIR", dir.path().toUtf8());
    {
        ProjectController project;
        project.newProject();
        const int builtin = int(project.titlePresets().size());
        REQUIRE(project.addTitle(1, -1, 0)); // rótulo inferior
        const Id title = Id(project.selection()[0].toLongLong());
        project.setTitleStyle(qint64(title), {{"text", "Ana Pérez"}, {"color", "#ff3366"}});
        project.setFades(qint64(title), 0.5, 1.0, false);
        REQUIRE(project.project().findClip(title)->fadeIn > 0);
        REQUIRE(project.saveTitleTemplate(qint64(title), "Entrevistada"));
        const QVariantList presets = project.titlePresets();
        REQUIRE(presets.size() == builtin + 1);
        CHECK(presets.last().toMap()["user"].toBool());
        CHECK(presets.last().toMap()["name"].toString() == "Entrevistada");

        // La plantilla reproduce el título: estilo, posición y duración.
        const Clip original = *project.project().findClip(title);
        REQUIRE(project.addTitle(builtin, -1, 300));
        const Clip& copy = *project.project().findClip(Id(project.selection()[0].toLongLong()));
        CHECK(copy.start == 300);
        CHECK(*copy.title == *original.title);
        CHECK(copy.transform.x.value == original.transform.x.value);
        CHECK(copy.transform.y.value == original.transform.y.value);
        CHECK(copy.duration == original.duration);
        CHECK(copy.fadeIn == original.fadeIn);
        CHECK(copy.fadeOut == original.fadeOut);

        // Plantilla de proyecto: un proyecto nuevo (sin archivo) con el mismo contenido.
        REQUIRE(project.saveProjectTemplate("Entrevistas"));
        CHECK(project.projectTemplates() == QStringList{"Entrevistas"});
        const auto videoBefore = project.project().videoTracks;
        project.newProject();
        CHECK(project.project().videoTracks[0].clips.empty());
        REQUIRE(project.newFromTemplate("Entrevistas"));
        CHECK(project.project().videoTracks == videoBefore);
        CHECK(project.fileUrl().isEmpty());
        CHECK(project.isModified());

        project.removeTitleTemplate(builtin);
        CHECK(project.titlePresets().size() == builtin);
        project.removeProjectTemplate("Entrevistas");
        CHECK(project.projectTemplates().isEmpty());
    }
    qunsetenv("MOVIX_TEMPLATES_DIR");
}

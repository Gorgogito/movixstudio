// MovixStudio — editor de video libre y de código abierto.
// Copyright (C) 2026 Colaboradores de MovixStudio
// SPDX-License-Identifier: GPL-3.0-or-later

#include "LanguageManager.h"

#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QUrl>

extern "C" {
#include <libavutil/log.h>
}

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("MovixStudio"));
    QGuiApplication::setApplicationName(QStringLiteral("MovixStudio"));
    QGuiApplication::setApplicationVersion(QStringLiteral(MOVIX_VERSION));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qt/qml/MovixStudio/icons/logo.svg")));
    QQuickStyle::setStyle(QStringLiteral("Fusion"));
    av_log_set_level(AV_LOG_ERROR);

    QQmlApplicationEngine engine;
    LanguageManager language(&engine);
    LanguageManager::setInstance(&language);
    language.loadSaved();

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("archivo"), QStringLiteral("Medio que se abrirá al iniciar."));
    parser.process(app);

    QVariantMap initial;
    if (!parser.positionalArguments().isEmpty())
        initial.insert(QStringLiteral("initialFile"), QUrl::fromLocalFile(QDir::current().absoluteFilePath(parser.positionalArguments().first())));
    engine.setInitialProperties(initial);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(-1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("MovixStudio", "Main");
    return QGuiApplication::exec();
}

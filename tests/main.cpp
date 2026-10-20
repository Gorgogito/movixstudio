#include <catch2/catch_session.hpp>

#include <QGuiApplication>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#include <crtdbg.h>
#include <cstdlib>
#endif

extern "C" {
#include <libavutil/log.h>
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
    timeBeginPeriod(1); // esperas de ~1 ms para muestrear el reproductor como un bucle de render real
#ifdef _DEBUG
    // Las aserciones de la CRT de depuración van a la consola en vez de abrir un diálogo
    // (un diálogo bloquea la ejecución desatendida de las pruebas).
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#endif
    // Aplicación gráfica sin ventanas: los títulos necesitan las fuentes del sistema (QPainter).
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    // Ajustes (QSettings) propios de las pruebas, separados de los de la aplicación.
    QGuiApplication::setOrganizationName(QStringLiteral("MovixStudio-pruebas"));
    QGuiApplication::setApplicationName(QStringLiteral("movix_tests"));
    av_log_set_level(AV_LOG_ERROR);
    const int result = Catch::Session().run(argc, argv);
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return result;
}

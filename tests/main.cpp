#include <catch2/catch_session.hpp>

#include <QCoreApplication>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

extern "C" {
#include <libavutil/log.h>
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
    timeBeginPeriod(1); // esperas de ~1 ms para muestrear el reproductor como un bucle de render real
#endif
    QCoreApplication app(argc, argv); // el reproductor usa temporizadores y señales de Qt
    av_log_set_level(AV_LOG_ERROR);
    const int result = Catch::Session().run(argc, argv);
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return result;
}

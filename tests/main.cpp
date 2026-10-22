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

#ifdef _WIN32
#include <cstdio>
#include <cstring>
static LONG CALLBACK traceCrash(EXCEPTION_POINTERS* e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT ctx = *e->ContextRecord;
    std::fprintf(stderr, "ACCESS VIOLATION (%s 0x%llx) hilo %lu\n", e->ExceptionRecord->ExceptionInformation[0] ? "escritura" : "lectura",
                 (unsigned long long)e->ExceptionRecord->ExceptionInformation[1], GetCurrentThreadId());
    for (int i = 0; i < 25 && ctx.Rip; ++i) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ctx.Rip, &mod))
            GetModuleFileNameA(mod, name, MAX_PATH);
        std::fprintf(stderr, "  #%d %s+0x%llx\n", i, std::strrchr(name, '\\') ? std::strrchr(name, '\\') + 1 : name,
                     (unsigned long long)(ctx.Rip - (DWORD64)mod));
        DWORD64 base = 0;
        auto* fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (!fn) {
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
            continue;
        }
        void* data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &data, &frame, nullptr);
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char* argv[])
{
#ifdef _WIN32
    // Ante un acceso inválido se imprime la pila (módulo+desplazamiento) antes de que Catch2 lo
    // informe: sin depurador, es lo que permite saber en qué biblioteca ocurrió.
    AddVectoredExceptionHandler(1, traceCrash);
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

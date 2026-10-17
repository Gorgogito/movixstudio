#include "LanguageManager.h"

#include <QCoreApplication>
#include <QQmlEngine>
#include <QSettings>

namespace {
const QString kSettingsKey = QStringLiteral("ui/language");
const QString kSourceLanguage = QStringLiteral("es");
} // namespace

LanguageManager::LanguageManager(QQmlEngine* engine, QObject* parent) : QObject(parent), m_engine(engine) {}

LanguageManager* LanguageManager::create(QQmlEngine*, QJSEngine*)
{
    Q_ASSERT(s_instance);
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

void LanguageManager::setInstance(LanguageManager* instance)
{
    s_instance = instance;
}

void LanguageManager::loadSaved()
{
    // Sin preferencia guardada se usa español, independientemente del idioma del sistema.
    const QString saved = QSettings().value(kSettingsKey, kSourceLanguage).toString();
    if (!apply(saved))
        apply(kSourceLanguage);
}

void LanguageManager::setCurrent(const QString& code)
{
    if (code == m_current || !apply(code))
        return;
    QSettings().setValue(kSettingsKey, m_current);
    m_engine->retranslate();
    emit currentChanged();
}

bool LanguageManager::apply(const QString& code)
{
    QCoreApplication::removeTranslator(&m_translator);
    if (code != kSourceLanguage) {
        if (!m_translator.load(QStringLiteral(":/i18n/movixstudio_%1.qm").arg(code))) {
            qWarning("Traducción no disponible: %s", qPrintable(code));
            m_current = kSourceLanguage;
            return false;
        }
        QCoreApplication::installTranslator(&m_translator);
    }
    m_current = code;
    return true;
}

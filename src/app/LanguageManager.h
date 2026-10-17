#pragma once

#include <QObject>
#include <QTranslator>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

// Idioma de la interfaz. El español es el idioma fuente y el predeterminado; el inglés se
// carga como traducción. El cambio es inmediato (retraduce toda la interfaz) y se recuerda.
class LanguageManager : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Language)
    QML_SINGLETON
    Q_PROPERTY(QString current READ current WRITE setCurrent NOTIFY currentChanged)

public:
    explicit LanguageManager(QQmlEngine* engine, QObject* parent = nullptr);

    static LanguageManager* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    static void setInstance(LanguageManager* instance);

    QString current() const { return m_current; }
    void setCurrent(const QString& code);
    void loadSaved();

signals:
    void currentChanged();

private:
    bool apply(const QString& code);

    static inline LanguageManager* s_instance = nullptr;
    QQmlEngine* m_engine;
    QTranslator m_translator;
    QString m_current = QStringLiteral("es");
};

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <vector>

// Atajos de teclado configurables: cada acción tiene sus atajos por defecto y, si el usuario los
// cambia, los suyos (se guardan en la configuración, grupo "atajos"). La interfaz enlaza los
// Shortcut de QML a `keys` y muestra el texto en los menús.
class ShortcutController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    // id → lista de atajos (texto portable, p. ej. "Ctrl+Shift+S") para Shortcut.sequences.
    Q_PROPERTY(QVariantMap keys READ keys NOTIFY changed)
    // [{id, label, group, shortcut (texto visible), isDefault}] en el orden de la lista.
    Q_PROPERTY(QVariantList actions READ actions NOTIFY changed)

public:
    explicit ShortcutController(QObject* parent = nullptr);

    QVariantMap keys() const;
    QVariantList actions() const;

    // Texto para un menú del primer atajo de la acción ("Ctrl+Mayús+S"), "" si no tiene.
    Q_INVOKABLE QString text(const QString& id) const;
    // Nombre de la acción que ya usa el atajo (otra que `id`), o "".
    Q_INVOKABLE QString conflict(const QString& id, const QString& sequence) const;
    // Sustituye los atajos de la acción por uno ("" = sin atajo). Si otra acción lo usaba, se lo quita.
    Q_INVOKABLE bool assign(const QString& id, const QString& sequence);
    Q_INVOKABLE void reset(const QString& id);
    Q_INVOKABLE void resetAll();
    // Atajo portable a partir de una tecla pulsada (Keys.onPressed); "" para las de solo modificador.
    Q_INVOKABLE QString sequenceFromKey(int key, int modifiers) const;
    // Texto visible de un atajo portable.
    Q_INVOKABLE QString display(const QString& sequence) const;

signals:
    void changed();

private:
    struct Action {
        QString id;
        QString label;
        QString group;
        QStringList defaults;
    };
    const Action* find(const QString& id) const;
    QStringList current(const Action& a) const;
    void store(const QString& id, const QStringList& sequences);

    std::vector<Action> m_actions;
    QVariantMap m_overrides; // id → QStringList del usuario
};

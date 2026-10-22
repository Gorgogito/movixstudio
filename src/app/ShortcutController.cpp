#include "ShortcutController.h"

#include <QKeySequence>
#include <QSettings>

ShortcutController::ShortcutController(QObject* parent) : QObject(parent)
{
    const QString file = tr("Archivo"), edit = tr("Edición"), play = tr("Reproducción"), view = tr("Vista");
    m_actions = {
        {"new", tr("Nuevo proyecto"), file, {"Ctrl+N"}},
        {"open", tr("Abrir proyecto"), file, {"Ctrl+O"}},
        {"save", tr("Guardar"), file, {"Ctrl+S"}},
        {"saveAs", tr("Guardar como"), file, {"Ctrl+Shift+S"}},
        {"import", tr("Importar medios"), file, {"Ctrl+I"}},
        {"export", tr("Exportar video"), file, {"Ctrl+E"}},
        {"undo", tr("Deshacer"), edit, {"Ctrl+Z"}},
        {"redo", tr("Rehacer"), edit, {"Ctrl+Y", "Ctrl+Shift+Z"}},
        {"split", tr("Dividir en el cabezal"), edit, {"Ctrl+B", "S"}},
        {"delete", tr("Eliminar"), edit, {"Del"}},
        {"rippleDelete", tr("Eliminar y cerrar hueco"), edit, {"Shift+Del"}},
        {"selectAll", tr("Seleccionar todo"), edit, {"Ctrl+A"}},
        {"deselect", tr("Quitar la selección"), edit, {"Esc"}},
        {"copy", tr("Copiar"), edit, {"Ctrl+C"}},
        {"cut", tr("Cortar"), edit, {"Ctrl+X"}},
        {"paste", tr("Pegar en el cabezal"), edit, {"Ctrl+V"}},
        {"title", tr("Añadir título"), edit, {"T"}},
        {"compound", tr("Crear clip compuesto"), edit, {"Ctrl+G"}},
        {"breakCompound", tr("Romper clip compuesto"), edit, {"Ctrl+Shift+G"}},
        {"marker", tr("Añadir marcador"), edit, {"Ctrl+M"}},
        {"nextMarker", tr("Marcador siguiente"), edit, {"Alt+Right"}},
        {"prevMarker", tr("Marcador anterior"), edit, {"Alt+Left"}},
        {"playPause", tr("Reproducir / pausa"), play, {"Space"}},
        {"pause", tr("Pausa"), play, {"K"}},
        {"play", tr("Reproducir"), play, {"L"}},
        {"stepBack", tr("Fotograma anterior"), play, {"Left"}},
        {"stepForward", tr("Fotograma siguiente"), play, {"Right"}},
        {"back1s", tr("Un segundo atrás"), play, {"Shift+Left"}},
        {"forward1s", tr("Un segundo adelante"), play, {"Shift+Right"}},
        {"home", tr("Ir al inicio"), play, {"Home"}},
        {"end", tr("Ir al final"), play, {"End"}},
        {"mute", tr("Silenciar"), play, {"M"}},
        {"zoomIn", tr("Acercar la línea de tiempo"), view, {"+", "="}},
        {"zoomOut", tr("Alejar la línea de tiempo"), view, {"-"}},
        {"fitAll", tr("Ajustar todo"), view, {"Shift+Z"}},
        {"snapping", tr("Imán"), view, {"N"}},
    };
    QSettings settings;
    settings.beginGroup(QStringLiteral("atajos"));
    for (const QString& id : settings.childKeys())
        if (find(id))
            m_overrides[id] = settings.value(id).toStringList();
}

const ShortcutController::Action* ShortcutController::find(const QString& id) const
{
    for (const Action& a : m_actions)
        if (a.id == id)
            return &a;
    return nullptr;
}

QStringList ShortcutController::current(const Action& a) const
{
    auto it = m_overrides.find(a.id);
    return it != m_overrides.end() ? it->toStringList() : a.defaults;
}

QVariantMap ShortcutController::keys() const
{
    QVariantMap out;
    for (const Action& a : m_actions)
        out[a.id] = current(a);
    return out;
}

QVariantList ShortcutController::actions() const
{
    QVariantList out;
    for (const Action& a : m_actions)
        out.append(QVariantMap{{"id", a.id},
                               {"label", a.label},
                               {"group", a.group},
                               {"shortcut", text(a.id)},
                               {"isDefault", !m_overrides.contains(a.id)}});
    return out;
}

QString ShortcutController::display(const QString& sequence) const
{
    if (sequence.isEmpty())
        return {};
    // Los nombres de las teclas como en el resto de la interfaz.
    QStringList parts = sequence.split(QLatin1Char('+'));
    if (sequence.endsWith(QLatin1String("++"))) { // la tecla "+"
        parts.removeLast();
        parts.last() = QStringLiteral("+");
    }
    for (QString& p : parts) {
        if (p == QLatin1String("Shift")) p = tr("Mayús");
        else if (p == QLatin1String("Del")) p = tr("Supr");
        else if (p == QLatin1String("Home")) p = tr("Inicio");
        else if (p == QLatin1String("End")) p = tr("Fin");
        else if (p == QLatin1String("Space")) p = tr("Espacio");
        else if (p == QLatin1String("Left")) p = QStringLiteral("←");
        else if (p == QLatin1String("Right")) p = QStringLiteral("→");
        else if (p == QLatin1String("Up")) p = QStringLiteral("↑");
        else if (p == QLatin1String("Down")) p = QStringLiteral("↓");
    }
    return parts.join(QLatin1Char('+'));
}

QString ShortcutController::text(const QString& id) const
{
    const Action* a = find(id);
    if (!a)
        return {};
    const QStringList list = current(*a);
    return list.isEmpty() ? QString() : display(list.first());
}

QString ShortcutController::conflict(const QString& id, const QString& sequence) const
{
    if (sequence.isEmpty())
        return {};
    const QKeySequence wanted(sequence, QKeySequence::PortableText);
    for (const Action& a : m_actions)
        if (a.id != id)
            for (const QString& s : current(a))
                if (QKeySequence(s, QKeySequence::PortableText) == wanted)
                    return a.label;
    return {};
}

void ShortcutController::store(const QString& id, const QStringList& sequences)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("atajos"));
    const Action* a = find(id);
    if (a && sequences == a->defaults) {
        m_overrides.remove(id);
        settings.remove(id);
    } else {
        m_overrides[id] = sequences;
        settings.setValue(id, sequences);
    }
}

bool ShortcutController::assign(const QString& id, const QString& sequence)
{
    if (!find(id))
        return false;
    // El atajo deja de servir para la acción que lo tuviera.
    if (!sequence.isEmpty()) {
        const QKeySequence wanted(sequence, QKeySequence::PortableText);
        for (const Action& a : m_actions) {
            if (a.id == id)
                continue;
            QStringList list = current(a);
            const qsizetype before = list.size();
            list.removeIf([&](const QString& s) { return QKeySequence(s, QKeySequence::PortableText) == wanted; });
            if (list.size() != before)
                store(a.id, list);
        }
    }
    store(id, sequence.isEmpty() ? QStringList{} : QStringList{sequence});
    emit changed();
    return true;
}

void ShortcutController::reset(const QString& id)
{
    if (const Action* a = find(id)) {
        store(id, a->defaults);
        emit changed();
    }
}

void ShortcutController::resetAll()
{
    QSettings settings;
    settings.remove(QStringLiteral("atajos"));
    m_overrides.clear();
    emit changed();
}

QString ShortcutController::sequenceFromKey(int key, int modifiers) const
{
    switch (key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
    case 0:
    case Qt::Key_unknown:
        return {};
    default:
        break;
    }
    const auto mods = Qt::KeyboardModifiers(modifiers) & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    return QKeySequence(QKeyCombination(mods, Qt::Key(key))).toString(QKeySequence::PortableText);
}

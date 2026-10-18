#pragma once

#include "core/Project.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace movix::core {

// Historial de deshacer/rehacer basado en instantáneas del proyecto.
//
// Cada edición se ejecuta sobre una copia; si cambia algo, se guarda el estado anterior. Así el
// deshacer es exacto por construcción (no depende de operaciones inversas). Las ediciones con la
// misma `mergeKey` consecutivas (p. ej. arrastrar un control de volumen) se agrupan en un paso.
class History {
public:
    explicit History(Project initial = Project::createDefault(), size_t limit = 300);

    const Project& project() const { return m_current; }

    // Aplica una edición. Devuelve true si el proyecto cambió (y entonces se registra).
    bool apply(const std::string& text, const std::function<bool(Project&)>& edit, const std::string& mergeKey = {});

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    std::string undoText() const { return canUndo() ? m_undo.back().text : std::string(); }
    std::string redoText() const { return canRedo() ? m_redo.back().text : std::string(); }
    bool undo();
    bool redo();

    // Sustituye el proyecto completo y vacía el historial (abrir / nuevo proyecto).
    void reset(Project project);

    // Revisión del estado actual: cambia con cada edición, deshacer o rehacer.
    std::uint64_t revision() const { return m_revision; }
    bool isClean() const { return m_revision == m_cleanRevision; }
    void markClean() { m_cleanRevision = m_revision; }
    void markDirty() { m_cleanRevision = 0; } // p. ej. un proyecto recuperado aún no guardado

    // Corta la agrupación en curso (p. ej. al soltar el ratón).
    void breakMerge() { m_mergeOpen = false; }

private:
    struct Entry {
        Project state;
        std::uint64_t revision;
        std::string text;
    };

    Project m_current;
    std::vector<Entry> m_undo;
    std::vector<Entry> m_redo;
    size_t m_limit;
    std::uint64_t m_revision = 1;
    std::uint64_t m_nextRevision = 2;
    std::uint64_t m_cleanRevision = 1;
    std::string m_lastMergeKey;
    bool m_mergeOpen = false;
};

} // namespace movix::core

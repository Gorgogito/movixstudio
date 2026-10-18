#include "core/History.h"

namespace movix::core {

History::History(Project initial, size_t limit) : m_current(std::move(initial)), m_limit(limit) {}

bool History::apply(const std::string& text, const std::function<bool(Project&)>& edit, const std::string& mergeKey)
{
    Project next = m_current;
    if (!edit(next) || next == m_current)
        return false;

    const bool merge = !mergeKey.empty() && m_mergeOpen && mergeKey == m_lastMergeKey && !m_undo.empty();
    if (!merge) {
        m_undo.push_back({std::move(m_current), m_revision, text});
        if (m_undo.size() > m_limit)
            m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
    m_current = std::move(next);
    m_revision = m_nextRevision++;
    m_lastMergeKey = mergeKey;
    m_mergeOpen = !mergeKey.empty();
    return true;
}

bool History::undo()
{
    if (m_undo.empty())
        return false;
    Entry e = std::move(m_undo.back());
    m_undo.pop_back();
    m_redo.push_back({std::move(m_current), m_revision, e.text});
    m_current = std::move(e.state);
    m_revision = e.revision;
    m_mergeOpen = false;
    return true;
}

bool History::redo()
{
    if (m_redo.empty())
        return false;
    Entry e = std::move(m_redo.back());
    m_redo.pop_back();
    m_undo.push_back({std::move(m_current), m_revision, e.text});
    m_current = std::move(e.state);
    m_revision = e.revision;
    m_mergeOpen = false;
    return true;
}

void History::reset(Project project)
{
    m_current = std::move(project);
    m_undo.clear();
    m_redo.clear();
    m_revision = m_nextRevision++;
    m_cleanRevision = m_revision;
    m_mergeOpen = false;
}

} // namespace movix::core

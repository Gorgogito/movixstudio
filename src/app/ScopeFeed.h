#pragma once

#include <QByteArray>
#include <QSize>

#include <atomic>
#include <cstdint>
#include <mutex>

// Copia reducida del lienzo del visor para los scopes. La escribe el hilo de render (lectura de la
// GPU) y la leen los ScopeView en el hilo de la interfaz.
class ScopeFeed {
public:
    void setActive(bool active) { m_active = active; }
    bool active() const { return m_active; }

    void publish(const QByteArray& rgba, QSize size)
    {
        std::lock_guard lock(m_mutex);
        m_rgba = rgba;
        m_size = size;
        ++m_seq;
    }
    uint64_t sequence() const
    {
        std::lock_guard lock(m_mutex);
        return m_seq;
    }
    // RGBA de 8 bits, filas consecutivas sin relleno.
    bool latest(QByteArray& rgba, QSize& size, uint64_t& seq) const
    {
        std::lock_guard lock(m_mutex);
        if (m_seq == 0)
            return false;
        rgba = m_rgba;
        size = m_size;
        seq = m_seq;
        return true;
    }

private:
    std::atomic<bool> m_active{false};
    mutable std::mutex m_mutex;
    QByteArray m_rgba;
    QSize m_size;
    uint64_t m_seq = 0;
};

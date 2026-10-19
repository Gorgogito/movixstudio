#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace movix::exporting {

// Cola acotada entre dos hilos de la cadena de exportación. push() espera si está llena y pop()
// si está vacía; close() despierta a ambos (pop devuelve nullopt cuando no queda nada).
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity) : m_capacity(capacity) {}

    bool push(T value)
    {
        std::unique_lock lock(m_mutex);
        m_notFull.wait(lock, [&] { return m_closed || m_items.size() < m_capacity; });
        if (m_closed)
            return false;
        m_items.push_back(std::move(value));
        m_notEmpty.notify_one();
        return true;
    }

    std::optional<T> pop()
    {
        std::unique_lock lock(m_mutex);
        m_notEmpty.wait(lock, [&] { return m_closed || !m_items.empty(); });
        if (m_items.empty())
            return std::nullopt;
        T value = std::move(m_items.front());
        m_items.pop_front();
        m_notFull.notify_one();
        return value;
    }

    // Sin esperar (para reciclar buffers): si está llena, el valor se descarta.
    void tryPush(T value)
    {
        std::lock_guard lock(m_mutex);
        if (!m_closed && m_items.size() < m_capacity) {
            m_items.push_back(std::move(value));
            m_notEmpty.notify_one();
        }
    }

    std::optional<T> tryPop()
    {
        std::lock_guard lock(m_mutex);
        if (m_items.empty())
            return std::nullopt;
        T value = std::move(m_items.front());
        m_items.pop_front();
        m_notFull.notify_one();
        return value;
    }

    void close()
    {
        std::lock_guard lock(m_mutex);
        m_closed = true;
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

private:
    size_t m_capacity;
    std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
    std::deque<T> m_items;
    bool m_closed = false;
};

} // namespace movix::exporting

#include "meguminlink/wireless/uart_egress.hpp"

namespace mlink
{
size_t UartEgress::space()
{
    portENTER_CRITICAL(&m_lock);
    const size_t free_bytes = CAPACITY - m_count;
    portEXIT_CRITICAL(&m_lock);
    return free_bytes;
}

size_t UartEgress::push(const u8 *data, size_t len)
{
    size_t written = 0;

    portENTER_CRITICAL(&m_lock);
    while (written < len && m_count < CAPACITY)
    {
        m_buffer[m_tail] = data[written];
        m_tail = (m_tail + 1) % CAPACITY;
        m_count++;
        written++;
    }
    portEXIT_CRITICAL(&m_lock);

    return written;
}

size_t UartEgress::peek(u8 *dst, size_t max)
{
    size_t n = 0;

    portENTER_CRITICAL(&m_lock);
    n = (m_count < max) ? m_count : max;
    for (size_t i = 0; i < n; i++)
    {
        dst[i] = m_buffer[(m_head + i) % CAPACITY];
    }
    portEXIT_CRITICAL(&m_lock);

    return n;
}

void UartEgress::consume(size_t len)
{
    portENTER_CRITICAL(&m_lock);
    const size_t n = (m_count < len) ? m_count : len;
    m_head = (m_head + n) % CAPACITY;
    m_count -= n;
    portEXIT_CRITICAL(&m_lock);
}

size_t UartEgress::size()
{
    portENTER_CRITICAL(&m_lock);
    const size_t count = m_count;
    portEXIT_CRITICAL(&m_lock);
    return count;
}

void UartEgress::flush()
{
    portENTER_CRITICAL(&m_lock);
    m_head = 0;
    m_tail = 0;
    m_count = 0;
    portEXIT_CRITICAL(&m_lock);
}
} // namespace mlink

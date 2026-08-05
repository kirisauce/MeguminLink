#include "meguminlink/binparser.hpp"

#include <cstring>
#include <endian.h>

// ── Single byte ──────────────────────────────────────────────

uint8_t BinParser::read_u8() {
    return *m_head++;
}

int8_t BinParser::read_i8() {
    return static_cast<int8_t>(*m_head++);
}

// ── Big-endian (network byte order) ──────────────────────────

uint16_t BinParser::read_u16() {
    uint16_t val;
    std::memcpy(&val, m_head, sizeof(val));
    m_head += sizeof(val);
    return be16toh(val);
}

int16_t BinParser::read_i16() {
    return static_cast<int16_t>(read_u16());
}

uint32_t BinParser::read_u32() {
    uint32_t val;
    std::memcpy(&val, m_head, sizeof(val));
    m_head += sizeof(val);
    return be32toh(val);
}

int32_t BinParser::read_i32() {
    return static_cast<int32_t>(read_u32());
}

uint64_t BinParser::read_u64() {
    uint64_t val;
    std::memcpy(&val, m_head, sizeof(val));
    m_head += sizeof(val);
    return be64toh(val);
}

int64_t BinParser::read_i64() {
    return static_cast<int64_t>(read_u64());
}

// ── Little-endian ────────────────────────────────────────────

uint16_t BinParser::read_u16_le() {
    uint16_t val;
    std::memcpy(&val, m_head, sizeof(val));
    m_head += sizeof(val);
    return le16toh(val);
}

int16_t BinParser::read_i16_le() {
    return static_cast<int16_t>(read_u16_le());
}

uint32_t BinParser::read_u32_le() {
    uint32_t val;
    std::memcpy(&val, m_head, sizeof(val));
    m_head += sizeof(val);
    return le32toh(val);
}

int32_t BinParser::read_i32_le() {
    return static_cast<int32_t>(read_u32_le());
}

uint64_t BinParser::read_u64_le() {
    uint64_t val;
    std::memcpy(&val, m_head, sizeof(val));
    m_head += sizeof(val);
    return le64toh(val);
}

int64_t BinParser::read_i64_le() {
    return static_cast<int64_t>(read_u64_le());
}

// ── Advance ──────────────────────────────────────────────────

void BinParser::consume(size_t n) {
    m_head += n;
}

// ── Accessors ────────────────────────────────────────────────

const uint8_t* BinParser::current() const {
    return m_head;
}

size_t BinParser::remaining() const {
    return static_cast<size_t>(m_end - m_head);
}

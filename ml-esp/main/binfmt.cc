#include "meguminlink/binfmt.hpp"

#include <cstring>
#include <endian.h>

// ── Single byte ──────────────────────────────────────────────

uint8_t BinDeserializer::read_u8() {
    return *m_head++;
}

int8_t BinDeserializer::read_i8() {
    return static_cast<int8_t>(*m_head++);
}

// ── Big-endian (network byte order) ──────────────────────────

uint16_t BinDeserializer::read_u16() {
    uint16_t val;
    std::memcpy(&val, m_head, sizeof(val));
    m_head += sizeof(val);
    return be16toh(val);
}

int16_t BinDeserializer::read_i16() {
    return static_cast<int16_t>(read_u16());
}

uint32_t BinDeserializer::read_u32() {
    uint32_t val;
    std::memcpy(&val, m_head, sizeof(val));
    m_head += sizeof(val);
    return be32toh(val);
}

int32_t BinDeserializer::read_i32() {
    return static_cast<int32_t>(read_u32());
}

uint64_t BinDeserializer::read_u64() {
    uint64_t val;
    std::memcpy(&val, m_head, sizeof(val));
    m_head += sizeof(val);
    return be64toh(val);
}

int64_t BinDeserializer::read_i64() {
    return static_cast<int64_t>(read_u64());
}

// ── Little-endian ────────────────────────────────────────────

uint16_t BinDeserializer::read_u16_le() {
    uint16_t val;
    std::memcpy(&val, m_head, sizeof(val));
    m_head += sizeof(val);
    return le16toh(val);
}

int16_t BinDeserializer::read_i16_le() {
    return static_cast<int16_t>(read_u16_le());
}

uint32_t BinDeserializer::read_u32_le() {
    uint32_t val;
    std::memcpy(&val, m_head, sizeof(val));
    m_head += sizeof(val);
    return le32toh(val);
}

int32_t BinDeserializer::read_i32_le() {
    return static_cast<int32_t>(read_u32_le());
}

uint64_t BinDeserializer::read_u64_le() {
    uint64_t val;
    std::memcpy(&val, m_head, sizeof(val));
    m_head += sizeof(val);
    return le64toh(val);
}

int64_t BinDeserializer::read_i64_le() {
    return static_cast<int64_t>(read_u64_le());
}

// ── Advance ──────────────────────────────────────────────────

void BinDeserializer::consume(size_t n) {
    m_head += n;
}

// ── Accessors ────────────────────────────────────────────────

const uint8_t* BinDeserializer::current() const {
    return m_head;
}

size_t BinDeserializer::remaining() const {
    return static_cast<size_t>(m_end - m_head);
}

// ══ BinSerializer ════════════════════════════════════════════

// ── Single byte ──────────────────────────────────────────────

void BinSerializer::write_u8(uint8_t val) {
    *m_head++ = val;
}

void BinSerializer::write_i8(int8_t val) {
    *m_head++ = static_cast<uint8_t>(val);
}

// ── Big-endian (network byte order) ──────────────────────────

void BinSerializer::write_u16(uint16_t val) {
    val = htobe16(val);
    std::memcpy(m_head, &val, sizeof(val));
    m_head += sizeof(val);
}

void BinSerializer::write_i16(int16_t val) {
    write_u16(static_cast<uint16_t>(val));
}

void BinSerializer::write_u32(uint32_t val) {
    val = htobe32(val);
    std::memcpy(m_head, &val, sizeof(val));
    m_head += sizeof(val);
}

void BinSerializer::write_i32(int32_t val) {
    write_u32(static_cast<uint32_t>(val));
}

void BinSerializer::write_u64(uint64_t val) {
    val = htobe64(val);
    std::memcpy(m_head, &val, sizeof(val));
    m_head += sizeof(val);
}

void BinSerializer::write_i64(int64_t val) {
    write_u64(static_cast<uint64_t>(val));
}

// ── Little-endian ────────────────────────────────────────────

void BinSerializer::write_u16_le(uint16_t val) {
    val = htole16(val);
    std::memcpy(m_head, &val, sizeof(val));
    m_head += sizeof(val);
}

void BinSerializer::write_i16_le(int16_t val) {
    write_u16_le(static_cast<uint16_t>(val));
}

void BinSerializer::write_u32_le(uint32_t val) {
    val = htole32(val);
    std::memcpy(m_head, &val, sizeof(val));
    m_head += sizeof(val);
}

void BinSerializer::write_i32_le(int32_t val) {
    write_u32_le(static_cast<uint32_t>(val));
}

void BinSerializer::write_u64_le(uint64_t val) {
    val = htole64(val);
    std::memcpy(m_head, &val, sizeof(val));
    m_head += sizeof(val);
}

void BinSerializer::write_i64_le(int64_t val) {
    write_u64_le(static_cast<uint64_t>(val));
}

// ── Raw bytes ────────────────────────────────────────────────

void BinSerializer::write_bytes(const uint8_t *data, size_t n) {
    std::memcpy(m_head, data, n);
    m_head += n;
}

// ── Accessors ────────────────────────────────────────────────

uint8_t* BinSerializer::current() const {
    return m_head;
}

size_t BinSerializer::size() const {
    return static_cast<size_t>(m_head - m_begin);
}

size_t BinSerializer::remaining() const {
    return static_cast<size_t>(m_end - m_head);
}

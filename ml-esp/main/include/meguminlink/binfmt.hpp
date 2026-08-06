#pragma once
#ifndef __MEGUMINLINK_BINFMT_HPP__
#define __MEGUMINLINK_BINFMT_HPP__

#include <cstddef>
#include <cstdint>

class BinDeserializer
{
  public:
    BinDeserializer(const uint8_t *begin, const uint8_t *end) : m_head(begin), m_end(end)
    {
    }

    BinDeserializer(const uint8_t *begin, size_t size) : m_head(begin), m_end(begin + size)
    {
    }

    /** 读取 1 字节，调用方必须保证 remaining() >= 1 */
    uint8_t read_u8();
    /** 读取 1 字节（有符号），调用方必须保证 remaining() >= 1 */
    int8_t read_i8();

    /** 读取大端 uint16，调用方必须保证 remaining() >= 2 */
    uint16_t read_u16();
    /** 读取大端 int16，调用方必须保证 remaining() >= 2 */
    int16_t read_i16();
    /** 读取大端 uint32，调用方必须保证 remaining() >= 4 */
    uint32_t read_u32();
    /** 读取大端 int32，调用方必须保证 remaining() >= 4 */
    int32_t read_i32();
    /** 读取大端 uint64，调用方必须保证 remaining() >= 8 */
    uint64_t read_u64();
    /** 读取大端 int64，调用方必须保证 remaining() >= 8 */
    int64_t read_i64();

    /** 读取小端 uint16，调用方必须保证 remaining() >= 2 */
    uint16_t read_u16_le();
    /** 读取小端 int16，调用方必须保证 remaining() >= 2 */
    int16_t read_i16_le();
    /** 读取小端 uint32，调用方必须保证 remaining() >= 4 */
    uint32_t read_u32_le();
    /** 读取小端 int32，调用方必须保证 remaining() >= 4 */
    int32_t read_i32_le();
    /** 读取小端 uint64，调用方必须保证 remaining() >= 8 */
    uint64_t read_u64_le();
    /** 读取小端 int64，调用方必须保证 remaining() >= 8 */
    int64_t read_i64_le();

    /** 跳过 n 字节，调用方必须保证 remaining() >= n */
    void consume(size_t n);

    /** 返回当前读取位置指针 */
    const uint8_t *current() const;
    /** 返回剩余未读取字节数 */
    size_t remaining() const;

  private:
    const uint8_t *m_head;
    const uint8_t *m_end;
};

class BinSerializer
{
  public:
    BinSerializer(uint8_t *begin, uint8_t *end) : m_head(begin), m_end(end), m_begin(begin)
    {
    }

    BinSerializer(uint8_t *begin, size_t capacity) : m_head(begin), m_end(begin + capacity), m_begin(begin)
    {
    }

    /** 写入 1 字节，调用方必须保证 remaining() >= 1 */
    void write_u8(uint8_t val);
    /** 写入 1 字节（有符号），调用方必须保证 remaining() >= 1 */
    void write_i8(int8_t val);

    /** 写入大端 uint16，调用方必须保证 remaining() >= 2 */
    void write_u16(uint16_t val);
    /** 写入大端 int16，调用方必须保证 remaining() >= 2 */
    void write_i16(int16_t val);
    /** 写入大端 uint32，调用方必须保证 remaining() >= 4 */
    void write_u32(uint32_t val);
    /** 写入大端 int32，调用方必须保证 remaining() >= 4 */
    void write_i32(int32_t val);
    /** 写入大端 uint64，调用方必须保证 remaining() >= 8 */
    void write_u64(uint64_t val);
    /** 写入大端 int64，调用方必须保证 remaining() >= 8 */
    void write_i64(int64_t val);

    /** 写入小端 uint16，调用方必须保证 remaining() >= 2 */
    void write_u16_le(uint16_t val);
    /** 写入小端 int16，调用方必须保证 remaining() >= 2 */
    void write_i16_le(int16_t val);
    /** 写入小端 uint32，调用方必须保证 remaining() >= 4 */
    void write_u32_le(uint32_t val);
    /** 写入小端 int32，调用方必须保证 remaining() >= 4 */
    void write_i32_le(int32_t val);
    /** 写入小端 uint64，调用方必须保证 remaining() >= 8 */
    void write_u64_le(uint64_t val);
    /** 写入小端 int64，调用方必须保证 remaining() >= 8 */
    void write_i64_le(int64_t val);

    /** 写入 n 字节原始数据，调用方必须保证 remaining() >= n */
    void write_bytes(const uint8_t *data, size_t n);

    /** 返回当前写入位置指针 */
    uint8_t *current() const;
    /** 返回已写入字节数 */
    size_t size() const;
    /** 返回剩余可用字节数 */
    size_t remaining() const;

  private:
    uint8_t *m_head;
    uint8_t *m_end;
    uint8_t *m_begin;
};

#endif /* __MEGUMINLINK_BINFMT_HPP__ */

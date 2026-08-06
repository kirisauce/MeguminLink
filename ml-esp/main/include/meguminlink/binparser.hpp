#pragma once
#ifndef __MEGUMINLINK_BINPARSER_HPP__
#define __MEGUMINLINK_BINPARSER_HPP__

#include <cstddef>
#include <cstdint>

class BinParser
{
  public:
    BinParser(const uint8_t *begin, const uint8_t *end) : m_head(begin), m_end(end)
    {
    }

    BinParser(const uint8_t *begin, size_t size) : m_head(begin), m_end(begin + size)
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

#endif /* __MEGUMINLINK_BINPARSER_HPP__ */

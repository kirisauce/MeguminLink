# MeguminLink 工作区约定

## C/C++ 代码风格

以下规则仅约束 C/C++ 代码；Python、Rust 等其他语言按各自项目的约定执行。

1. 协议常量一律 `inline constexpr`，不用宏；宏只用于头文件 guard。
2. 头文件使用 `#pragma once` + `#ifndef __<MODULE>_<NAME>_HPP__` guard。
3. 4 空格缩进、120 列、Allman 大括号、命名空间不缩进，格式以 `.clang-format` 为准。
4. 命名按 Rust 风格：类型/枚举/概念 UpperCamelCase；函数、变量、字段 snake_case；常量与编译期量 SCREAMING_SNAKE_CASE；命名空间 snake_case；成员变量用 `m_` 前缀。
5. 类型别名统一 `u8/u16/u32/u64`、`i8/i16/i32/i64`、`usize/isize`、`f32/f64`。
6. `#include` 行后不加任何注释，`// IWYU pragma: keep` 除外。
7. 注释只在含义不明显时写；结构体字段用 `/**< */`；不写解释设计动机的长注释。
8. 不写文件级运行时状态：状态收进持有它的对象；匿名 namespace 可放常量、类型与辅助函数，但不承载运行状态。

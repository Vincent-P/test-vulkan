#pragma once
#include <cstddef>
#include <cstdint>

using i8    = std::int8_t;
using i16   = std::int16_t;
using i32   = std::int32_t;
using i64   = std::int64_t;
using u8    = std::uint8_t;
using u16   = std::uint16_t;
using u32   = std::uint32_t;
using u64   = std::uint64_t;
using usize = std::size_t;
using uchar = unsigned char;
using uint  = unsigned int;

static constexpr u32 u32_invalid = ~0lu;
static constexpr u64 u64_invalid = ~0llu;

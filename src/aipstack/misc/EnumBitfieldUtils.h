/*
 * Copyright (c) 2017 Ambroz Bizjak
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef AIPSTACK_ENUM_BITFIELD_UTILS_H
#define AIPSTACK_ENUM_BITFIELD_UTILS_H

#include <type_traits>

#include <aipstack/misc/EnumUtils.h>

namespace AIpStack {

/**
 * @ingroup misc
 * @defgroup enum-bitfield Enum Bitfield Utilities
 * @brief Bitwise operations for enum types
 * 
 * The @ref AIPSTACK_ENUM_BITFIELD_OPS macro defines different operators for the specified
 * enum type. It is intended to be used with enum types that represent bitfields.
 * @{
 */

/**
 * Dummy type used with `==` and `!=` operators for checking if a bitfield enum is zero.
 * 
 * See @ref AIPSTACK_ENUM_BITFIELD_OPS. The intent is to use the @ref EnumZero constant not
 * this type directly.
 */
class EnumZeroType {};

/**
 * An @ref EnumZeroType value for convenience.
 */
constexpr EnumZeroType EnumZero = EnumZeroType();

/**
 * Define an unary operator for an enum type (use via @ref AIPSTACK_ENUM_BITFIELD_OPS).
 */
#define AIPSTACK_ENUM_UN_OP(EnumType, Op) \
inline constexpr EnumType operator Op (EnumType arg1) \
{ \
    return EnumType(Op AIpStack::ToUnderlyingType(arg1)); \
}

/**
 * Define a binary operator for an enum type (use via @ref AIPSTACK_ENUM_BITFIELD_OPS).
 */
#define AIPSTACK_ENUM_BIN_OP(EnumType, Op) \
inline constexpr EnumType operator Op (EnumType arg1, EnumType arg2) \
{ \
    return EnumType(AIpStack::ToUnderlyingType(arg1) Op AIpStack::ToUnderlyingType(arg2)); \
}

/**
 * Define a compound assignment operator for an enum type (use via
 * @ref AIPSTACK_ENUM_BITFIELD_OPS).
 */
#define AIPSTACK_ENUM_COMPOUND_OP(EnumType, Op) \
inline constexpr EnumType & operator Op##= (EnumType &arg1, EnumType arg2) \
{ \
    arg1 = EnumType(AIpStack::ToUnderlyingType(arg1) Op AIpStack::ToUnderlyingType(arg2)); \
    return arg1; \
}

/**
 * Define equality and inequality operators for an enum type with
 * @ref AIpStack::EnumZeroType "EnumZeroType" as the second operand.
 */
#define AIPSTACK_ENUM_ZERO_OPS(EnumType) \
inline constexpr bool operator== (EnumType arg1, AIpStack::EnumZeroType) \
{ \
    return AIpStack::ToUnderlyingType(arg1) == 0; \
} \
inline constexpr bool operator!= (EnumType arg1, AIpStack::EnumZeroType) \
{ \
    return AIpStack::ToUnderlyingType(arg1) != 0; \
}

/**
 * Macro which defines various operators intended for a bitfield-like enum type.
 * 
 * The `EnumType` must be an enum type (preferably enum class).
 * 
 * The operators `~`, `|`, `&`, `^`, `|=`, `&=`, `^=` will be defined to do the
 * corresponding bitwise operation on the underlying type.
 * 
 * Operators `==` and `!=` will be defined for @ref AIpStack::EnumZeroType "EnumZeroType"
 * as the second operand which check if the first operand (`EnumType`) is or is not zero
 * respectively. These are intended to be used with @ref AIpStack::EnumZero "EnumZero" as
 * follows: `e == EnumZero`, `e != EnumZero`.
 * 
 * @param EnumType Enum type to define operators for.
 */
#define AIPSTACK_ENUM_BITFIELD_OPS(EnumType) \
AIPSTACK_ENUM_UN_OP(EnumType, ~) \
AIPSTACK_ENUM_BIN_OP(EnumType, |) \
AIPSTACK_ENUM_BIN_OP(EnumType, &) \
AIPSTACK_ENUM_BIN_OP(EnumType, ^) \
AIPSTACK_ENUM_COMPOUND_OP(EnumType, |) \
AIPSTACK_ENUM_COMPOUND_OP(EnumType, &) \
AIPSTACK_ENUM_COMPOUND_OP(EnumType, ^) \
AIPSTACK_ENUM_ZERO_OPS(EnumType)

/** @} */

}

#endif

/*
 * Copyright (c) 2013 Ambroz Bizjak
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

#ifndef AIPSTACK_CHOOSE_INT_H
#define AIPSTACK_CHOOSE_INT_H

#include <cstdint>

#include <type_traits>

#include <aipstack/meta/BitsInInt.h>

namespace AIpStack {

/**
 * @addtogroup meta
 * @{
 */

#ifndef IN_DOXYGEN

template<int NumBits, bool Signed>
class ChooseIntHelper {
public:
    static_assert(NumBits > 0);
    static_assert((!Signed || NumBits < 64), "Too many bits (signed)");
    static_assert((!!Signed || NumBits <= 64), "Too many bits (unsigned).");
    
    using Result =
        std::conditional_t<(Signed && NumBits < 8), std::int8_t,
        std::conditional_t<(Signed && NumBits < 16), std::int16_t,
        std::conditional_t<(Signed && NumBits < 32), std::int32_t,
        std::conditional_t<(Signed && NumBits < 64), std::int64_t,
        std::conditional_t<(!Signed && NumBits <= 8), std::uint8_t,
        std::conditional_t<(!Signed && NumBits <= 16), std::uint16_t,
        std::conditional_t<(!Signed && NumBits <= 32), std::uint32_t,
        std::conditional_t<(!Signed && NumBits <= 64), std::uint64_t,
        void>>>>>>>>;
};

#endif

template<int NumBits, bool Signed = false>
using ChooseInt =
#ifdef IN_DOXYGEN
implementation_hidden;
#else
typename ChooseIntHelper<NumBits, Signed>::Result;
#endif

template<std::uintmax_t N, bool Signed = false>
using ChooseIntForMax = ChooseInt<BitsInInt<N>, Signed>;

/** @} */

}

#endif

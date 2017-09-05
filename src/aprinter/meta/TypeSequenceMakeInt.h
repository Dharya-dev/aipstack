/*
 * Copyright (c) 2014 Ambroz Bizjak
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

#ifndef AMBROLIB_TYPE_SEQUENCE_MAKE_INT_H
#define AMBROLIB_TYPE_SEQUENCE_MAKE_INT_H

#include <aprinter/meta/BasicMetaUtils.h>
#include <aprinter/meta/TypeSequence.h>

namespace APrinter {

template <typename, typename>
struct TypeSequenceMakeIntConcatHelper;

template <typename... Ints1, typename... Ints2>
struct TypeSequenceMakeIntConcatHelper<TypeSequence<Ints1...>, TypeSequence<Ints2...>> {
    using Result = TypeSequence<Ints1..., WrapInt<sizeof...(Ints1) + Ints2::Value>...>;
};

template <int N>
struct TypeSequenceMakeIntHelper {
    using Result = typename TypeSequenceMakeIntConcatHelper<
        typename TypeSequenceMakeIntHelper<(N / 2)>::Result,
        typename TypeSequenceMakeIntHelper<(N - (N / 2))>::Result
    >::Result;
};

template <>
struct TypeSequenceMakeIntHelper<0> {
    using Result = TypeSequence<>;
};

template <>
struct TypeSequenceMakeIntHelper<1> {
    using Result = TypeSequence<WrapInt<0>>;
};

template <int N>
using TypeSequenceMakeInt = typename TypeSequenceMakeIntHelper<N>::Result;

}

#endif

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

#ifndef AIPSTACK_INSTANTIATE_VARIADIC_H
#define AIPSTACK_INSTANTIATE_VARIADIC_H

#include <aipstack/meta/TypeSequence.h>
#include <aipstack/meta/TypeSequenceFromList.h>

namespace AIpStack {

/**
 * @addtogroup meta
 * @{
 */

#ifndef IN_DOXYGEN

template<template<typename ...> typename Template, typename Sequence>
struct InstantiateVariadicHelper;

template<template<typename ...> typename Template, typename ...Args>
struct InstantiateVariadicHelper<Template, TypeSequence<Args...>> {
    using Result = Template<Args...>;
};

#endif

template<template<typename ...> typename Template, typename List>
using InstantiateVariadic =
#ifdef IN_DOXYGEN
implementation_hidden;
#else
typename InstantiateVariadicHelper<
    Template, TypeSequenceFromList<List>>::Result;
#endif

}

/** @} */

#endif

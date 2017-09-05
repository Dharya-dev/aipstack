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

#ifndef AMBROLIB_ASSERT_H
#define AMBROLIB_ASSERT_H

#include <stdio.h>
#include <stdlib.h>
#ifdef AMBROLIB_AVR
#include <avr/pgmspace.h>
#endif

#include <aprinter/base/Preprocessor.h>
#include <aprinter/base/Hints.h>
#include <aprinter/base/ProgramMemory.h>

#define AMBRO_ASSERT_ABORT(msg) \
    do { \
        APrinter_AssertAbort(AMBRO_PSTR(msg)); \
    } while (0)

#define AMBRO_ASSERT_FORCE(e) \
    { \
        if (!(e)) AMBRO_ASSERT_ABORT("BUG " __FILE__ ":" AMBRO_STRINGIFY(__LINE__)); \
    }

#define AMBRO_ASSERT_FORCE_MSG(e, msg) \
    { \
        if (!(e)) AMBRO_ASSERT_ABORT(msg " at " __FILE__ ":" AMBRO_STRINGIFY(__LINE__)); \
    }

#ifdef AMBROLIB_ASSERTIONS
#define AMBRO_ASSERT(e) AMBRO_ASSERT_FORCE(e)
#else
#define AMBRO_ASSERT(e) {}
#endif

#ifdef __cplusplus
extern "C"
#endif
APRINTER_NO_INLINE APRINTER_NO_RETURN
inline void APrinter_AssertAbort (char const *msg)
{
#ifdef AMBROLIB_EMERGENCY_ACTION
    AMBROLIB_EMERGENCY_ACTION
#endif
    
#if !defined(AMBROLIB_NO_PRINT)
#ifdef AMBROLIB_AVR
    puts_P(msg);
#else
    puts(msg);
#endif
#endif
    
#ifdef AMBROLIB_ABORT_ACTION
    AMBROLIB_ABORT_ACTION
#else
    abort();
#endif
}

#endif

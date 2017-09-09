/*
 * Copyright (c) 2016 Ambroz Bizjak
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

#ifndef AIPSTACK_IP4_PROTO_H
#define AIPSTACK_IP4_PROTO_H

#include <stdint.h>
#include <stddef.h>

#include <aipstack/common/Struct.h>
#include <aipstack/proto/IpAddr.h>

namespace AIpStack {

AIPSTACK_DEFINE_STRUCT(Ip4Header,
    (VersionIhlDscpEcn, uint16_t)
    (TotalLen,          uint16_t)
    (Ident,             uint16_t)
    (FlagsOffset,       uint16_t)
    (TtlProto,          uint16_t)
    (HeaderChksum,      uint16_t)
    (SrcAddr,           Ip4Addr)
    (DstAddr,           Ip4Addr)
)

static int const Ip4VersionShift = 4;
static uint8_t const Ip4IhlMask = 0xF;

static uint16_t const Ip4FlagDF = (uint16_t)1 << 14;
static uint16_t const Ip4FlagMF = (uint16_t)1 << 13;

static uint16_t const Ip4OffsetMask = UINT16_C(0x1fff);

static size_t const Ip4MaxHeaderSize = 60;

static uint8_t const Ip4ProtocolIcmp = 1;
static uint8_t const Ip4ProtocolTcp  = 6;
static uint8_t const Ip4ProtocolUdp  = 17;

// The full datagram size which every internet destination must be
// be able to receive either in one piece or in fragments (RFC 791 page 25).
static uint16_t const Ip4RequiredRecvSize = 576;

static uint16_t Ip4RoundFragLen (uint8_t header_length, uint16_t mtu)
{
    return header_length + (((mtu - header_length) / 8) * 8);
}

}

#endif

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

#ifndef AIPSTACK_IP_TCP_PROTO_CONSTANTS_H
#define AIPSTACK_IP_TCP_PROTO_CONSTANTS_H

#include <stdint.h>

#include <aipstack/meta/BitsInInt.h>
#include <aipstack/misc/Use.h>
#include <aipstack/misc/MinMax.h>
#include <aipstack/proto/Tcp4Proto.h>
#include <aipstack/ip/IpStack.h>
#include <aipstack/tcp/TcpUtils.h>

namespace AIpStack {

#ifndef IN_DOXYGEN
template <typename> class IpTcpProto;
#endif

template <typename Arg>
class IpTcpProto_constants
{
    AIPSTACK_USE_TYPES(TcpUtils, (SeqType))
    AIPSTACK_USE_TYPES(IpTcpProto<Arg>, (TimeType, RttType, Platform, TheIpStack))
    AIPSTACK_USE_VALS(IpTcpProto<Arg>, (RttTimeFreq, RttTypeMaxDbl))
    
    // Make sure the MinMTU permits an unfragmented TCP segment with some data.
    static_assert(TheIpStack::MinMTU >= Ip4TcpHeaderSize + 32, "");
    
public:    
    // Don't allow the remote host to lower the MSS beyond this.
    // NOTE: pcb_calc_snd_mss_from_pmtu relies on this definition.
    static uint16_t const MinAllowedMss = TheIpStack::MinMTU - Ip4TcpHeaderSize;
    
    // Common flags passed to IpStack::sendIp4Dgram.
    // We disable fragmentation of TCP segments sent by us, due to PMTUD.
    static IpSendFlags const TcpIpSendFlags = IpSendFlags::DontFragmentFlag;
    
public:
    // Maximum theoreticaly possible send and receive window.
    static SeqType const MaxWindow = UINT32_C(0x3fffffff);
    
    // Default window update threshold (overridable by setWindowUpdateThreshold).
    static SeqType const DefaultWndAnnThreshold = 2700;
    
    // How old at most an ACK may be to be considered acceptable (MAX.SND.WND in RFC 5961).
    static SeqType const MaxAckBefore = UINT32_C(0xFFFF);
    
    // SYN_RCVD state timeout.
    static TimeType const SynRcvdTimeoutTicks     = 20.0  * Platform::TimeFreq;
    
    // SYN_SENT state timeout.
    static TimeType const SynSentTimeoutTicks     = 30.0  * Platform::TimeFreq;
    
    // TIME_WAIT state timeout.
    static TimeType const TimeWaitTimeTicks       = 120.0 * Platform::TimeFreq;
    
    // Timeout to abort connection after it has been abandoned.
    static TimeType const AbandonedTimeoutTicks   = 30.0  * Platform::TimeFreq;
    
    // Time after the send buffer is extended to calling pcb_output.
    static TimeType const OutputTimerTicks        = 0.0005 * Platform::TimeFreq;
    
    // Time to retry after sending failed with error IpErr::BUFFER_FULL.
    static TimeType const OutputRetryFullTicks    = 0.1 * Platform::TimeFreq;
    
    // Time to retry after sending failed with error other then IpErr::BUFFER_FULL.
    static TimeType const OutputRetryOtherTicks   = 2.0 * Platform::TimeFreq;
    
    // Initial retransmission time, before any round-trip-time measurement.
    static RttType const InitialRtxTime           = 1.0 * RttTimeFreq;
    
    // Minimum retransmission time.
    static RttType const MinRtxTime               = 0.25 * RttTimeFreq;
    
    // Maximum retransmission time (need care not to overflow RttType).
    static RttType const MaxRtxTime = MinValue(RttTypeMaxDbl, 60. * RttTimeFreq);
    
    // Number of duplicate ACKs to trigger fast retransmit/recovery.
    static uint8_t const FastRtxDupAcks = 3;
    
    // Maximum number of additional duplicate ACKs that will result in CWND increase.
    static uint8_t const MaxAdditionaDupAcks = 32;
    
    // Window scale shift count to send and use in outgoing ACKs.
    static uint8_t const RcvWndShift = 6;
    static_assert(RcvWndShift <= 14, "");
    
    // Minimum amount to extend the receive window when a PCB is
    // abandoned before the FIN has been received.
    static SeqType const MinAbandonRcvWndIncr = TypeMax<uint16_t>();
    
public:
    static int const DupAckBits =
        BitsInInt<FastRtxDupAcks + MaxAdditionaDupAcks>::Value;
};

}

#endif

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

#ifndef AIPSTACK_IP_TCP_PROTO_OUTPUT_H
#define AIPSTACK_IP_TCP_PROTO_OUTPUT_H

#include <stdint.h>
#include <stddef.h>

#include <limits>

#include <aipstack/misc/Use.h>
#include <aipstack/misc/Assert.h>
#include <aipstack/misc/Hints.h>
#include <aipstack/misc/MinMax.h>
#include <aipstack/misc/OneOf.h>
#include <aipstack/infra/Buf.h>
#include <aipstack/infra/Chksum.h>
#include <aipstack/infra/TxAllocHelper.h>
#include <aipstack/infra/Err.h>
#include <aipstack/proto/Tcp4Proto.h>
#include <aipstack/ip/IpStack.h>
#include <aipstack/tcp/TcpUtils.h>

namespace AIpStack {

template <typename TcpProto>
class IpTcpProto_output
{
    AIPSTACK_USE_TYPES(TcpUtils, (FlagsType, SeqType, PortType, TcpState, TcpSegMeta,
                                   OptionFlags, TcpOptions))
    AIPSTACK_USE_VALS(TcpUtils, (seq_add, seq_diff, seq_lt2, seq_add_sat, tcplen,
                                 can_output_in_state, snd_open_in_state))
    AIPSTACK_USE_TYPES(TcpProto, (Ip4RxInfo, TcpPcb, PcbFlags, Input, TimeType, RttType,
                                   RttNextType, Constants, OutputTimer, RtxTimer,
                                   TheIpStack, MtuRef, Connection, PcbKey))
    AIPSTACK_USE_VALS(TcpProto, (RttTypeMax))
    AIPSTACK_USE_VALS(TheIpStack, (HeaderBeforeIp4Dgram))
    
public:
    // Check if our FIN has been ACKed.
    static bool pcb_fin_acked (TcpPcb *pcb)
    {
        return pcb->hasFlag(PcbFlags::FIN_SENT) && pcb->snd_una == pcb->snd_nxt;
    }
    
    // Send SYN or SYN-ACK packet (in the SYN_SENT or SYN_RCVD states respectively).
    AIPSTACK_NO_INLINE
    static void pcb_send_syn (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(pcb->state == OneOf(TcpState::SYN_SENT, TcpState::SYN_RCVD))
        
        // Include the MSS option.
        TcpOptions tcp_opts;
        tcp_opts.options = OptionFlags::MSS;
        // The iface_mss is stored in a variable otherwise unused in this state.
        tcp_opts.mss = (pcb->state == TcpState::SYN_SENT) ?
            pcb->base_snd_mss : pcb->snd_mss;
        
        // Send the window scale option if needed.
        if (pcb->hasFlag(PcbFlags::WND_SCALE)) {
            tcp_opts.options |= OptionFlags::WND_SCALE;
            tcp_opts.wnd_scale = pcb->rcv_wnd_shift;
        }
        
        // The SYN and SYN-ACK must always have non-scaled window size.
        // For justification of assert see see create_connection, listen_input.
        AIPSTACK_ASSERT(pcb->rcv_ann_wnd <= std::numeric_limits<uint16_t>::max())
        uint16_t window_size = pcb->rcv_ann_wnd;
        
        // Send SYN or SYN-ACK flags depending on the state.
        FlagsType flags = Tcp4FlagSyn |
            ((pcb->state == TcpState::SYN_RCVD) ? Tcp4FlagAck : 0);
        
        // Send the segment.
        IpErr err = send_tcp_nodata(pcb->tcp, *pcb, pcb->snd_una, pcb->rcv_nxt,
                                    window_size, flags, &tcp_opts, pcb);
        
        if (err == IpErr::SUCCESS) {
            // Have we sent the SYN for the first time?
            if (pcb->snd_nxt == pcb->snd_una) {
                // Start a round-trip-time measurement.
                pcb_start_rtt_measurement(pcb, true);
                
                // Bump snd_nxt.
                pcb->snd_nxt = seq_add(pcb->snd_nxt, 1);
            } else {
                // Retransmission, stop any round-trip-time measurement.
                pcb->clearFlag(PcbFlags::RTT_PENDING);
            }
        }
    }
    
    // Send an empty ACK (which may be a window update).
    AIPSTACK_NO_INLINE
    static void pcb_send_empty_ack (TcpPcb *pcb)
    {
        // Get the window size value.
        uint16_t window_size = Input::pcb_ann_wnd(pcb);
        
        // Send it.
        send_tcp_nodata(pcb->tcp, *pcb, pcb->snd_nxt, pcb->rcv_nxt, window_size,
                        Tcp4FlagAck, nullptr, pcb);
    }
    
    // Send an RST for this PCB.
    static void pcb_send_rst (TcpPcb *pcb)
    {
        bool ack = pcb->state != TcpState::SYN_SENT;
        
        send_rst(pcb->tcp, *pcb, pcb->snd_nxt, ack, pcb->rcv_nxt);
    }
    
    static void pcb_need_ack (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(pcb->state != TcpState::CLOSED)
        
        // If we're in input processing just set a flag that ACK is
        // needed which will be picked up at the end, otherwise send
        // an ACK ourselves.
        if (pcb->inInputProcessing()) {
            pcb->setFlag(PcbFlags::ACK_PENDING);
        } else {
            pcb_send_empty_ack(pcb);
        }
    }
    
    static void pcb_snd_buf_extended (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(pcb->state == TcpState::SYN_SENT || snd_open_in_state(pcb->state))
        AIPSTACK_ASSERT(pcb->state == TcpState::SYN_SENT || pcb_has_snd_outstanding(pcb))
        
        if (AIPSTACK_LIKELY(pcb->state != TcpState::SYN_SENT)) {
            // Set the output timer.
            pcb_set_output_timer_for_output(pcb);
            
            // Delayed timer update is needed by pcb_set_output_timer_for_output.
            pcb->doDelayedTimerUpdateIfNeeded();
        }
    }
    
    static void pcb_end_sending (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(snd_open_in_state(pcb->state))
        // If sending was closed without abandoning the connection, the push
        // index must have been set to the end of the send buffer.
        AIPSTACK_ASSERT(pcb->con == nullptr ||
                     pcb->con->m_v.snd_psh_index == pcb->con->m_v.snd_buf.tot_len)
        
        // Make the appropriate state transition.
        if (pcb->state == TcpState::ESTABLISHED) {
            pcb->state = TcpState::FIN_WAIT_1;
        } else {
            AIPSTACK_ASSERT(pcb->state == TcpState::CLOSE_WAIT)
            pcb->state = TcpState::LAST_ACK;
        }
        
        // Queue a FIN for sending.
        pcb->setFlag(PcbFlags::FIN_PENDING);
        
        // Push output.
        pcb_push_output(pcb);
    }
    
    static void pcb_push_output (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(can_output_in_state(pcb->state))
        AIPSTACK_ASSERT(pcb_has_snd_outstanding(pcb))
        
        // Schedule a call to pcb_output soon.
        if (pcb->inInputProcessing()) {
            pcb->setFlag(PcbFlags::OUT_PENDING);
        } else {
            // Schedule the output timer to call pcb_output.
            pcb_set_output_timer_for_output(pcb);
            
            // Delayed timer update is needed by pcb_set_output_timer_for_output.
            pcb->doDelayedTimerUpdateIfNeeded();
        }
    }
    
    // Check if there is any unacknowledged or unsent data or FIN.
    static bool pcb_has_snd_outstanding (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(can_output_in_state(pcb->state))
        
        // If sending was close, FIN is outstanding.
        if (AIPSTACK_UNLIKELY(!snd_open_in_state(pcb->state))) {
            return true;
        }
        
        // PCB must still have a Connection, if not sending would
        // have been closed not open.
        Connection *con = pcb->con;
        AIPSTACK_ASSERT(con != nullptr)
        
        // Check whether there is any data in the send buffer.
        return con->m_v.snd_buf.tot_len > 0;
    }
    
    // Determine if there is any data or FIN which is no longer queued for
    // sending but has not been ACKed. This is NOT necessarily the same as
    // snd_una!=snd_nxt due to requeuing in pcb_rtx_timer_handler.
    static bool pcb_has_snd_unacked (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(can_output_in_state(pcb->state))
        
        Connection *con = pcb->con;
        return
            (AIPSTACK_LIKELY(con != nullptr) &&
                con->m_v.snd_buf_cur.tot_len < con->m_v.snd_buf.tot_len) ||
            (!snd_open_in_state(pcb->state) && !pcb->hasFlag(PcbFlags::FIN_PENDING));
    }
    
    /**
     * With rtx_or_window_probe==false, transmits queued data as permissible
     * and controls the rtx_timer.
     * NOTE: doDelayedTimerUpdate must be called after return.
     * 
     * With rtx_or_window_probe==true, sends one segment from the start of
     * the send buffer, does nothing else and always returns true. It does
     * not change the queue position (snd_buf_cur and FIN_PENDING). In this
     * case it only respects snd_wnd not cwnd, and forces sending of at least
     * one sequence count.
     */
    AIPSTACK_NO_INLINE
    static void pcb_output_active (TcpPcb *pcb, bool rtx_or_window_probe)
    {
        AIPSTACK_ASSERT(can_output_in_state(pcb->state))
        AIPSTACK_ASSERT(pcb_has_snd_outstanding(pcb))
        AIPSTACK_ASSERT(pcb->con != nullptr)
        
        Connection *con = pcb->con;
        
        IpBufRef *snd_buf_cur;
        SeqType rem_wnd;
        size_t data_threshold;
        bool fin;
        
        if (AIPSTACK_UNLIKELY(rtx_or_window_probe)) {
            // Send from the start of the start buffer. We take care to not
            // modify the real snd_buf via the snd_buf_cur pointer.
            snd_buf_cur = &con->m_v.snd_buf;
            
            // Send no more than allowed by the receiver window but at
            // least one count. We can ignore the congestion window.
            rem_wnd = MaxValue((SeqType)1, con->m_v.snd_wnd);
            
            // Set the data_threshold to zero to not inhibit sending.
            data_threshold = 0;
            
            // Allow sending a FIN if sending was closed.
            fin = !snd_open_in_state(pcb->state);
            
            // Note that in this case the send loop condition will always
            // be true, pcb_output_segment will be called once and then this
            // function will return.
        } else {
            AIPSTACK_ASSERT(con->m_v.cwnd >= pcb->snd_mss)
            AIPSTACK_ASSERT(con->m_v.snd_buf_cur.tot_len <= con->m_v.snd_buf.tot_len)
            AIPSTACK_ASSERT(con->m_v.snd_psh_index <= con->m_v.snd_buf.tot_len)
            
            // Use and update real snd_buf_cur.
            snd_buf_cur = &con->m_v.snd_buf_cur;
            
            // Calculate the miniumum of snd_wnd and cwnd which is how much
            // we can send relative to the start of the send buffer.
            SeqType full_wnd = MinValue(con->m_v.snd_wnd, con->m_v.cwnd);
            
            // Calculate the remaining window relative to snd_buf_cur.
            size_t snd_offset = con->m_v.snd_buf.tot_len - snd_buf_cur->tot_len;
            if (AIPSTACK_LIKELY(snd_offset <= full_wnd)) {
                rem_wnd = full_wnd - snd_offset;
            } else {
                rem_wnd = 0;
            }
            
            // Calculate the threshold length for the remaining unsent data above
            // which sending will not be delayed. This calculation achieves that
            // delay is only allowed if we have less than snd_mss data left and none
            // of this is being pushed via snd_psh_index.
            size_t psh_to_end = con->m_v.snd_buf.tot_len - con->m_v.snd_psh_index;
            data_threshold = MinValue(psh_to_end, (size_t)(pcb->snd_mss - 1));
            
            // Allow sending a FIN if it is queued.
            fin = pcb->hasFlag(PcbFlags::FIN_PENDING);
        }
        
        // Create the output helper (which optimizes sending multiple segments at a time).
        PcbOutputHelper output_helper;
        
        // Send segments while we have some non-delayable data or FIN
        // queued, and there is some window availabe. But for the case
        // of rtx_or_window_probe, this condition is always true.
        while ((snd_buf_cur->tot_len > data_threshold || fin) && rem_wnd > 0) {
            // Send a segment.
            SeqType seg_seqlen;
            IpErr err = pcb_output_segment(
                pcb, output_helper, *snd_buf_cur, fin, rem_wnd, &seg_seqlen);
            
            // If we got the FRAG_NEEDED error, make sure the Path MTU estimate
            // does not exceed the interface MTU, to handle lowering of the
            // interface MTU. We don't retry sending immediately, this is a very
            // rare event anyway.
            if (AIPSTACK_UNLIKELY(err == IpErr::FRAG_NEEDED)) {
                pcb->tcp->m_stack->handleLocalPacketTooBig(pcb->remote_addr);
            }
            
            // If this was for retransmission or window probe, don't do anything
            // else than sending.
            if (AIPSTACK_UNLIKELY(rtx_or_window_probe)) {
                return;
            }
            
            // If there was an error sending the segment, stop for now and retry later.
            if (AIPSTACK_UNLIKELY(err != IpErr::SUCCESS)) {
                pcb_set_output_timer_for_retry(pcb, err);
                break;
            }
            
            // If we were successful we must have sent something and not more
            // than the window allowed or more than we had to send.
            AIPSTACK_ASSERT(seg_seqlen > 0)
            AIPSTACK_ASSERT(seg_seqlen <= rem_wnd)
            AIPSTACK_ASSERT(seg_seqlen <= snd_buf_cur->tot_len + fin)
            
            // Check sent sequence length to see if a FIN was sent.
            size_t data_sent;
            if (AIPSTACK_UNLIKELY(seg_seqlen > snd_buf_cur->tot_len)) {
                // FIN was sent, we must still have FIN_PENDING.
                AIPSTACK_ASSERT(pcb->hasFlag(PcbFlags::FIN_PENDING))
                
                // All remaining data was sent.
                data_sent = snd_buf_cur->tot_len;
                
                // Clear the FIN_PENDING flag.
                pcb->clearFlag(PcbFlags::FIN_PENDING);
                
                // Clear the local fin flag to let the loop stop.
                fin = false;
            } else {
                // Only data was sent.
                data_sent = seg_seqlen;
            }
            
            // Advance snd_buf_cur over any data just sent.
            if (AIPSTACK_LIKELY(data_sent > 0)) {
                snd_buf_cur->skipBytes(data_sent);
            }
            
            // Decrement remaining window.
            rem_wnd -= seg_seqlen;
            
            // Clear ACK_PENDING flag to avoid sending an empty ACK needlessly.
            pcb->clearFlag(PcbFlags::ACK_PENDING);
        }
        
        // If the IDLE_TIMER flag is set, clear it and ensure that the RtxTimer
        // is set. This way the code below for setting the timer does not need
        // to concern itself with the idle timeout, and performance is improved
        // for sending with no idle timeouts in between.
        if (AIPSTACK_UNLIKELY(pcb->hasFlag(PcbFlags::IDLE_TIMER))) {
            pcb->clearFlag(PcbFlags::IDLE_TIMER);
            pcb->tim(RtxTimer()).unset();
        }
        
        // If the retransmission timer is already running then leave it.
        // Otherwise start it if we have sent and unacknowledged data or
        // if we have zero window (to send windor probe). Note that for
        // zero window it would not be wrong to have an extra condition
        // !pcb_may_delay_snd but we don't for simplicity.
        if (!pcb->tim(RtxTimer()).isSet()) {
            if (AIPSTACK_LIKELY(pcb_has_snd_unacked(pcb)) || pcb->con->m_v.snd_wnd == 0) {
                pcb->tim(RtxTimer()).setAfter(pcb_rto_time(pcb));
            }
        }
    }
    
    /**
     * This is the equivalent of pcb_output_active for abandoned PCBs.
     * NOTE: doDelayedTimerUpdate must be called after return.
     */
    AIPSTACK_NO_INLINE
    static void pcb_output_abandoned (TcpPcb *pcb, bool rtx_or_window_probe)
    {
        AIPSTACK_ASSERT(can_output_in_state(pcb->state))
        AIPSTACK_ASSERT(pcb->con == nullptr)
        // below are implied by con == nullptr, see also pcb_abandoned
        AIPSTACK_ASSERT(!snd_open_in_state(pcb->state))
        AIPSTACK_ASSERT(!pcb->hasFlag(PcbFlags::IDLE_TIMER))
        
        // Send a FIN if rtx_or_window_probe or otherwise if FIN is queued.
        bool fin = rtx_or_window_probe ? true : pcb->hasFlag(PcbFlags::FIN_PENDING);
        
        // Send a FIN if it is queued.
        if (fin) do {
            // Send a FIN segment.
            uint16_t window_size = Input::pcb_ann_wnd(pcb);
            FlagsType flags = Tcp4FlagAck|Tcp4FlagFin|Tcp4FlagPsh;
            IpErr err = send_tcp_nodata(pcb->tcp, *pcb, pcb->snd_una, pcb->rcv_nxt,
                                        window_size, flags, nullptr, pcb);
            
            // On success take note of what was sent.
            if (AIPSTACK_LIKELY(err == IpErr::SUCCESS)) {
                // Set the FIN_SENT flag.
                pcb->setFlag(PcbFlags::FIN_SENT);
                
                // Bump snd_nxt if needed.
                if (pcb->snd_nxt == pcb->snd_una) {
                    pcb->snd_nxt++;
                }
            }
            
            // If this was for retransmission or window probe, don't do anything else.
            if (AIPSTACK_UNLIKELY(rtx_or_window_probe)) {
                return;
            }
            
            // If there was an error sending the segment, stop for now and retry later.
            if (AIPSTACK_UNLIKELY(err != IpErr::SUCCESS)) {
                pcb_set_output_timer_for_retry(pcb, err);
                break;
            }
            
            // Clear the FIN_PENDING flag.
            pcb->clearFlag(PcbFlags::FIN_PENDING);
            
            // Clear ACK_PENDING flag to avoid sending an empty ACK needlessly.
            pcb->clearFlag(PcbFlags::ACK_PENDING);
        } while (false);
        
        // Set the retransmission timer as needed. This is really the same as
        // in pcb_output_active, the logic just reduces to this.
        if (!pcb->tim(RtxTimer()).isSet()) {
            if (AIPSTACK_LIKELY(!pcb->hasFlag(PcbFlags::FIN_PENDING))) {
                pcb->tim(RtxTimer()).setAfter(pcb_rto_time(pcb));
            }
        }
    }
    
    /**
     * Calls pcb_output_active or pcb_output_abandoned as appropriate.
     * NOTE: doDelayedTimerUpdate must be called after return.
     */
    inline static void pcb_output (TcpPcb *pcb, bool rtx_or_window_probe)
    {
        AIPSTACK_ASSERT(can_output_in_state(pcb->state))
        AIPSTACK_ASSERT(pcb_has_snd_outstanding(pcb))
        
        if (AIPSTACK_LIKELY(pcb->con != nullptr)) {
            pcb_output_active(pcb, rtx_or_window_probe);
        } else {
            pcb_output_abandoned(pcb, rtx_or_window_probe);
        }
    }
    
    // OutputTimer handler. Sends any queued data/FIN as permissible.
    inline static void pcb_output_timer_handler (TcpPcb *pcb)
    {
        // Output using pcb_output.
        pcb_output(pcb, false);
        
        // Delayed timer update is needed by timer expiration and pcb_output.
        pcb->doDelayedTimerUpdate();
    }
    
    inline static void pcb_rtx_timer_handler (TcpPcb *pcb)
    {
        // Handle retransmission or idle timeout.
        pcb_rtx_timer_handler_core(pcb);
        
        // Delayed timer update is needed by timer expiration and
        // pcb_rtx_timer_handler_core.
        pcb->doDelayedTimerUpdate();
    }
    
    static void pcb_rtx_timer_handler_core (TcpPcb *pcb)
    {
        // This timer is only for SYN_SENT, SYN_RCVD and can_output_in_state
        // states. In any change to another state the timer would be stopped.
        AIPSTACK_ASSERT(pcb->state == OneOf(TcpState::SYN_SENT, TcpState::SYN_RCVD) ||
                     can_output_in_state(pcb->state))
        
        // Is this an idle timeout?
        if (pcb->hasFlag(PcbFlags::IDLE_TIMER)) {
            // When the idle timer was set, !pcb_has_snd_outstanding held. However
            // for the expiration we have a relaxed precondition (implied by the former),
            // that is !pcb_has_snd_unacked and that the connection is not abandoned.
            
            // 1) !pcb_has_snd_unacked could only be invalidated by sending data/FIN:
            //    - pcb_output_active/pcb_output_abandoned which would stop the idle
            //      timeout when anything is sent.
            //    - pcb_rtx_timer_handler can obviously not send anything before here.
            //    - Fast-recovery related sending (pcb_fast_rtx_dup_acks_received,
            //      pcb_output_handle_acked) can only happen when pcb_has_snd_unacked.
            // 2) pcb->con != nullptr could only be invalidated when the connection is
            //    abandoned, and pcb_abandoned would stop the idle timeout.
            
            AIPSTACK_ASSERT(can_output_in_state(pcb->state))
            AIPSTACK_ASSERT(!pcb_has_snd_unacked(pcb))
            AIPSTACK_ASSERT(pcb->con != nullptr)
            
            // Clear the IDLE_TIMER flag. This is not strictly necessarily but is mostly
            // cosmetical and for a minor performance gain in pcb_output_active where it
            // avoids clearing this flag and redundantly stopping the timer.
            pcb->clearFlag(PcbFlags::IDLE_TIMER);
            
            Connection *con = pcb->con;
            
            // Reduce the CWND (RFC 5681 section 4.1).
            // Also reset cwnd_acked to avoid old accumulated value
            // from causing an undesired cwnd increase later.
            SeqType initial_cwnd = TcpUtils::calc_initial_cwnd(pcb->snd_mss);
            if (con->m_v.cwnd >= initial_cwnd) {
                con->m_v.cwnd = initial_cwnd;
                pcb->setFlag(PcbFlags::CWND_INIT);
            }
            con->m_v.cwnd_acked = 0;
            
            // This is all, the remainder of this function is for retransmission.
            return;
        }
        
        // Check if this is for SYN or SYN-ACK retransmission.
        bool syn_sent_rcvd = pcb->state == OneOf(TcpState::SYN_SENT, TcpState::SYN_RCVD);
        
        // We must have something outstanding to be sent or acked.
        // This was the case when we were sent and if that changed
        // the timer would have been unset.
        AIPSTACK_ASSERT(syn_sent_rcvd || pcb_has_snd_outstanding(pcb))
        
        // Check for spurious timer expiration after timer is no longer
        // needed (no unacked data and no zero window).
        if (!syn_sent_rcvd && !pcb_has_snd_unacked(pcb) &&
            (pcb->con == nullptr || pcb->con->m_v.snd_wnd != 0))
        {
            // Return without restarting the timer.
            return;
        }
        
        // Double the retransmission timeout and restart the timer.
        RttType doubled_rto = (pcb->rto > RttTypeMax / 2) ? RttTypeMax : (2 * pcb->rto);
        pcb->rto = MinValue(Constants::MaxRtxTime, doubled_rto);
        pcb->tim(RtxTimer()).setAfter(pcb_rto_time(pcb));
        
        // In SYN_SENT and SYN_RCVD, only retransmit the SYN or SYN-ACK.
        if (syn_sent_rcvd) {
            pcb_send_syn(pcb);
            return;
        }
        
        Connection *con = pcb->con;
        
        if (con == nullptr || con->m_v.snd_wnd == 0) {
            // This is for:
            // - FIN retransmission or window probe after connection was
            //   abandoned (we don't disinguish these two cases).
            // - Zero window probe while not abandoned.
            pcb_output(pcb, true);
        } else {
            // This is for data or FIN retransmission while not abandoned.
            
            // Check for first retransmission.
            if (!pcb->hasFlag(PcbFlags::RTX_ACTIVE)) {
                // Set flag to indicate there has been a retransmission.
                // This will be cleared upon new ACK.
                pcb->setFlag(PcbFlags::RTX_ACTIVE);
                
                // Update ssthresh (RFC 5681).
                pcb_update_ssthresh_for_rtx(pcb);
            }
            
            // Set cwnd to one segment (RFC 5681).
            // Also reset cwnd_acked to avoid old accumulated value
            // from causing an undesired cwnd increase later.
            con->m_v.cwnd = pcb->snd_mss;
            pcb->clearFlag(PcbFlags::CWND_INIT);
            con->m_v.cwnd_acked = 0;
            
            // Set recover.
            pcb->setFlag(PcbFlags::RECOVER);
            con->m_v.recover = pcb->snd_nxt;
            
            // Exit any fast recovery.
            pcb->num_dupack = 0;
            
            // Requeue all data and FIN.
            pcb_requeue_everything(pcb);
            
            // Retransmit using pcb_output_active.
            pcb_output_active(pcb, false);
            
            // NOTE: There may be a remote possibility that nothing was sent
            // by pcb_output_active, if snd_mss increased to allow delaying
            // sending (pcb_may_delay_snd). In that case the rtx_timer would
            // have been unset by pcb_output_active, but we still did all the
            // congestion related state changes above and that's fine.
        }
    }
    
    static void pcb_requeue_everything (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(can_output_in_state(pcb->state))
        
        // Requeue data.
        Connection *con = pcb->con;
        if (AIPSTACK_LIKELY(con != nullptr)) {
            con->m_v.snd_buf_cur = con->m_v.snd_buf;
        }
        
        // Requeue any FIN.
        if (!snd_open_in_state(pcb->state)) {
            pcb->setFlag(PcbFlags::FIN_PENDING);
        }
    }
    
    // This is called from Input when something new is acked, before the
    // related state changes are made (snd_una, snd_wnd, snd_buf*, state
    // transition due to FIN acked).
    static void pcb_output_handle_acked (TcpPcb *pcb, SeqType ack_num, SeqType acked)
    {
        AIPSTACK_ASSERT(can_output_in_state(pcb->state))
        AIPSTACK_ASSERT(pcb_has_snd_outstanding(pcb))
        
        // Clear the RTX_ACTIVE flag since any retransmission has now been acked.
        pcb->clearFlag(PcbFlags::RTX_ACTIVE);
        
        Connection *con = pcb->con;
        
        // Handle end of round-trip-time measurement.
        if (pcb->hasFlag(PcbFlags::RTT_PENDING)) {
            // If we have RTT_PENDING outside of SYN_SENT/SYN_RCVD we must
            // also have a Connection (see pcb_abandoned, pcb_start_rtt_measurement).
            AIPSTACK_ASSERT(con != nullptr)
            
            if (seq_lt2(con->m_v.rtt_test_seq, ack_num)) {
                // Update the RTT variables and RTO.
                pcb_end_rtt_measurement(pcb);
                
                // Allow more CWND increase in congestion avoidance.
                pcb->clearFlag(PcbFlags::CWND_INCRD);
            }
        }
        
        // Connection was abandoned?
        if (AIPSTACK_UNLIKELY(con == nullptr)) {
            // Reset the duplicate ACK counter.
            pcb->num_dupack = 0;
        }
        // Not in fast recovery?
        else if (AIPSTACK_LIKELY(pcb->num_dupack < Constants::FastRtxDupAcks)) {
            // Reset the duplicate ACK counter.
            pcb->num_dupack = 0;
            
            // Perform congestion-control processing.
            if (con->m_v.cwnd <= con->m_v.ssthresh) {
                // Slow start.
                pcb_increase_cwnd_acked(pcb, acked);
            } else {
                // Congestion avoidance.
                if (!pcb->hasFlag(PcbFlags::CWND_INCRD)) {
                    // Increment cwnd_acked.
                    con->m_v.cwnd_acked = seq_add_sat(con->m_v.cwnd_acked, acked);
                    
                    // If cwnd data has now been acked, increment cwnd and reset cwnd_acked,
                    // and inhibit such increments until the next RTT measurement completes.
                    if (AIPSTACK_UNLIKELY(con->m_v.cwnd_acked >= con->m_v.cwnd)) {
                        pcb_increase_cwnd_acked(pcb, con->m_v.cwnd_acked);
                        con->m_v.cwnd_acked = 0;
                        pcb->setFlag(PcbFlags::CWND_INCRD);
                    }
                }
            }
        }
        // In fast recovery
        else {
            // We had sent but unkacked data when fast recovery was started
            // and this must still be true. Because when all unkacked data is
            // ACKed we would exit fast recovery, just below (the condition
            // below is implied then because recover<=snd_nxt).
            AIPSTACK_ASSERT(pcb_has_snd_unacked(pcb))
            
            // If all data up to recover is being ACKed, exit fast recovery.
            if (!pcb->hasFlag(PcbFlags::RECOVER) || !seq_lt2(ack_num, con->m_v.recover)) {
                // Deflate the CWND.
                // Note, cwnd>=snd_mss is respected because ssthresh>=snd_mss.
                SeqType flight_size = seq_diff(pcb->snd_nxt, ack_num);
                AIPSTACK_ASSERT(con->m_v.ssthresh >= pcb->snd_mss)
                con->m_v.cwnd = MinValue(con->m_v.ssthresh,
                    seq_add(MaxValue(flight_size, (SeqType)pcb->snd_mss),
                            pcb->snd_mss));
                
                // Reset num_dupack to indicate end of fast recovery.
                pcb->num_dupack = 0;
            } else {
                // Retransmit the first unacknowledged segment.
                pcb_output_active(pcb, true);
                
                // Deflate CWND by the amount of data ACKed.
                // Be careful to not bring CWND below snd_mss.
                AIPSTACK_ASSERT(con->m_v.cwnd >= pcb->snd_mss)
                con->m_v.cwnd -=
                    MinValue(seq_diff(con->m_v.cwnd, pcb->snd_mss), acked);
                
                // If this ACK acknowledges at least snd_mss of data,
                // add back snd_mss bytes to CWND.
                if (acked >= pcb->snd_mss) {
                    con->m_v.cwnd = seq_add_sat(con->m_v.cwnd, pcb->snd_mss);
                }
            }
        }
        
        // If the snd_una increment that will be done for this ACK will
        // leave recover behind snd_una, clear the recover flag to indicate
        // that recover is no longer valid and assumed <snd_una.
        if (AIPSTACK_UNLIKELY(pcb->hasFlag(PcbFlags::RECOVER)) &&
            con != nullptr && seq_lt2(con->m_v.recover, ack_num))
        {
            pcb->clearFlag(PcbFlags::RECOVER);
        }
    }
    
    // Called from Input when the number of duplicate ACKs has
    // reached FastRtxDupAcks, the fast recovery threshold.
    static void pcb_fast_rtx_dup_acks_received (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(can_output_in_state(pcb->state))
        AIPSTACK_ASSERT(pcb_has_snd_unacked(pcb))
        AIPSTACK_ASSERT(pcb->num_dupack == Constants::FastRtxDupAcks)
        
        // If we have recover (>=snd_nxt), we must not enter fast recovery.
        // In that case we must decrement num_dupack by one, to indicate that
        // we are not in fast recovery and the next duplicate ACK is still
        // a candidate.
        if (pcb->hasFlag(PcbFlags::RECOVER)) {
            pcb->num_dupack--;
            return;
        }
        
        // Do the retransmission.
        pcb_output(pcb, true);
        
        Connection *con = pcb->con;
        if (AIPSTACK_LIKELY(con != nullptr)) {
            // Set recover.
            pcb->setFlag(PcbFlags::RECOVER);
            con->m_v.recover = pcb->snd_nxt;
            
            // Update ssthresh.
            pcb_update_ssthresh_for_rtx(pcb);
            
            // Update cwnd.
            SeqType three_mss = 3 * (SeqType)pcb->snd_mss;
            con->m_v.cwnd = seq_add_sat(con->m_v.ssthresh, three_mss);
            pcb->clearFlag(PcbFlags::CWND_INIT);
            
            // Schedule output due to possible CWND increase.
            pcb->setFlag(PcbFlags::OUT_PENDING);
        }
    }
    
    // Called from Input when an additional duplicate ACK has been
    // received while already in fast recovery.
    static void pcb_extra_dup_ack_received (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(can_output_in_state(pcb->state))
        AIPSTACK_ASSERT(pcb_has_snd_unacked(pcb))
        AIPSTACK_ASSERT(pcb->num_dupack > Constants::FastRtxDupAcks)
        
        if (AIPSTACK_LIKELY(pcb->con != nullptr)) {
            // Increment CWND by snd_mss.
            pcb->con->m_v.cwnd = seq_add_sat(pcb->con->m_v.cwnd, pcb->snd_mss);
            
            // Schedule output due to possible CWND increase.
            pcb->setFlag(PcbFlags::OUT_PENDING);
        }
    }
    
    static TimeType pcb_rto_time (TcpPcb *pcb)
    {
        return (TimeType)pcb->rto << TcpProto::RttShift;
    }
    
    static void pcb_end_rtt_measurement (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(pcb->hasFlag(PcbFlags::RTT_PENDING))
        AIPSTACK_ASSERT(pcb->con != nullptr)
        
        // Clear the flag to indicate end of RTT measurement.
        pcb->clearFlag(PcbFlags::RTT_PENDING);
        
        // Calculate how much time has passed, also in RTT units.
        TimeType time_diff = pcb->platform().getTime() - pcb->rtt_test_time;
        RttType this_rtt = MinValueU(RttTypeMax, time_diff >> TcpProto::RttShift);
        
        Connection *con = pcb->con;
        
        // Update RTTVAR and SRTT.
        if (!pcb->hasFlag(PcbFlags::RTT_VALID)) {
            pcb->setFlag(PcbFlags::RTT_VALID);
            con->m_v.rttvar = this_rtt/2;
            con->m_v.srtt = this_rtt;
        } else {
            RttType rtt_diff = AbsoluteDiff(con->m_v.srtt, this_rtt);
            con->m_v.rttvar = ((RttNextType)3 * con->m_v.rttvar + rtt_diff) / 4;
            con->m_v.srtt = ((RttNextType)7 * con->m_v.srtt + this_rtt) / 8;
        }
        
        // Update RTO.
        int const k = 4;
        RttType k_rttvar = (con->m_v.rttvar > RttTypeMax / k) ?
            RttTypeMax : (k * con->m_v.rttvar);
        RttType var_part = MaxValue((RttType)1, k_rttvar);
        RttType base_rto = (var_part > RttTypeMax - con->m_v.srtt) ?
            RttTypeMax : (con->m_v.srtt + var_part);
        pcb->rto = MaxValue(Constants::MinRtxTime,
                                      MinValue(Constants::MaxRtxTime, base_rto));
    }
    
    // This is called from the lower layers when sending failed but
    // is now expected to succeed. Currently the mechanism is used to
    // retry after ARP resolution completes.
    static void pcb_send_retry (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(pcb->state != TcpState::CLOSED)
        
        if (pcb->state == OneOf(TcpState::SYN_SENT, TcpState::SYN_RCVD)) {
            // Retry sending SYN or SYN-ACK.
            pcb_send_syn(pcb);
        }
        else if (can_output_in_state(pcb->state) && pcb_has_snd_outstanding(pcb)) {
            // Try sending data/FIN as permissible.
            pcb_output(pcb, false);
            
            // Delayed timer update is needed by pcb_output.
            pcb->doDelayedTimerUpdate();
        }
    }
    
    // Calculate snd_mss based on the current MtuRef information.
    static uint16_t pcb_calc_snd_mss_from_pmtu (TcpPcb *pcb, uint16_t pmtu)
    {
        AIPSTACK_ASSERT(pmtu >= TheIpStack::MinMTU)
        
        // Calculate the snd_mss from the MTU, bound to no more than base_snd_mss.
        uint16_t mtu_mss = pmtu - Ip4TcpHeaderSize;
        uint16_t snd_mss = MinValue(pcb->base_snd_mss, mtu_mss);
        
        // This snd_mss cannot be less than MinAllowedMss:
        // - base_snd_mss was explicitly checked in TcpUtils::calc_snd_mss.
        // - mtu-Ip4TcpHeaderSize cannot be less because
        //   MinAllowedMss==MinMTU-Ip4TcpHeaderSize.
        AIPSTACK_ASSERT(snd_mss >= Constants::MinAllowedMss)
        
        return snd_mss;
    }
    
    // This is called when the MtuRef notifies us that the PMTU has
    // changed. It is very important that we do not reset/deinit any
    // MtuRef here (including this PCB's, such as through pcb_abort).
    static void pcb_pmtu_changed (TcpPcb *pcb, uint16_t pmtu)
    {
        AIPSTACK_ASSERT(pcb->state != OneOf(TcpState::CLOSED,
                                         TcpState::SYN_RCVD, TcpState::TIME_WAIT))
        AIPSTACK_ASSERT(pcb->con != nullptr)
        AIPSTACK_ASSERT(pcb->con->MtuRef::isSetup())
        
        // In SYN_SENT, just update the PMTU temporarily stuffed in snd_mss.
        if (pcb->state == TcpState::SYN_SENT) {
            pcb->snd_mss = pmtu;
            return;
        }
        
        // If we are not in a state where output is possible,
        // there is nothing to do.
        if (!can_output_in_state(pcb->state)) {
            return;
        }
        
        // Calculate the new snd_mss based on the PMTU.
        uint16_t new_snd_mss = pcb_calc_snd_mss_from_pmtu(pcb, pmtu);
        
        // If the snd_mss has not changed, there is nothing to do.
        if (AIPSTACK_UNLIKELY(new_snd_mss == pcb->snd_mss)) {
            return;
        }
        
        // Update the snd_mss.
        pcb->snd_mss = new_snd_mss;
        
        Connection *con = pcb->con;
        
        // Make sure that ssthresh does not become lesser than snd_mss.
        if (con->m_v.ssthresh < pcb->snd_mss) {
            con->m_v.ssthresh = pcb->snd_mss;
        }
        
        if (pcb->hasFlag(PcbFlags::CWND_INIT)) {
            // Recalculate initial CWND (RFC 5681 page 5).
            con->m_v.cwnd = TcpUtils::calc_initial_cwnd(pcb->snd_mss);
        } else {
            // The standards do not require updating cwnd for the new snd_mss,
            // but we have to make sure that cwnd does not become less than snd_mss.
            // We also set cwnd to snd_mss if we have done a retransmission from the
            // rtx_timer and no new ACK has been received since; since the cwnd would
            // have been set to snd_mss then, and should not have been changed since
            // (the latter is not trivial to see though).
            if (con->m_v.cwnd < pcb->snd_mss || pcb->hasFlag(PcbFlags::RTX_ACTIVE)) {
                con->m_v.cwnd = pcb->snd_mss;
            }
        }
        
        // NOTE: If we decreased snd_mss, pcb_output_active may be able to send
        // something more when it was previously delaying due to pcb_may_delay_snd.
        // But we don't bother ensuring such a transmission happens immediately.
        // This is not a real case of blocked transmission because we only promise
        // tranamission when at least base_snd_mss data is queued. In other words
        // the user is anyway expected to queue more data or push.
        
        // NOTE: We must not call pcb_output_active from this function, that
        // could lead to problematic recursion via pcb_output_active ->
        // handleLocalPacketTooBig -> pcb_pmtu_changed.
    }
    
    // Update the snd_wnd to the given value.
    // NOTE: doDelayedTimerUpdate must be called after return.
    static void pcb_update_snd_wnd (TcpPcb *pcb, SeqType new_snd_wnd)
    {
        AIPSTACK_ASSERT(pcb->state != OneOf(TcpState::CLOSED,
                                         TcpState::SYN_SENT, TcpState::SYN_RCVD))
        // With maximum snd_wnd_shift=14, MaxWindow or more cannot be reported.
        AIPSTACK_ASSERT(new_snd_wnd <= Constants::MaxWindow)
        
        // If the connection has been abandoned we no longer keep snd_wnd.
        Connection *con = pcb->con;
        if (AIPSTACK_UNLIKELY(con == nullptr)) {
            return;
        }
        
        // We don't need window updates in states where output is no longer possible.
        if (AIPSTACK_UNLIKELY(!can_output_in_state(pcb->state))) {
            return;
        }
        
        // Check if the window has changed.
        SeqType old_snd_wnd = con->m_v.snd_wnd;
        if (new_snd_wnd == old_snd_wnd) {
            return;
        }
        
        // Update the window.
        con->m_v.snd_wnd = new_snd_wnd;
        
        // Is there any data or FIN outstanding to be sent/acked?
        if (pcb_has_snd_outstanding(pcb)) {
            // Set the flag OUT_PENDING so that more can be sent due to window
            // enlargement or (unlikely) window probing can start due to window
            // shrinkage.
            pcb->setFlag(PcbFlags::OUT_PENDING);
            
            // If the window now became zero or nonzero, make sure the rtx_timer
            // is stopped. Because if it is currently set for one kind of message
            // (retransmission or window probe) it might otherwise expire and send
            // the other kind too early. If the timer is actually needed it will
            // be restarted by pcb_output_active due to setting OUT_PENDING.
            if (AIPSTACK_UNLIKELY((new_snd_wnd == 0) != (old_snd_wnd == 0))) {
                pcb->tim(RtxTimer()).unset();
            }
        }
    }
    
    // Send an RST as a reply to a received segment.
    // This conforms to RFC 793 handling of segments not belonging to a known
    // connection.
    static void send_rst_reply (TcpProto *tcp, Ip4RxInfo const &ip_info,
                                TcpSegMeta const &tcp_meta, size_t tcp_data_len)
    {
        SeqType rst_seq_num;
        bool rst_ack;
        SeqType rst_ack_num;
        if ((tcp_meta.flags & Tcp4FlagAck) != 0) {
            rst_seq_num = tcp_meta.ack_num;
            rst_ack = false;
            rst_ack_num = 0;
        } else {
            rst_seq_num = 0;
            rst_ack = true;
            rst_ack_num = tcp_meta.seq_num + tcplen(tcp_meta.flags, tcp_data_len);
        }
        
        PcbKey key{ip_info.dst_addr, ip_info.src_addr,
                   tcp_meta.local_port, tcp_meta.remote_port};
        send_rst(tcp, key, rst_seq_num, rst_ack, rst_ack_num);
    }
    
    AIPSTACK_NO_INLINE
    static void send_rst (TcpProto *tcp, PcbKey const &key,
                          SeqType seq_num, bool ack, SeqType ack_num)
    {
        FlagsType flags = Tcp4FlagRst | (ack ? Tcp4FlagAck : 0);
        send_tcp_nodata(tcp, key, seq_num, ack_num, 0, flags, nullptr, nullptr);
    }
    
private:
    class PcbOutputHelper;
    
    // Set the OutputTimer to expire after no longer than OutputTimerTicks.
    // NOTE: doDelayedTimerUpdate must be called after return.
    static void pcb_set_output_timer_for_output (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(can_output_in_state(pcb->state))
        AIPSTACK_ASSERT(pcb_has_snd_outstanding(pcb))
        
        // If the OUT_RETRY flag is set, clear it and ensure that
        // the OutputTimer is stopped before the check below.
        if (AIPSTACK_UNLIKELY(pcb->hasFlag(PcbFlags::OUT_RETRY))) {
            pcb->clearFlag(PcbFlags::OUT_RETRY);
            pcb->tim(OutputTimer()).unset();
        }
        
        // Set the timer if it is not running already.
        if (!pcb->tim(OutputTimer()).isSet()) {
            pcb->tim(OutputTimer()).setAfter(Constants::OutputTimerTicks);
        }
    }
    
    // Set the OutputTimer for retrying sending.
    // NOTE: doDelayedTimerUpdate must be called after return.
    static void pcb_set_output_timer_for_retry (TcpPcb *pcb, IpErr err)
    {
        // Set the timer based on the error. Also set the flag OUT_RETRY which
        // allows pcb_set_output_timer_for_output to reset the timer it despite
        // being already set, avoiding undesired delays.
        TimeType after = (err == IpErr::BUFFER_FULL) ?
            Constants::OutputRetryFullTicks : Constants::OutputRetryOtherTicks;
        pcb->tim(OutputTimer()).setAfter(after);
        pcb->setFlag(PcbFlags::OUT_RETRY);
    }
    
    // This function sends data/FIN for referenced PCBs. It is designed to be
    // inlined into pcb_output_active and should not be called from elsewhere.
    AIPSTACK_ALWAYS_INLINE
    static IpErr pcb_output_segment (TcpPcb *pcb, PcbOutputHelper &helper,
                                     IpBufRef data, bool fin, SeqType rem_wnd,
                                     SeqType *out_seg_seqlen)
    {
        AIPSTACK_ASSERT(can_output_in_state(pcb->state))
        AIPSTACK_ASSERT(pcb->con != nullptr)
        AIPSTACK_ASSERT(data.tot_len <= pcb->con->m_v.snd_buf.tot_len)
        AIPSTACK_ASSERT(!fin || !snd_open_in_state(pcb->state))
        AIPSTACK_ASSERT(data.tot_len > 0 || fin)
        AIPSTACK_ASSERT(rem_wnd > 0)
        
        size_t rem_data_len = data.tot_len;
        
        // Calculate segment data length and adjust data to contain only that.
        // We send the minimum of:
        // - remaining data in the send buffer,
        // - remaining available window,
        // - maximum segment size.
        data.tot_len = MinValueU(rem_data_len,
                                           MinValueU(rem_wnd, pcb->snd_mss));
        
        // We always send the ACK flag, others may be added below.
        FlagsType seg_flags = Tcp4FlagAck;
        
        // Check if a FIN should be sent. This is when:
        // - a FIN is queued,
        // - there is no more data after any data sent now, and
        // - there is window availabe for the FIN.
        // The first two parts are optimized into a single condition.
        if (AIPSTACK_UNLIKELY(data.tot_len + fin > rem_data_len)) {
            if (rem_wnd > data.tot_len) {
                seg_flags |= Tcp4FlagFin|Tcp4FlagPsh;
            }
        }
        
        // Determine offset from start of send buffer.
        size_t offset = pcb->con->m_v.snd_buf.tot_len - rem_data_len;
        
        // Set the PSH flag if the push index is within this segment.
        size_t psh_index = pcb->con->m_v.snd_psh_index;
        if (TcpUtils::InOpenClosedIntervalStartLen(offset, data.tot_len, psh_index)) {
            seg_flags |= Tcp4FlagPsh;
        }
        
        // Calculate the sequence number.
        SeqType seq_num = seq_add(pcb->snd_una, offset);
        
        // Send the segment.
        IpErr err = helper.sendSegment(pcb, seq_num, seg_flags, data);
        if (AIPSTACK_UNLIKELY(err != IpErr::SUCCESS)) {
            return err;
        }
        
        // Calculate the sequence length of the segment and set
        // the FIN_SENT flag if a FIN was sent.
        SeqType seg_seqlen = data.tot_len;
        if (AIPSTACK_UNLIKELY((seg_flags & Tcp4FlagFin) != 0)) {
            seg_seqlen++;
            pcb->setFlag(PcbFlags::FIN_SENT);
        }
        
        // Return the sequence length to the caller.
        *out_seg_seqlen = seg_seqlen;
        
        // Stop a round-trip-time measurement if we have retransmitted
        // a segment containing the associated sequence number.
        if (AIPSTACK_LIKELY(pcb->hasFlag(PcbFlags::RTT_PENDING))) {
            if (AIPSTACK_UNLIKELY(seq_diff(pcb->con->m_v.rtt_test_seq, seq_num) < seg_seqlen))
            {
                pcb->clearFlag(PcbFlags::RTT_PENDING);
            }
        }
        
        // Calculate the end sequence number of the sent segment.
        SeqType seg_endseq = seq_add(seq_num, seg_seqlen);
        
        // Did we send anything new?
        if (AIPSTACK_LIKELY(seq_lt2(pcb->snd_nxt, seg_endseq))) {
            // Start a round-trip-time measurement if not already started
            // and if we still have a Connection.
            if (!pcb->hasFlag(PcbFlags::RTT_PENDING)) {
                pcb_start_rtt_measurement(pcb, false);
            }
            
            // Bump snd_nxt.
            pcb->snd_nxt = seg_endseq;
        }
        
        return IpErr::SUCCESS;
    }
    
    static void pcb_increase_cwnd_acked (TcpPcb *pcb, SeqType acked)
    {
        AIPSTACK_ASSERT(can_output_in_state(pcb->state))
        AIPSTACK_ASSERT(pcb->con != nullptr)
        
        // Increase cwnd by acked but no more than snd_mss.
        SeqType cwnd_inc = MinValueU(acked, pcb->snd_mss);
        pcb->con->m_v.cwnd = seq_add_sat(pcb->con->m_v.cwnd, cwnd_inc);
        
        // No longer have initial CWND.
        pcb->clearFlag(PcbFlags::CWND_INIT);
    }
    
    // Sets sshthresh according to RFC 5681 equation (4).
    static void pcb_update_ssthresh_for_rtx (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(can_output_in_state(pcb->state))
        AIPSTACK_ASSERT(pcb->con != nullptr)
        
        SeqType half_flight_size = seq_diff(pcb->snd_nxt, pcb->snd_una) / 2;
        SeqType two_smss = 2 * (SeqType)pcb->snd_mss;
        pcb->con->m_v.ssthresh = MaxValue(half_flight_size, two_smss);
    }
    
    static void pcb_start_rtt_measurement (TcpPcb *pcb, bool syn)
    {
        AIPSTACK_ASSERT(!syn || pcb->state == OneOf(TcpState::SYN_SENT, TcpState::SYN_RCVD))
        AIPSTACK_ASSERT(syn || can_output_in_state(pcb->state))
        AIPSTACK_ASSERT(syn || pcb->con != nullptr)
        
        // Set the flag, remember the time.
        pcb->setFlag(PcbFlags::RTT_PENDING);
        pcb->rtt_test_time = pcb->platform().getTime();
        
        // Remember the sequence number except for SYN.
        if (AIPSTACK_LIKELY(!syn)) {
            pcb->con->m_v.rtt_test_seq = pcb->snd_nxt;
        }
    }
    
    class PcbOutputHelper {
    private:
        bool prepared;
        IpChksumAccumulator::State partial_chksum_state;
        typename TheIpStack::Ip4SendPrepared ip_prep;
        TxAllocHelper<Tcp4Header::Size, HeaderBeforeIp4Dgram> dgram_alloc;
        
    public:
        inline PcbOutputHelper ()
        : prepared(false),
          dgram_alloc(TxAllocHelperUninitialized())
        {
            // We try to do as little as possible here since it would be a waste if
            // pcb_output_active() then determines that nothing needs to be sent.
            // At the first sendSegment call, we will call prepare() to setup common
            // things, to optimize sending multiple segments at a time.
        }
        
        IpErr sendSegment (TcpPcb *pcb, SeqType seq_num, FlagsType seg_flags, IpBufRef data)
        {
            // Reset the TxAllocHelper.
            dgram_alloc.reset(Tcp4Header::Size);
            
            // If this is the first tranamission, prepare common things.
            if (!prepared) {
                IpErr err = prepareCommon(pcb);
                if (AIPSTACK_UNLIKELY(err != IpErr::SUCCESS)) {
                    return err;
                }
            }
            
            // Continue calculating the checksum from the partial calculation.
            IpChksumAccumulator chksum(partial_chksum_state);
            
            // Write remaining TCP header fields...
            auto tcp_header = Tcp4Header::MakeRef(dgram_alloc.getPtr());
            
            // Sequence number
            tcp_header.set(Tcp4Header::SeqNum(), seq_num);
            chksum.addWord(WrapType<uint32_t>(), seq_num);
            
            // Offset+flags
            FlagsType offset_flags = ((FlagsType)5 << TcpOffsetShift) | seg_flags;
            tcp_header.set(Tcp4Header::OffsetFlags(), offset_flags);
            chksum.addWord(WrapType<uint16_t>(), offset_flags);
            
            // Add TCP length to checksum.
            uint16_t tcp_len = Tcp4Header::Size + data.tot_len;
            chksum.addWord(WrapType<uint16_t>(), tcp_len);
            
            // Include any data.
            IpBufNode data_node;
            if (AIPSTACK_LIKELY(data.tot_len > 0)) {
                data_node = data.toNode();
                dgram_alloc.setNext(&data_node, data.tot_len);
            }
            
            // Calculate checksum.
            tcp_header.set(Tcp4Header::Checksum(), chksum.getChksum(data));
            
            // Get the complete datagram reference starting with the TCP header.
            IpBufRef dgram = dgram_alloc.getBufRef();
            
            // Send it.
            return pcb->tcp->m_stack->sendIp4DgramFast(ip_prep, dgram, pcb);
        }
        
    private:
        IpErr prepareCommon (TcpPcb *pcb)
        {
            // We will calculate part of the checksum.
            IpChksumAccumulator chksum;
            
            // Write known TCP header fields...
            auto tcp_header = Tcp4Header::MakeRef(dgram_alloc.getPtr());
            
            // Source port
            tcp_header.set(Tcp4Header::SrcPort(), pcb->local_port);
            chksum.addWord(WrapType<uint16_t>(), pcb->local_port);
            
            // Destination port
            tcp_header.set(Tcp4Header::DstPort(), pcb->remote_port);
            chksum.addWord(WrapType<uint16_t>(), pcb->remote_port);
            
            // Acknowledgement
            tcp_header.set(Tcp4Header::AckNum(), pcb->rcv_nxt);
            chksum.addWord(WrapType<uint32_t>(), pcb->rcv_nxt);
            
            // Window size (update it first)
            uint16_t window_size = Input::pcb_ann_wnd(pcb);
            tcp_header.set(Tcp4Header::WindowSize(), window_size);
            chksum.addWord(WrapType<uint16_t>(), window_size);
            
            // Urgent pointer
            tcp_header.set(Tcp4Header::UrgentPtr(), 0);
            
            // Add known pseudo-header fields to checksum.
            chksum.addWord(WrapType<uint16_t>(), Ip4ProtocolTcp);
            chksum.addWords(&pcb->local_addr.data);
            chksum.addWords(&pcb->remote_addr.data);
            
            // Store the state of the partial checksum.
            partial_chksum_state = chksum.getState();
            
            // Perform IP level preparation.
            IpErr err = pcb->tcp->m_stack->prepareSendIp4Dgram(
                *pcb, {TcpProto::TcpTTL, Ip4ProtocolTcp}, dgram_alloc.getPtr(),
                Constants::TcpIpSendFlags, ip_prep);
            if (AIPSTACK_UNLIKELY(err != IpErr::SUCCESS)) {
                return err;
            }
            
            prepared = true;
            
            return IpErr::SUCCESS;
        }
    };
    
    AIPSTACK_NO_INLINE
    static IpErr send_tcp_nodata (
        TcpProto *tcp, PcbKey const &key, SeqType seq_num, SeqType ack_num,
        uint16_t window_size, FlagsType flags, TcpOptions *opts,
        IpSendRetryRequest *retryReq)
    {
        // Compute length of TCP options.
        uint8_t opts_len = (opts != nullptr) ? TcpUtils::calc_options_len(*opts) : 0;
        
        // Allocate memory for headers.
        TxAllocHelper<Tcp4Header::Size+TcpUtils::MaxOptionsWriteLen, HeaderBeforeIp4Dgram>
            dgram_alloc(Tcp4Header::Size+opts_len);
        
        // Caculate the offset+flags field.
        FlagsType offset_flags =
            ((FlagsType)(5+opts_len/4) << TcpOffsetShift) | flags;
        
        // The header parts of the checksum will be calculated inline.
        IpChksumAccumulator chksum_accum;
        
        // Adding constants to checksum is more easily optimized if done first.
        // Add protocol field of pseudo-header.
        chksum_accum.addWord(WrapType<uint16_t>(), Ip4ProtocolTcp);
        
        // Write the TCP header...
        auto tcp_header = Tcp4Header::MakeRef(dgram_alloc.getPtr());
        
        tcp_header.set(Tcp4Header::SrcPort(),     key.local_port);
        chksum_accum.addWord(WrapType<uint16_t>(), key.local_port);
        
        tcp_header.set(Tcp4Header::DstPort(),     key.remote_port);
        chksum_accum.addWord(WrapType<uint16_t>(), key.remote_port);
        
        tcp_header.set(Tcp4Header::SeqNum(),      seq_num);
        chksum_accum.addWord(WrapType<uint32_t>(), seq_num);
        
        tcp_header.set(Tcp4Header::AckNum(),      ack_num);
        chksum_accum.addWord(WrapType<uint32_t>(), ack_num);
        
        tcp_header.set(Tcp4Header::OffsetFlags(), offset_flags);
        chksum_accum.addWord(WrapType<uint16_t>(), offset_flags);
        
        tcp_header.set(Tcp4Header::WindowSize(),  window_size);
        chksum_accum.addWord(WrapType<uint16_t>(), window_size);
        
        tcp_header.set(Tcp4Header::UrgentPtr(),   0);
        
        // Write any TCP options.
        if (opts != nullptr) {
            TcpUtils::write_options(*opts, dgram_alloc.getPtr() + Tcp4Header::Size);
        }
        
        // Construct the datagram reference including any data.
        IpBufRef dgram = dgram_alloc.getBufRef();
        
        // Add remaining pseudo-header to checksum (protocol was added above).
        chksum_accum.addWords(&key.local_addr.data);
        chksum_accum.addWords(&key.remote_addr.data);
        chksum_accum.addWord(WrapType<uint16_t>(), dgram.tot_len);
        
        // Complete and write checksum.
        uint16_t calc_chksum = chksum_accum.getChksum(dgram.hideHeader(Tcp4Header::Size));
        tcp_header.set(Tcp4Header::Checksum(), calc_chksum);
        
        // Send the datagram.
        return tcp->m_stack->sendIp4Dgram(key, {TcpProto::TcpTTL, Ip4ProtocolTcp}, dgram,
                                          nullptr, retryReq, Constants::TcpIpSendFlags);
    }
};

}

#endif

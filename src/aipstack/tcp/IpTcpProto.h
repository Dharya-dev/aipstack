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

#ifndef AIPSTACK_IP_TCP_PROTO_H
#define AIPSTACK_IP_TCP_PROTO_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <aipstack/infra/Instance.h>
#include <aipstack/meta/ChooseInt.h>
#include <aipstack/meta/BasicMetaUtils.h>
#include <aipstack/misc/Hints.h>
#include <aipstack/misc/Assert.h>
#include <aipstack/misc/Use.h>
#include <aipstack/misc/IntRange.h>
#include <aipstack/misc/MinMax.h>
#include <aipstack/misc/ResourceArray.h>
#include <aipstack/misc/NonCopyable.h>
#include <aipstack/misc/OneOf.h>
#include <aipstack/structure/LinkedList.h>
#include <aipstack/structure/LinkModel.h>
#include <aipstack/structure/StructureRaiiWrapper.h>
#include <aipstack/structure/Accessor.h>
#include <aipstack/infra/Buf.h>
#include <aipstack/infra/SendRetry.h>
#include <aipstack/infra/Options.h>
#include <aipstack/proto/Ip4Proto.h>
#include <aipstack/proto/Tcp4Proto.h>
#include <aipstack/ip/IpAddr.h>
#include <aipstack/ip/IpStack.h>
#include <aipstack/platform/PlatformFacade.h>
#include <aipstack/tcp/TcpUtils.h>
#include <aipstack/tcp/TcpOosBuffer.h>
#include <aipstack/tcp/TcpApi.h>
#include <aipstack/tcp/TcpListener.h>
#include <aipstack/tcp/TcpConnection.h>
#include <aipstack/tcp/TcpMultiTimer.h>
#include <aipstack/tcp/IpTcpProto_constants.h>
#include <aipstack/tcp/IpTcpProto_input.h>
#include <aipstack/tcp/IpTcpProto_output.h>

namespace AIpStack {

/**
 * TCP protocol implementation.
 */
template <typename Arg>
class IpTcpProto :
    private NonCopyable<IpTcpProto<Arg>>,
    private TcpApi<Arg>
{
    AIPSTACK_USE_VALS(Arg::Params, (TcpTTL, NumTcpPcbs, NumOosSegs,
                                    EphemeralPortFirst, EphemeralPortLast,
                                    LinkWithArrayIndices))
    AIPSTACK_USE_TYPES(Arg::Params, (PcbIndexService))
    AIPSTACK_USE_TYPES(Arg, (PlatformImpl, StackArg))
    
    using Platform = PlatformFacade<PlatformImpl>;
    AIPSTACK_USE_TYPE(Platform, TimeType)
    
    static_assert(NumTcpPcbs > 0, "");
    static_assert(NumOosSegs > 0 && NumOosSegs < 16, "");
    static_assert(EphemeralPortFirst > 0, "");
    static_assert(EphemeralPortFirst <= EphemeralPortLast, "");
    
    template <typename> friend class IpTcpProto_constants;
    template <typename> friend class IpTcpProto_input;
    template <typename> friend class IpTcpProto_output;
    template <typename> friend class TcpApi;
    template <typename> friend class TcpListener;
    template <typename> friend class TcpConnection;
    
    using Constants = IpTcpProto_constants<Arg>;
    using Input = IpTcpProto_input<Arg>;
    using Output = IpTcpProto_output<Arg>;
    
    AIPSTACK_USE_TYPES(TcpUtils, (TcpState, TcpOptions, PcbKey, PcbKeyCompare, SeqType))
    AIPSTACK_USE_VALS(TcpUtils, (state_is_active, accepting_data_in_state,
                                 snd_open_in_state))
    AIPSTACK_USE_TYPES(Constants, (RttType))
    
    struct TcpPcb;
    
    // PCB flags, see flags in TcpPcb.
    using FlagsType = std::uint16_t;
    struct PcbFlags { enum : FlagsType {
        // ACK is needed; used in input processing
        ACK_PENDING = FlagsType(1) << 0,
        // pcb_output_active/pcb_output_abandoned should be called at the end of
        // input processing. This flag must imply can_output_in_state and
        // pcb_has_snd_outstanding at the point in pcb_input where it is checked.
        // Any change that would break this implication must clear the flag.
        OUT_PENDING = FlagsType(1) << 1,
        // A FIN was sent at least once and is included in snd_nxt
        FIN_SENT    = FlagsType(1) << 2,
        // A FIN is to queued for sending
        FIN_PENDING = FlagsType(1) << 3,
        // Round-trip-time is being measured
        RTT_PENDING = FlagsType(1) << 4,
        // Round-trip-time is not in initial state
        RTT_VALID   = FlagsType(1) << 5,
        // cwnd has been increaded by snd_mss this round-trip
        CWND_INCRD  = FlagsType(1) << 6,
        // A segment has been retransmitted and not yet acked
        RTX_ACTIVE  = FlagsType(1) << 7,
        // The recover variable valid (and >=snd_una)
        RECOVER     = FlagsType(1) << 8,
        // If rtx_timer is running it is for idle timeout
        IDLE_TIMER  = FlagsType(1) << 9,
        // Window scaling is used
        WND_SCALE   = FlagsType(1) << 10,
        // Current cwnd is the initial cwnd
        CWND_INIT   = FlagsType(1) << 11,
        // If OutputTimer is set it is for OutputRetry*Ticks
        OUT_RETRY   = FlagsType(1) << 12,
        // rcv_ann_wnd needs update before sending a segment, implies con != nullptr
        RCV_WND_UPD = FlagsType(1) << 13,
        // NOTE: Currently no more bits are available, see TcpPcb::flags.
    }; };
    
    // Number of ephemeral ports.
    static PortNum const NumEphemeralPorts = EphemeralPortLast - EphemeralPortFirst + 1;
    
    // Unsigned integer type usable as an index for the PCBs array.
    // We use the largest value of that type as null (which cannot
    // be a valid PCB index).
    using PcbIndexType = ChooseIntForMax<NumTcpPcbs, false>;
    static PcbIndexType const PcbIndexNull = PcbIndexType(-1);
    
    // Instantiate the out-of-sequence buffering.
    using OosBufferService = TcpOosBufferService<
        TcpOosBufferServiceOptions::NumOosSegs::Is<NumOosSegs>
    >;
    AIPSTACK_MAKE_INSTANCE(OosBuffer, (OosBufferService))
    
    struct PcbLinkModel;
    
    // Instantiate the PCB index.
    struct PcbIndexAccessor;
    using PcbIndexLookupKeyArg = PcbKey const &;
    struct PcbIndexKeyFuncs;
    AIPSTACK_MAKE_INSTANCE(PcbIndex, (PcbIndexService::template Index<
        PcbIndexAccessor, PcbIndexLookupKeyArg, PcbIndexKeyFuncs, PcbLinkModel,
        /*Duplicates=*/false>))
    
    using ListenerLinkModel = PointerLinkModel<TcpListener<Arg>>;
    
    using Listener = TcpListener<Arg>;
    using Connection = TcpConnection<Arg>;
    
    // These TcpPcb fields are injected into TcpMultiTimer to fill up what
    // would otherwise be holes in the layout, for better memory use.
    struct MultiTimerUserData {
        // The base send MSS. It is computed based on the interface
        // MTU and the MTU option provided by the peer.
        // In the SYN_SENT state this is set based on the interface MTU and
        // the calculation is completed at the transition to ESTABLISHED.
        std::uint16_t base_snd_mss;
    };
    
    /**
     * Timers:
     * AbrtTimer: for aborting PCB (TIME_WAIT, abandonment)
     * OutputTimer: for pcb_output after send buffer extension
     * RtxTimer: for retransmission, window probe and cwnd idle reset
     */
    struct AbrtTimer {};
    struct OutputTimer {};
    struct RtxTimer {};
    using PcbMultiTimer = TcpMultiTimer<
        PlatformImpl, TcpPcb, MultiTimerUserData, AbrtTimer, OutputTimer, RtxTimer>;
    
    /**
     * A TCP Protocol Control Block.
     * These are maintained internally within the stack and may
     * survive deinit/reset of an associated Connection object.
     */
    struct TcpPcb final :
        // Send retry request (inherited for efficiency).
        public IpSendRetryRequest,
        // PCB timers.
        public PcbMultiTimer,
        // Local/remote IP address and port
        public PcbKey
    {
        using PcbMultiTimer::platform;
        
        inline TcpPcb (typename IpTcpProto::Platform platform_, IpTcpProto *tcp_) :
            PcbMultiTimer(platform_),
            tcp(tcp_),
            state(TcpState::CLOSED)
        {
            con = nullptr;
            
            // Add the PCB to the list of unreferenced PCBs.
            tcp->m_unrefed_pcbs_list.prepend({*this, *tcp}, *tcp);
        }
        
        inline ~TcpPcb ()
        {
            AIPSTACK_ASSERT(state != TcpState::SYN_RCVD)
            AIPSTACK_ASSERT(con == nullptr)
        }
        
        // Node for the PCB index.
        typename PcbIndex::Node index_hook;
        
        // Node for the unreferenced PCBs list.
        // The function pcb_is_in_unreferenced_list specifies exactly when
        // a PCB is suposed to be in the unreferenced list. The only
        // exception to this is while pcb_unlink_con is during the callback
        // pcb_unlink_con-->pcb_aborted-->connectionAborted.
        LinkedListNode<PcbLinkModel> unrefed_list_node;
        
        // Pointer back to IpTcpProto.
        IpTcpProto *tcp;    
        
        union {
            // Pointer to the associated Listener, if in SYN_RCVD.
            Listener *lis;
            
            // Pointer to any associated Connection, otherwise.
            Connection *con;
        };
        
        // Sender variables.
        SeqType snd_una;
        SeqType snd_nxt;
        
        // Receiver variables.
        SeqType rcv_nxt;
        SeqType rcv_ann_wnd; // ensured to fit in size_t (in case size_t is 16-bit)
        
        // Round-trip-time and retransmission time management.
        typename IpTcpProto::TimeType rtt_test_time;
        RttType rto;
        
        // The maximum segment size we will send.
        // This is dynamic based on Path MTU Discovery, but it will always
        // be between Constants::MinAllowedMss and base_snd_mss.
        // It is first properly initialized at the transition to ESTABLISHED
        // state, before that in SYN_SENT/SYN_RCVD is is used to store the
        // pmtu/iface_mss respectively.
        // Due to invariants and other requirements associated with snd_mss,
        // fixups must be performed when snd_mss is changed, specifically of
        // ssthresh, cwnd and rtx_timer (see pcb_pmtu_changed).
        std::uint16_t snd_mss;
        
        // NOTE: The following 5 fields are uint32_t to encourage compilers
        // to pack them into a single 32-bit word, if they were narrower
        // they may be packed less efficiently.
        
        // Flags (see comments in PcbFlags).
        std::uint32_t flags : 14;
        
        // PCB state.
        std::uint32_t state : TcpUtils::TcpStateBits;
        
        // Number of duplicate ACKs (>=FastRtxDupAcks means we're in fast recovery).
        std::uint32_t num_dupack : Constants::DupAckBits;
        
        // Window shift values.
        std::uint32_t snd_wnd_shift : 4;
        std::uint32_t rcv_wnd_shift : 4;
        
        // Convenience functions for flags.
        inline bool hasFlag (FlagsType flag) { return (flags & flag) != 0; }
        inline void setFlag (FlagsType flag) { flags |= flag; }
        inline void clearFlag (FlagsType flag) { flags &= ~flag; }
        
        // Check if a flag is set and clear it.
        inline bool hasAndClearFlag (FlagsType flag)
        {
            FlagsType the_flags = flags;
            if ((the_flags & flag) != 0) {
                flags = the_flags & ~flag;
                return true;
            }
            return false;
        }
        
        // Check if we are called from PCB input processing (pcb_input).
        inline bool inInputProcessing ()
        {
            return this == tcp->m_current_pcb;
        }
        
        // Apply delayed timer updates. This must be called after any PCB timer
        // has been changed before returning to the event loop.
        inline void doDelayedTimerUpdate ()
        {
            PcbMultiTimer::doDelayedUpdate();
        }
        
        // Call doDelayedTimerUpdate if not called from input processing (pcb_input).
        // The update is not needed if called from pcb_input as it will be done at
        // return from pcb_input.
        inline void doDelayedTimerUpdateIfNeeded ()
        {
            if (!inInputProcessing()) {
                doDelayedTimerUpdate();
            }
        }
        
        // Trampolines for timer handlers.
        
        inline void timerExpired (AbrtTimer)
        {
            pcb_abrt_timer_handler(this);
        }
        
        inline void timerExpired (OutputTimer)
        {
            Output::pcb_output_timer_handler(this);
        }
        
        inline void timerExpired (RtxTimer)
        {
            Output::pcb_rtx_timer_handler(this);
        }
        
        // Send retry callback.
        void retrySending () override final { Output::pcb_send_retry(this); }
    };
    
    // Define the hook accessor for the PCB index.
    struct PcbIndexAccessor : public MemberAccessor<TcpPcb, typename PcbIndex::Node,
                                                    &TcpPcb::index_hook> {};
    
public:
    /**
     * Initialize the TCP protocol implementation.
     * 
     * The TCP will register itself with the IpStack to receive incoming TCP packets.
     */
    IpTcpProto (IpProtocolHandlerArgs<StackArg> args) :
        m_stack(args.stack),
        m_current_pcb(nullptr),
        m_next_ephemeral_port(EphemeralPortFirst),
        m_pcbs(ResourceArrayInitSame(), args.platform, this)
    {
        AIPSTACK_ASSERT(args.stack != nullptr)
    }
    
    /**
     * Deinitialize the TCP protocol implementation.
     * 
     * Any TCP listeners and connections must have been deinited.
     * It is not permitted to call this from any TCP callbacks.
     */
    ~IpTcpProto ()
    {
        AIPSTACK_ASSERT(m_listeners_list.isEmpty())
        AIPSTACK_ASSERT(m_current_pcb == nullptr)
    }
    
    inline TcpApi<Arg> & getApi ()
    {
        return *this;
    }

    inline void recvIp4Dgram (IpRxInfoIp4<StackArg> const &ip_info, IpBufRef dgram)
    {
        Input::recvIp4Dgram(this, ip_info, dgram);
    }
    
    inline void handleIp4DestUnreach (Ip4DestUnreachMeta const &du_meta,
                IpRxInfoIp4<StackArg> const &ip_info, IpBufRef dgram_initial)
    {
        Input::handleIp4DestUnreach(this, du_meta, ip_info, dgram_initial);
    }
    
private:
    inline Platform platform () const
    {
        return m_pcbs[0].platform();
    }
    
    TcpPcb * allocate_pcb ()
    {
        // No PCB available?
        if (m_unrefed_pcbs_list.isEmpty()) {
            return nullptr;
        }
        
        // Get a PCB to use.
        TcpPcb *pcb = m_unrefed_pcbs_list.lastNotEmpty(*this);
        AIPSTACK_ASSERT(pcb_is_in_unreferenced_list(pcb))
        
        // Abort the PCB if it's not closed.
        if (pcb->state != TcpState::CLOSED) {
            pcb_abort(pcb);
        } else {
            pcb_assert_closed(pcb);
        }
        
        return pcb;
    }
    
    void pcb_assert_closed (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(!pcb->tim(AbrtTimer()).isSet())
        AIPSTACK_ASSERT(!pcb->tim(OutputTimer()).isSet())
        AIPSTACK_ASSERT(!pcb->tim(RtxTimer()).isSet())
        AIPSTACK_ASSERT(!pcb->IpSendRetryRequest::isActive())
        AIPSTACK_ASSERT(pcb->tcp == this)
        AIPSTACK_ASSERT(pcb->state == TcpState::CLOSED)
        AIPSTACK_ASSERT(pcb->con == nullptr)
        (void)pcb;
    }
    
    inline static void pcb_abort (TcpPcb *pcb)
    {
        // This function aborts a PCB while sending an RST in
        // all states except these.
        bool send_rst = pcb->state !=
            OneOf(TcpState::SYN_SENT, TcpState::SYN_RCVD, TcpState::TIME_WAIT);
        
        pcb_abort(pcb, send_rst);
    }
    
    static void pcb_abort (TcpPcb *pcb, bool send_rst)
    {
        AIPSTACK_ASSERT(pcb->state != TcpState::CLOSED)
        IpTcpProto *tcp = pcb->tcp;
        
        // Send RST if desired.
        if (send_rst) {
            Output::pcb_send_rst(pcb);
        }
        
        if (pcb->state == TcpState::SYN_RCVD) {
            // Disassociate the Listener.
            pcb_unlink_lis(pcb);
        } else {
            // Disassociate any Connection. This will call the
            // connectionAborted callback if we do have a Connection.
            pcb_unlink_con(pcb, true);
        }
        
        // If this is called from input processing of this PCB,
        // clear m_current_pcb. This way, input processing can
        // detect aborts performed from within user callbacks.
        if (tcp->m_current_pcb == pcb) {
            tcp->m_current_pcb = nullptr;
        }
        
        // Remove the PCB from the index in which it is.
        if (pcb->state == TcpState::TIME_WAIT) {
            tcp->m_pcb_index_timewait.removeEntry({*pcb, *tcp}, *tcp);
        } else {
            tcp->m_pcb_index_active.removeEntry({*pcb, *tcp}, *tcp);
        }
        
        // Make sure the PCB is at the end of the unreferenced list.
        if (pcb != tcp->m_unrefed_pcbs_list.lastNotEmpty(*tcp)) {
            tcp->m_unrefed_pcbs_list.remove({*pcb, *tcp}, *tcp);
            tcp->m_unrefed_pcbs_list.append({*pcb, *tcp}, *tcp);
        }
        
        // Reset other relevant fields to initial state.
        pcb->PcbMultiTimer::unsetAll();
        pcb->IpSendRetryRequest::reset();
        pcb->state = TcpState::CLOSED;
        
        tcp->pcb_assert_closed(pcb);
    }
    
    // NOTE: doDelayedTimerUpdate must be called after return.
    // We are okay because this is only called from pcb_input.
    static void pcb_go_to_time_wait (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(pcb->state != OneOf(TcpState::CLOSED, TcpState::SYN_RCVD,
                                         TcpState::TIME_WAIT))
        
        // Disassociate any Connection. This will call the
        // connectionAborted callback if we do have a Connection.
        pcb_unlink_con(pcb, false);
        
        // Set snd_nxt to snd_una in order to not accept any more acknowledgements.
        // This is currently not necessary since we only enter TIME_WAIT after
        // having received a FIN, but in the future we might do some non-standard
        // transitions where this is not the case.
        pcb->snd_nxt = pcb->snd_una;
        
        // Change state.
        pcb->state = TcpState::TIME_WAIT;
        
        // Move the PCB from the active index to the time-wait index.
        IpTcpProto *tcp = pcb->tcp;
        tcp->m_pcb_index_active.removeEntry({*pcb, *tcp}, *tcp);
        tcp->m_pcb_index_timewait.addEntry({*pcb, *tcp}, *tcp);
        
        // Stop timers due to asserts in their handlers.
        pcb->tim(OutputTimer()).unset();
        pcb->tim(RtxTimer()).unset();
        
        // Clear the OUT_PENDING flag due to its preconditions.
        pcb->clearFlag(PcbFlags::OUT_PENDING);
        
        // Start the TIME_WAIT timeout.
        pcb->tim(AbrtTimer()).setAfter(Constants::TimeWaitTimeTicks);
    }
    
    // NOTE: doDelayedTimerUpdate must be called after return.
    // We are okay because this is only called from pcb_input.
    static void pcb_go_to_fin_wait_2 (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(pcb->state == TcpState::FIN_WAIT_1)
        
        // Change state.
        pcb->state = TcpState::FIN_WAIT_2;
        
        // Stop these timers due to asserts in their handlers.
        pcb->tim(OutputTimer()).unset();
        pcb->tim(RtxTimer()).unset();
        
        // Clear the OUT_PENDING flag due to its preconditions.
        pcb->clearFlag(PcbFlags::OUT_PENDING);
        
        // Reset the MTU reference.
        if (pcb->con != nullptr) {
            pcb->con->mtu_ref().reset(pcb->tcp->m_stack);
        }
    }
    
    static void pcb_unlink_con (TcpPcb *pcb, bool closing)
    {
        AIPSTACK_ASSERT(pcb->state != OneOf(TcpState::CLOSED, TcpState::SYN_RCVD))
        
        if (pcb->con != nullptr) {
            // Inform the connection object about the aborting.
            // Note that the PCB is not yet on the list of unreferenced
            // PCBs, which protects it from being aborted by allocate_pcb
            // during this callback.
            Connection *con = pcb->con;
            AIPSTACK_ASSERT(con->m_v.pcb == pcb)
            con->pcb_aborted();
            
            // The pcb->con has been cleared by con->pcb_aborted().
            AIPSTACK_ASSERT(pcb->con == nullptr)
            
            // Add the PCB to the list of unreferenced PCBs.
            IpTcpProto *tcp = pcb->tcp;
            if (closing) {
                tcp->m_unrefed_pcbs_list.append({*pcb, *tcp}, *tcp);
            } else {
                tcp->m_unrefed_pcbs_list.prepend({*pcb, *tcp}, *tcp);
            }
        }
    }
    
    static void pcb_unlink_lis (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(pcb->state == TcpState::SYN_RCVD)
        AIPSTACK_ASSERT(pcb->lis != nullptr)
        
        Listener *lis = pcb->lis;
        
        // Decrement the listener's PCB count.
        AIPSTACK_ASSERT(lis->m_num_pcbs > 0)
        lis->m_num_pcbs--;
        
        // Is this a PCB which is being accepted?
        if (lis->m_accept_pcb == pcb) {
            // Break the link from the listener.
            lis->m_accept_pcb = nullptr;
            
            // The PCB was removed from the list of unreferenced
            // PCBs, so we have to add it back.
            IpTcpProto *tcp = pcb->tcp;
            tcp->m_unrefed_pcbs_list.append({*pcb, *tcp}, *tcp);
        }
        
        // Clear pcb->con since we will be going to CLOSED state
        // and it is was not undefined due to the union with pcb->lis.
        pcb->con = nullptr;
    }
    
    // This is called from Connection::reset when the Connection
    // is abandoning the PCB.
    static void pcb_abandoned (TcpPcb *pcb, bool rst_needed, SeqType rcv_ann_thres)
    {
        AIPSTACK_ASSERT(pcb->state == TcpState::SYN_SENT || state_is_active(pcb->state))
        AIPSTACK_ASSERT(pcb->con == nullptr) // Connection just cleared it
        IpTcpProto *tcp = pcb->tcp;
        
        // Add the PCB to the unreferenced PCBs list.
        // This has not been done by Connection.
        tcp->m_unrefed_pcbs_list.append({*pcb, *tcp}, *tcp);
        
        // Clear any RTT_PENDING flag since we've lost the variables
        // needed for RTT measurement.
        pcb->clearFlag(PcbFlags::RTT_PENDING);
        
        // Clear RCV_WND_UPD flag since this flag must imply con != nullptr.
        pcb->clearFlag(PcbFlags::RCV_WND_UPD);
        
        // Abort if in SYN_SENT state or some data is queued or some data was received but
        // not processed by the application. The pcb_abort() will decide whether to send an
        // RST (no RST in SYN_SENT, RST otherwise).
        if (pcb->state == TcpState::SYN_SENT || rst_needed) {
            return pcb_abort(pcb);
        }
        
        // Make sure any idle timeout is stopped, because pcb_rtx_timer_handler
        // requires the connection to not be abandoned when the idle timeout expires.
        if (pcb->hasFlag(PcbFlags::IDLE_TIMER)) {
            pcb->clearFlag(PcbFlags::IDLE_TIMER);
            pcb->tim(RtxTimer()).unset();
        }
        
        // Arrange for sending the FIN.
        if (snd_open_in_state(pcb->state)) {
            Output::pcb_end_sending(pcb);
        }
        
        // If we haven't received a FIN, possibly announce more window
        // to encourage the peer to send its outstanding data/FIN.
        if (accepting_data_in_state(pcb->state)) {
            Input::pcb_update_rcv_wnd_after_abandoned(pcb, rcv_ann_thres);
        }
        
        // Start the abort timeout.
        pcb->tim(AbrtTimer()).setAfter(Constants::AbandonedTimeoutTicks);
        
        pcb->doDelayedTimerUpdateIfNeeded();
    }
    
    static void pcb_abrt_timer_handler (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(pcb->state != TcpState::CLOSED)
        
        // Abort the PCB.
        pcb_abort(pcb);
        
        // NOTE: A TcpMultiTimer callback would normally need to call doDelayedTimerUpdate
        // before returning to the event loop but pcb_abort calls PcbMultiTimer::unsetAll
        // which is also sufficient.
    }
    
    // This is used to check within pcb_input if the PCB was aborted
    // while performing a user callback.
    inline static bool pcb_aborted_in_callback (TcpPcb *pcb)
    {
        // It is safe to read pcb->tcp since PCBs cannot just go away
        // while in input processing. If the PCB was aborted or even
        // reused, the tcp pointer must still be valid.
        IpTcpProto *tcp = pcb->tcp;
        AIPSTACK_ASSERT(tcp->m_current_pcb == pcb || tcp->m_current_pcb == nullptr)
        return tcp->m_current_pcb == nullptr;
    }
    
    inline SeqType make_iss ()
    {
        return SeqType(platform().getTime());
    }
    
    Listener * find_listener (Ip4Addr addr, PortNum port)
    {
        for (Listener *lis = m_listeners_list.first();
             lis != nullptr; lis = m_listeners_list.next(*lis))
        {
            AIPSTACK_ASSERT(lis->m_listening)
            if (lis->m_addr == addr && lis->m_port == port) {
                return lis;
            }
        }
        return nullptr;
    }
    
    void unlink_listener (Listener *lis)
    {
        // Abort any PCBs associated with the listener (without RST).
        for (TcpPcb &pcb : m_pcbs) {
            if (pcb.state == TcpState::SYN_RCVD && pcb.lis == lis) {
                pcb_abort(&pcb, false);
            }
        }
    }
    
    IpErr create_connection (Connection *con, TcpStartConnectionArgs<Arg> const &args,
                             std::uint16_t pmtu, TcpPcb **out_pcb)
    {
        AIPSTACK_ASSERT(con != nullptr)
        AIPSTACK_ASSERT(con->mtu_ref().isSetup())
        AIPSTACK_ASSERT(out_pcb != nullptr)

        Ip4Addr remote_addr = args.addr;
        PortNum remote_port = args.port;
        std::size_t user_rcv_wnd = args.rcv_wnd;
        
        // Determine the interface and local IP address.
        IpIface<StackArg> *iface;
        Ip4Addr local_addr;
        IpErr select_err = m_stack->selectLocalIp4Address(remote_addr, iface, local_addr);
        if (select_err != IpErr::SUCCESS) {
            return select_err;
        }
        
        // Determine the local port.
        PortNum local_port = get_ephemeral_port(local_addr, remote_addr, remote_port);
        if (local_port == 0) {
            return IpErr::NO_PORT_AVAIL;
        }
        
        // Calculate the MSS based on the interface MTU.
        std::uint16_t iface_mss = iface->getMtu() - Ip4TcpHeaderSize;
        
        // Allocate the PCB.
        TcpPcb *pcb = allocate_pcb();
        if (pcb == nullptr) {
            return IpErr::NO_PCB_AVAIL;
        }
        
        // NOTE: If another error case is added after this, make sure
        // to reset the MtuRef before abandoning the PCB!
        
        // Remove the PCB from the unreferenced PCBs list.
        m_unrefed_pcbs_list.remove({*pcb, *this}, *this);
        
        // Generate an initial sequence number.
        SeqType iss = make_iss();
        
        // The initial receive window will be at least one for the SYN and
        // at most 16-bit wide since SYN segments have unscaled window.
        // NOTE: rcv_ann_wnd after SYN-ACKSYN reception (-1) fits into size_t
        // as required since user_rcv_wnd is size_t.
        SeqType rcv_wnd = 1 + MinValueU(std::uint16_t(TypeMax<std::uint16_t>() - 1), user_rcv_wnd);
        
        // Initialize most of the PCB.
        pcb->state = TcpState::SYN_SENT;
        pcb->flags = PcbFlags::WND_SCALE; // to send the window scale option
        pcb->con = con;
        pcb->local_addr = local_addr;
        pcb->remote_addr = remote_addr;
        pcb->local_port = local_port;
        pcb->remote_port = remote_port;
        pcb->rcv_nxt = 0; // it is sent in the SYN
        pcb->rcv_ann_wnd = rcv_wnd;
        pcb->snd_una = iss;
        pcb->snd_nxt = iss;
        pcb->snd_mss = pmtu; // store PMTU here temporarily
        pcb->base_snd_mss = iface_mss; // will be updated when the SYN-ACK is received
        pcb->rto = Constants::InitialRtxTime;
        pcb->num_dupack = 0;
        pcb->snd_wnd_shift = 0;
        pcb->rcv_wnd_shift = Constants::RcvWndShift;
        
        // Add the PCB to the active index.
        m_pcb_index_active.addEntry({*pcb, *this}, *this);
        
        // Start the connection timeout.
        pcb->tim(AbrtTimer()).setAfter(Constants::SynSentTimeoutTicks);
        
        // Start the retransmission timer.
        pcb->tim(RtxTimer()).setAfter(Output::pcb_rto_time(pcb));
        
        pcb->doDelayedTimerUpdate();
        
        // Send the SYN.
        Output::pcb_send_syn(pcb);
        
        // Return the PCB.
        *out_pcb = pcb;
        return IpErr::SUCCESS;
    }
    
    PortNum get_ephemeral_port (Ip4Addr local_addr,
                                 Ip4Addr remote_addr, PortNum remote_port)
    {
        for (PortNum i : IntRange(NumEphemeralPorts)) {
            (void)i;
            
            PortNum port = m_next_ephemeral_port;
            m_next_ephemeral_port = (port < EphemeralPortLast) ?
                (port + 1) : EphemeralPortFirst;
            
            if (find_pcb({local_addr, remote_addr, port, remote_port}) == nullptr) {
                return port;
            }
        }
        
        return 0;
    }
    
    inline static bool pcb_is_in_unreferenced_list (TcpPcb *pcb)
    {
        return pcb->state == TcpState::SYN_RCVD ? pcb->lis->m_accept_pcb != pcb
                                                : pcb->con == nullptr;
    }
    
    void move_unrefed_pcb_to_front (TcpPcb *pcb)
    {
        AIPSTACK_ASSERT(pcb_is_in_unreferenced_list(pcb))
        
        if (pcb != m_unrefed_pcbs_list.first(*this)) {
            m_unrefed_pcbs_list.remove({*pcb, *this}, *this);
            m_unrefed_pcbs_list.prepend({*pcb, *this}, *this);
        }
    }
    
    // Find a PCB by address tuple.
    TcpPcb * find_pcb (PcbKey const &key)
    {
        // Look in the active index first.
        TcpPcb *pcb = m_pcb_index_active.findEntry(key, *this);
        AIPSTACK_ASSERT(pcb == nullptr ||
                     pcb->state != OneOf(TcpState::CLOSED, TcpState::TIME_WAIT))
        
        // If not found, look in the time-wait index.
        if (AIPSTACK_UNLIKELY(pcb == nullptr)) {
            pcb = m_pcb_index_timewait.findEntry(key, *this);
            AIPSTACK_ASSERT(pcb == nullptr || pcb->state == TcpState::TIME_WAIT)
        }
        
        return pcb;
    }
    
    // Find a listener by local address and port. This also considers listeners bound
    // to wildcard address since it is used to associate received segments with a listener.
    Listener * find_listener_for_rx (Ip4Addr local_addr, PortNum local_port)
    {
        for (Listener *lis = m_listeners_list.first();
             lis != nullptr; lis = m_listeners_list.next(*lis))
        {
            AIPSTACK_ASSERT(lis->m_listening)
            if (lis->m_port == local_port &&
                (lis->m_addr == local_addr || lis->m_addr.isZero()))
            {
                return lis;
            }
        }
        return nullptr;
    }
    
    // This is used by the two PCB indexes to obtain the keys
    // defining the ordering of the PCBs and compare keys.
    // The key comparison functions are inherited from PcbKeyCompare.
    struct PcbIndexKeyFuncs : public PcbKeyCompare {
        inline static PcbKey const & GetKeyOfEntry (TcpPcb const &pcb)
        {
            // TcpPcb inherits PcbKey so just return pcb.
            return pcb;
        }
    };
    
    // Define the link model for data structures of PCBs.
    struct PcbArrayAccessor;
    struct PcbLinkModel : public std::conditional_t<LinkWithArrayIndices,
        ArrayLinkModelWithAccessor<
            TcpPcb, PcbIndexType, PcbIndexNull, IpTcpProto, PcbArrayAccessor>,
        PointerLinkModel<TcpPcb>
    > {};
    AIPSTACK_USE_TYPES(PcbLinkModel, (Ref, State))
    
private:
    using ListenersList = LinkedList<
        MemberAccessor<Listener, LinkedListNode<ListenerLinkModel>,
                       &Listener::m_listeners_node>,
        ListenerLinkModel, false>;
    
    using UnrefedPcbsList = LinkedList<
        MemberAccessor<TcpPcb, LinkedListNode<PcbLinkModel>, &TcpPcb::unrefed_list_node>,
        PcbLinkModel, true>;
    
    IpStack<StackArg> *m_stack;
    StructureRaiiWrapper<ListenersList> m_listeners_list;
    TcpPcb *m_current_pcb;
    IpBufRef m_received_opts_buf;
    TcpOptions m_received_opts;
    PortNum m_next_ephemeral_port;
    StructureRaiiWrapper<UnrefedPcbsList> m_unrefed_pcbs_list;
    StructureRaiiWrapper<typename PcbIndex::Index> m_pcb_index_active;
    StructureRaiiWrapper<typename PcbIndex::Index> m_pcb_index_timewait;
    ResourceArray<TcpPcb, NumTcpPcbs> m_pcbs;
    
    struct PcbArrayAccessor : public
        MemberAccessor<IpTcpProto, ResourceArray<TcpPcb, NumTcpPcbs>,
                       &IpTcpProto::m_pcbs> {};
};

struct IpTcpProtoOptions {
    AIPSTACK_OPTION_DECL_VALUE(TcpTTL, std::uint8_t, 64)
    AIPSTACK_OPTION_DECL_VALUE(NumTcpPcbs, int, 32)
    AIPSTACK_OPTION_DECL_VALUE(NumOosSegs, std::uint8_t, 4)
    AIPSTACK_OPTION_DECL_VALUE(EphemeralPortFirst, std::uint16_t, 49152)
    AIPSTACK_OPTION_DECL_VALUE(EphemeralPortLast, std::uint16_t, 65535)
    AIPSTACK_OPTION_DECL_TYPE(PcbIndexService, void)
    AIPSTACK_OPTION_DECL_VALUE(LinkWithArrayIndices, bool, true)
};

template <typename... Options>
class IpTcpProtoService {
    template <typename> friend class IpTcpProto;
    template <typename> friend class TcpConnection;
    
    AIPSTACK_OPTION_CONFIG_VALUE(IpTcpProtoOptions, TcpTTL)
    AIPSTACK_OPTION_CONFIG_VALUE(IpTcpProtoOptions, NumTcpPcbs)
    AIPSTACK_OPTION_CONFIG_VALUE(IpTcpProtoOptions, NumOosSegs)
    AIPSTACK_OPTION_CONFIG_VALUE(IpTcpProtoOptions, EphemeralPortFirst)
    AIPSTACK_OPTION_CONFIG_VALUE(IpTcpProtoOptions, EphemeralPortLast)
    AIPSTACK_OPTION_CONFIG_TYPE(IpTcpProtoOptions, PcbIndexService)
    AIPSTACK_OPTION_CONFIG_VALUE(IpTcpProtoOptions, LinkWithArrayIndices)
    
public:
    // This tells IpStack which IP protocol we receive packets for.
    using IpProtocolNumber = WrapValue<Ip4Protocol, Ip4Protocol::Tcp>;
    
#ifndef IN_DOXYGEN
    template <typename PlatformImpl_, typename StackArg_>
    struct Compose {
        using PlatformImpl = PlatformImpl_;
        using StackArg = StackArg_;
        using Params = IpTcpProtoService;
        AIPSTACK_DEF_INSTANCE(Compose, IpTcpProto)
    };
#endif
};

}

#endif

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

#ifndef AIPSTACK_IPREASSEMBLY_H
#define AIPSTACK_IPREASSEMBLY_H

#include <cstdint>
#include <cstring>

#include <aipstack/misc/Use.h>
#include <aipstack/misc/Assert.h>
#include <aipstack/misc/MinMax.h>
#include <aipstack/misc/NonCopyable.h>
#include <aipstack/misc/MinMax.h>
#include <aipstack/misc/Function.h>
#include <aipstack/infra/Struct.h>
#include <aipstack/infra/Buf.h>
#include <aipstack/infra/Options.h>
#include <aipstack/infra/Instance.h>
#include <aipstack/proto/Ip4Proto.h>
#include <aipstack/ip/IpAddr.h>
#include <aipstack/platform/PlatformFacade.h>

namespace AIpStack {

/**
 * @addtogroup ip-stack
 * @{
 */

#ifndef IN_DOXYGEN

template <typename Arg>
class IpReassembly;

/**
 * Implements IPv4 datagram reassembly.
 * 
 * The implementation uses the strategy suggested in RFC 815, whereby hole
 * descriptors are placed at the beginnings of holes.
 * 
 * @tparam Arg An instantiated @ref IpReassemblyService::Compose template
 *         or a type derived from such. Note that the @ref IpStack actually
 *         performs this instantiation, the application must just pass an
 *         instantiated @ref IpReassemblyService to @ref IpStackService.
 */
template <typename Arg>
class IpReassembly :
    private NonCopyable<IpReassembly<Arg>>
{
    AIPSTACK_USE_VALS(Arg::Params, (MaxReassEntrys, MaxReassSize, MaxReassHoles,
                                    MaxReassTimeSeconds))
    AIPSTACK_USE_TYPES(Arg, (PlatformImpl))
    
    using Platform = PlatformFacade<PlatformImpl>;
    AIPSTACK_USE_TYPES(Platform, (TimeType))
    
    static_assert(MaxReassEntrys > 0, "");
    static_assert(MaxReassSize >= Ip4RequiredRecvSize, "");
    static_assert(MaxReassHoles >= 1, "");
    static_assert(MaxReassHoles <= 250, ""); // important to prevent num_holes overflow
    static_assert(MaxReassTimeSeconds >= 5, "");
    
    // Null link value in HoleDescriptor lists.
    static std::uint16_t const ReassNullLink = TypeMax<std::uint16_t>();
    
    // Hole descriptor structure, placed at the beginning of a hole.
    AIPSTACK_DEFINE_STRUCT(HoleDescriptor,
        (HoleSize,       StructRawField<std::uint16_t>)
        (NextHoleOffset, StructRawField<std::uint16_t>)
    )
    
    // We need to be able to put a hole descriptor after the reassembled data.
    static_assert(MaxReassSize <= TypeMax<std::uint16_t>() - HoleDescriptor::Size, "");
    
    // The size of the reassembly buffers, with additional space for a hole descriptor
    // at the end.
    static std::uint16_t const ReassBufferSize = MaxReassSize + HoleDescriptor::Size;
    
    // Maximum time that a reassembly entry can be valid.
    static TimeType const ReassMaxExpirationTicks =
        MaxReassTimeSeconds * TimeType(Platform::TimeFreq);
    
    static_assert(ReassMaxExpirationTicks <= Platform::WorkingTimeSpanTicks, "");
    
    // Interval of the purge timer. Use as large as possible, we only need it to
    // expire before any expiration time becomes ambiguous due to clock wraparound.
    static TimeType const PurgeTimerInterval = Platform::WorkingTimeSpanTicks;
    
    struct ReassEntry {
        // Offset in data to the first hole, or ReassNullLink for free entry.
        std::uint16_t first_hole_offset;
        // The total data length, or 0 if last fragment not yet received.
        std::uint16_t data_length;
        // Time after which the entry is considered invalid.
        TimeType expiration_time;
        // IPv4 header (options not stored).
        char header[Ip4Header::Size];
        // Data and holes, each hole starts with a HoleDescriptor.
        // The last HoleDescriptor::Size bytes are to ensure these is space
        // for the last hole descriptor, they cannot contain data.
        char data[ReassBufferSize];
    };
    
private:
    typename Platform::Timer m_timer;
    IpBufNode m_reass_node;
    ReassEntry m_reass_packets[MaxReassEntrys];
    
public:
    /**
     * Constructor.
     * 
     * @param platform_ The platform facade.
     */
    IpReassembly (Platform platform_) :
        m_timer(platform_, AIPSTACK_BIND_MEMBER_TN(&IpReassembly::timerHandler, this))
    {
        // Start the timer for the first interval.
        m_timer.setAfter(PurgeTimerInterval);
        
        // Mark all reassembly entries as unused.
        for (auto &reass : m_reass_packets) {
            reass.first_hole_offset = ReassNullLink;
        }
    }

    inline Platform platform () const
    {
        return m_timer.platform();
    }
    
    /**
     * Process a received packet and possibly return a reassembled datagram.
     * 
     * This must only be called for packets which are not complete datagrams,
     * that is packets which have the more-fragments flag set or a nonzero
     * fragment offset.
     * 
     * @param ident IP identification field.
     * @param src_addr Source address.
     * @param dst_addr Destination address.
     * @param proto IP protocol number.
     * @param ttl Time-to-live.
     * @param more_fragments More-fragments flag.
     * @param fragment_offset Fragment offset in bytes.
     * @param header Pointer to the IPv4 header (only the base header is used).
     *        The data in the header must match the various arguments of this
     *        function (ident...fragment_offset).
     * @param dgram The IP payload of the incoming datagram must be passed,
     *        and if a datagram is reassembled (return value is true) then this
     *        will be changed to reference the reassembled payload, otherwise it
     *        will not be changed. If a reassembled datagram is returned, then the
     *        referenced memory region may be used until the next call of this
     *        function.
     * @return True if a datagram was reassembled, false if not.
     */
    bool reassembleIp4 (std::uint16_t ident, Ip4Addr src_addr, Ip4Addr dst_addr, std::uint8_t proto,
                        std::uint8_t ttl, bool more_fragments, std::uint16_t fragment_offset,
                        char const *header, IpBufRef &dgram)
    {
        AIPSTACK_ASSERT(dgram.tot_len <= TypeMax<std::uint16_t>())
        AIPSTACK_ASSERT(more_fragments || fragment_offset > 0)
        
        // Sanity check data length.
        if (dgram.tot_len == 0) {
            return false;
        }
        
        // Check if we have a reassembly entry for this datagram.
        TimeType now = platform().getTime();
        ReassEntry *reass = find_reass_entry(now, ident, src_addr, dst_addr, proto);
        
        if (reass == nullptr) {
            // Allocate an entry.
            reass = alloc_reass_entry(now, ttl);
            
            // Copy the IP header.
            std::memcpy(reass->header, header, Ip4Header::Size);
            
            // Set first hole and unknown data length.
            reass->first_hole_offset = 0;
            reass->data_length = 0;
            
            // Write a hole from start of data to infinity (ReassBufferSize).
            // The final HoleDescriptor::Size bytes of the hole serve as
            // infinity because they cannot be filled by a fragment. This also
            // means that we will always have at least one hole in the list.
            auto hole = HoleDescriptor::MakeRef(reass->data);
            hole.set(typename HoleDescriptor::HoleSize(),       ReassBufferSize);
            hole.set(typename HoleDescriptor::NextHoleOffset(), ReassNullLink);
        }
        
        do {
            // Verify that the fragment fits into the buffer.
            if (fragment_offset > MaxReassSize ||
                dgram.tot_len > std::uint16_t(MaxReassSize - fragment_offset))
            {
                goto invalidate_reass;
            }
            std::uint16_t fragment_end = std::uint16_t(fragment_offset + dgram.tot_len);
            
            // Summary of last-fragment related sanity checks:
            // - When we first receive a last fragment, we remember the data size and
            //   also check that we have not yet received any data that would fall
            //   beyond the end of this last fragment.
            // - When we receive any subsequent fragment after having received a last
            //   fragment, we check that it does not contain any data beyond the
            //   remembered end of data.
            // - When we receive any additional last fragment we check that it has
            //   the same end as the first received last fragment.
            
            // Is this the last fragment?
            if (!more_fragments) {
                // Check for inconsistent data_length.
                if (reass->data_length != 0 && fragment_end != reass->data_length) {
                    goto invalidate_reass;
                }
                
                // Remember the data_length.
                reass->data_length = fragment_end;
            } else {
                // Check for data beyond the end.
                if (reass->data_length != 0 && fragment_end > reass->data_length) {
                    goto invalidate_reass;
                }
            }
            
            // Update the holes based on this fragment.
            std::uint16_t prev_hole_offset = ReassNullLink;
            std::uint16_t hole_offset = reass->first_hole_offset;
            std::uint8_t num_holes = 0;
            do {
                AIPSTACK_ASSERT(prev_hole_offset == ReassNullLink ||
                             hole_offset_valid(prev_hole_offset))
                AIPSTACK_ASSERT(hole_offset_valid(hole_offset))
                
                // Get the hole info.
                auto hole = HoleDescriptor::MakeRef(reass->data + hole_offset);
                std::uint16_t hole_size = hole.get(typename HoleDescriptor::HoleSize());
                std::uint16_t next_hole_offset =
                    hole.get(typename HoleDescriptor::NextHoleOffset());
                
                // Calculate the hole end.
                AIPSTACK_ASSERT(hole_size <= ReassBufferSize - hole_offset)
                std::uint16_t hole_end = hole_offset + hole_size;
                
                // If this is the last fragment, sanity check that the hole offset
                // is not greater than the end of this fragment; this would mean
                // that some data was received beyond the end.
                if (!more_fragments && hole_offset > fragment_end) {
                    goto invalidate_reass;
                }
                
                // If the fragment does not overlap with the hole, skip the hole.
                if (fragment_offset >= hole_end || fragment_end <= hole_offset) {
                    prev_hole_offset = hole_offset;
                    hole_offset = next_hole_offset;
                    num_holes++;
                    continue;
                }
                
                // The fragment overlaps with the hole. We will be dismantling
                // this hole and creating between zero and two new holes.
                
                // Create a new hole on the left if needed.
                if (fragment_offset > hole_offset) {
                    // Sanity check hole size.
                    std::uint16_t new_hole_size = fragment_offset - hole_offset;
                    if (new_hole_size < HoleDescriptor::Size) {
                        goto invalidate_reass;
                    }
                    
                    // Write the hole size.
                    // Note that the hole is in the same place as the old hole.
                    auto new_hole = HoleDescriptor::MakeRef(reass->data + hole_offset);
                    new_hole.set(typename HoleDescriptor::HoleSize(), new_hole_size);
                    
                    // The link to this hole is already set up.
                    //reass_link_prev(reass, prev_hole_offset, hole_offset);
                    
                    // Advance prev_hole_offset to this hole.
                    prev_hole_offset = hole_offset;
                    
                    num_holes++;
                }
                
                // Create a new hole on the right if needed.
                if (fragment_end < hole_end) {
                    // Sanity check hole size.
                    std::uint16_t new_hole_size = hole_end - fragment_end;
                    if (new_hole_size < HoleDescriptor::Size) {
                        goto invalidate_reass;
                    }
                    
                    // Write the hole size.
                    auto new_hole = HoleDescriptor::MakeRef(reass->data + fragment_end);
                    new_hole.set(typename HoleDescriptor::HoleSize(), new_hole_size);
                    
                    // Setup the link to this hole.
                    reass_link_prev(reass, prev_hole_offset, fragment_end);
                    
                    // Advance prev_hole_offset to this hole.
                    prev_hole_offset = fragment_end;
                    
                    num_holes++;
                }
                
                // Setup the link to the next hole.
                reass_link_prev(reass, prev_hole_offset, next_hole_offset);
                
                // Advance to the next hole (if any).
                hole_offset = next_hole_offset;
            } while (hole_offset != ReassNullLink);
            
            // It is not possible that there are no more holes due to
            // the final HoleDescriptor::Size bytes that cannot be filled.
            AIPSTACK_ASSERT(reass->first_hole_offset != ReassNullLink)
            
            // Copy the fragment data into the reassembly buffer.
            IpBufRef dgram_tmp = dgram;
            dgram_tmp.takeBytes(dgram.tot_len, reass->data + fragment_offset);
            
            // If we have not yet received the final fragment or there
            // are still holes after the end, the reassembly is not complete.
            if (reass->data_length == 0 || reass->first_hole_offset < reass->data_length) {
                // If there are too many holes, invalidate.
                if (num_holes > MaxReassHoles) {
                    goto invalidate_reass;
                }
                return false;
            }
            
            // If the above check passed, this must be the last hole starting at exactly
            // data_length and spanning to the end of the buffer (ReassBufferSize).
            // Consider that when we first got a !more_fragments fragment, we would have
            // aborted if there was any existing data buffered beyond data_length or if
            // we later received a fragment with data beyond that.
            AIPSTACK_ASSERT(reass->first_hole_offset == reass->data_length)
#if AIPSTACK_ASSERTIONS
            auto hole = HoleDescriptor::MakeRef(reass->data + reass->first_hole_offset);
            std::uint16_t hole_size        = hole.get(typename HoleDescriptor::HoleSize());
            std::uint16_t next_hole_offset = hole.get(typename HoleDescriptor::NextHoleOffset());
            AIPSTACK_ASSERT(hole_size == ReassBufferSize - reass->first_hole_offset)
            AIPSTACK_ASSERT(next_hole_offset == ReassNullLink)
#endif
            
            // Invalidate the reassembly entry.
            reass->first_hole_offset = ReassNullLink;
            
            // Setup dgram to point to the reassembled data.
            m_reass_node = IpBufNode{reass->data, MaxReassSize, nullptr};
            dgram = IpBufRef{&m_reass_node, 0, reass->data_length};
            
            // Continue to process the reassembled datagram.
            return true;
        } while (false);
        
    invalidate_reass:
        reass->first_hole_offset = ReassNullLink;
        return false;
    }
    
private:
    ReassEntry * find_reass_entry (TimeType now, std::uint16_t ident, Ip4Addr src_addr,
                                   Ip4Addr dst_addr, std::uint8_t proto)
    {
        ReassEntry *found_entry = nullptr;
        
        for (auto &reass : m_reass_packets) {
            // Ignore free entries.
            if (reass.first_hole_offset == ReassNullLink) {
                continue;
            }
            
            // If the entry has expired, mark is as free and ignore.
            if (TimeType(reass.expiration_time - now) > ReassMaxExpirationTicks) {
                reass.first_hole_offset = ReassNullLink;
                continue;
            }
            
            // If the entry matches, return it after goingh through all
            // so that we purge all expired entries.
            auto reass_hdr = Ip4Header::MakeRef(reass.header);
            if (reass_hdr.get(Ip4Header::Ident())    == ident &&
                reass_hdr.get(Ip4Header::SrcAddr())  == src_addr &&
                reass_hdr.get(Ip4Header::DstAddr())  == dst_addr &&
                std::uint8_t(reass_hdr.get(Ip4Header::TtlProto()) == proto))
            {
                found_entry = &reass;
            }
        }
        
        return found_entry;
    }
    
    ReassEntry * alloc_reass_entry (TimeType now, std::uint8_t ttl)
    {
        TimeType future = now + ReassMaxExpirationTicks;
        
        ReassEntry *result_reass = nullptr;
        
        for (auto &reass : m_reass_packets) {
            // If the entry is unused, use it.
            if (reass.first_hole_offset == ReassNullLink) {
                result_reass = &reass;
                break;
            }
            
            // Look for the entry with the least expiration time.
            if (result_reass == nullptr ||
                TimeType(future - reass.expiration_time) >
                    TimeType(future - result_reass->expiration_time))
            {
                result_reass = &reass;
            }
        }
        
        // Set the expiration time.
        std::uint8_t seconds = MinValue(ttl, MaxReassTimeSeconds);
        result_reass->expiration_time = now + seconds * TimeType(Platform::TimeFreq);
        
        return result_reass;
    }
    
    static void reass_link_prev (ReassEntry *reass, std::uint16_t prev_hole_offset, std::uint16_t hole_offset)
    {
        AIPSTACK_ASSERT(prev_hole_offset == ReassNullLink || hole_offset_valid(prev_hole_offset))
        
        if (prev_hole_offset == ReassNullLink) {
            reass->first_hole_offset = hole_offset;
        } else {
            auto prev_hole = HoleDescriptor::MakeRef(reass->data + prev_hole_offset);
            prev_hole.set(typename HoleDescriptor::NextHoleOffset(), hole_offset);
        }
    }
    
    inline static bool hole_offset_valid (std::uint16_t hole_offset)
    {
        return hole_offset <= MaxReassSize;
    }
    
    void timerHandler ()
    {
        // Restart the timer.
        m_timer.setAfter(PurgeTimerInterval);
        
        // Purge any expired reassembly entries by calling find_reass_entry with
        // dummy IP information.
        TimeType now = platform().getTime();
        find_reass_entry(now, 0, Ip4Addr::ZeroAddr(), Ip4Addr::ZeroAddr(), 0);
    }
};

#endif

/**
 * Static configuration options for @ref IpReassemblyService.
 */
struct IpReassemblyOptions {
    /**
     * Maximum number of datagrams being reassembled. This affects memory use.
     */
    AIPSTACK_OPTION_DECL_VALUE(MaxReassEntrys, int, 1)
    
    /**
     * Maximum size of reassembled datagrams. This affects memory use.
     */
    AIPSTACK_OPTION_DECL_VALUE(MaxReassSize, std::uint16_t, 1480)
    
    /**
     * Maximum number of holes in an incompletely reassembled datagram.
     */
    AIPSTACK_OPTION_DECL_VALUE(MaxReassHoles, std::uint8_t, 10)
    
    /**
     * Maximum allowed timeout of an incompletely reassembled datagram,
     * as an additional restriction to the TTL seconds limit.
     */
    AIPSTACK_OPTION_DECL_VALUE(MaxReassTimeSeconds, std::uint8_t, 60)
};

/**
 * Service definition for the IP Reassembly implementation.
 * 
 * An instantiation of this template must be passed to @ref IpStackService.
 * 
 * The template parameters of this class are assignments of options defined in
 * @ref IpReassemblyOptions, for example:
 * AIpStack::IpReassemblyOptions::MaxReassEntrys::Is\<5\>.
 * 
 * @tparam Options Assignments of options defined in @ref IpReassemblyOptions.
 */
template <typename... Options>
class IpReassemblyService {
    template <typename>
    friend class IpReassembly;
    
    AIPSTACK_OPTION_CONFIG_VALUE(IpReassemblyOptions, MaxReassEntrys)
    AIPSTACK_OPTION_CONFIG_VALUE(IpReassemblyOptions, MaxReassSize)
    AIPSTACK_OPTION_CONFIG_VALUE(IpReassemblyOptions, MaxReassHoles)
    AIPSTACK_OPTION_CONFIG_VALUE(IpReassemblyOptions, MaxReassTimeSeconds)
    
public:
#ifndef IN_DOXYGEN
    template <typename PlatformImpl_>
    struct Compose {
        using PlatformImpl = PlatformImpl_;
        using Params = IpReassemblyService;
        AIPSTACK_DEF_INSTANCE(Compose, IpReassembly)        
    };
#endif
};

/** @} */

}

#endif

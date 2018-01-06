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

#ifndef AIPSTACK_LINKED_HEAP_H
#define AIPSTACK_LINKED_HEAP_H

#include <stddef.h>
#include <stdint.h>

#include <type_traits>
#include <initializer_list>

#include <aipstack/misc/Assert.h>
#include <aipstack/misc/Hints.h>
#include <aipstack/misc/MinMax.h>

namespace AIpStack {

//#define AIPSTACK_LINKED_HEAP_VERIFY 1

template <typename, typename, typename, typename>
class LinkedHeap;

template <typename LinkModel>
class LinkedHeapNode {
    template<typename, typename, typename, typename>
    friend class LinkedHeap;
    
    using Link = typename LinkModel::Link;
    
private:
    Link parent;
    Link link[2];
};

template <
    typename Accessor,
    typename Compare,
    typename LinkModel,
    typename SizeType = size_t
>
class LinkedHeap
{
    static_assert(std::is_unsigned<SizeType>::value, "");
    
    using Link = typename LinkModel::Link;
    
private:
    Link m_root; // remaining fields are undefined when root is null
    Link m_last;
    SizeType m_count;
    SizeType m_level_bit;
    
public:
    using State = typename LinkModel::State;
    using Ref = typename LinkModel::Ref;
    
    inline void init ()
    {
        m_root = Link::null();
    }
    
    inline bool isEmpty () const
    {
        return m_root.isNull();
    }
    
    inline Ref first (State st = State()) const
    {
        return m_root.ref(st);
    }
    
    AIPSTACK_OPTIMIZE_SIZE
    void insert (Ref node, State st = State())
    {
        AIPSTACK_ASSERT(m_root.isNull() || m_count > 0)
        AIPSTACK_ASSERT(m_root.isNull() || m_count < TypeMax<SizeType>())
        
        int8_t child_dir;
        Ref child;
        
        if (AIPSTACK_UNLIKELY(m_root.isNull())) {
            m_root = node.link(st);
            m_count = 1;
            m_level_bit = 1;
            
            ac(node).parent = Link::null();
            child_dir = -1;
        } else {
            SizeType prev_count = increment_count();
            SizeType new_count = m_count;
            bool from_root = should_walk_from_root(prev_count, new_count, m_level_bit);
            
            Ref cur;
            bool dir;
            
            if (from_root) {
                SizeType bit = m_level_bit;
                cur = m_root.ref(st);
                
                while (bit > 2) {
                    bit >>= 1;
                    bool next_dir = (new_count & bit) != 0;
                    
                    AIPSTACK_ASSERT(!ac(cur).link[next_dir].isNull())
                    cur = ac(cur).link[next_dir].ref(st);
                }
                
                dir = (new_count & 1);
            } else {
                cur = m_last.ref(st);
                Ref parent = ac(cur).parent.ref(st);
                AIPSTACK_ASSERT(!parent.isNull())
                
                while (cur.link(st) == ac(parent).link[1]) {
                    AIPSTACK_ASSERT(!ac(cur).parent.isNull())
                    cur = parent;
                    parent = ac(cur).parent.ref(st);
                }
                
                if (!ac(parent).link[1].isNull()) {
                    cur = ac(parent).link[1].ref(st);
                    dir = false;
                    
                    while (!ac(cur).link[0].isNull()) {
                        cur = ac(cur).link[0].ref(st);
                    }
                } else {
                    cur = parent;
                    dir = true;
                }
            }
            
            Ref parent = cur;
            AIPSTACK_ASSERT(ac(parent).link[dir].isNull())
            AIPSTACK_ASSERT(ac(parent).link[1].isNull())
            
            if (Compare::compareEntries(st, parent, node) <= 0) {
                ac(parent).link[dir] = node.link(st);
                ac(node).parent = parent.link(st);
                child_dir = -1;
            } else {
                child = node;
                node = parent;
                child_dir = dir;
            }
        }
        
        m_last = node.link(st);
        
        Link other_child;
        if (child_dir >= 0) {
            other_child = ac(node).link[!child_dir];
        }
        
        ac(node).link[0] = Link::null();
        ac(node).link[1] = Link::null();
        
        if (child_dir >= 0) {
            bubble_up_node(st, child, node, other_child, child_dir);
        }
        
        assertValidHeap(st);
    }
    
    AIPSTACK_OPTIMIZE_SIZE
    void remove (Ref node, State st = State())
    {
        AIPSTACK_ASSERT(!m_root.isNull())
        AIPSTACK_ASSERT(m_count > 0)
        
        if (AIPSTACK_UNLIKELY(m_count == 1)) {
            m_root = Link::null();
        } else {
            SizeType prev_count = decrement_count();
            SizeType new_count = m_count;
            bool from_root = should_walk_from_root(prev_count, new_count, m_level_bit);
            
            Ref cur;
            
            if (from_root) {
                Ref last_parent = ac(m_last.ref(st)).parent.ref(st);
                ac(last_parent).link[m_last == ac(last_parent).link[1]] = Link::null();
                
                SizeType bit = m_level_bit;
                cur = m_root.ref(st);
                
                while (bit > 1) {
                    bit >>= 1;
                    bool next_dir = (new_count & bit) != 0;
                    
                    AIPSTACK_ASSERT(!ac(cur).link[next_dir].isNull())
                    cur = ac(cur).link[next_dir].ref(st);
                }
            } else {
                cur = m_last.ref(st);
                Ref parent = ac(cur).parent.ref(st);
                AIPSTACK_ASSERT(!parent.isNull())
                
                bool dir = cur.link(st) == ac(parent).link[1];
                ac(parent).link[dir] = Link::null();
                
                if (dir) {
                    AIPSTACK_ASSERT(!ac(parent).link[0].isNull())
                    cur = ac(parent).link[0].ref(st);
                    
                    AIPSTACK_ASSERT(ac(cur).link[0].isNull())
                    AIPSTACK_ASSERT(ac(cur).link[1].isNull())
                } else {
                    do {
                        cur = parent;
                        AIPSTACK_ASSERT(!ac(cur).parent.isNull())
                        parent = ac(cur).parent.ref(st);
                    } while (cur.link(st) == ac(parent).link[0]);
                    
                    AIPSTACK_ASSERT(!ac(parent).link[0].isNull())
                    cur = ac(parent).link[0].ref(st);
                    
                    AIPSTACK_ASSERT(!ac(cur).link[1].isNull())
                    do {
                        cur = ac(cur).link[1].ref(st);
                    } while (!ac(cur).link[1].isNull());
                }
            }
            
            Ref srcnode = m_last.ref(st);
            
            if (!(node == cur)) {
                m_last = cur.link(st);
            }
            
            if (!(node == srcnode)) {
                fixup_node(st, node, srcnode);
            }
        }
        
        assertValidHeap(st);
    }
    
    AIPSTACK_OPTIMIZE_SIZE
    void fixup (Ref node, State st = State())
    {
        AIPSTACK_ASSERT(!m_root.isNull())
        AIPSTACK_ASSERT(m_count > 0)
        
        if (AIPSTACK_LIKELY(m_count != 1)) {
            fixup_node(st, node, node);
        }
        
        assertValidHeap(st);
    }
    
    template <typename KeyType, typename Func>
    inline void findAllLesserOrEqual (KeyType key, Func func, State st = State())
    {
        find_all_lesser_or_equal(st, key, func, m_root);
    }
    
    template <typename KeyType>
    AIPSTACK_OPTIMIZE_SIZE
    Ref findFirstLesserOrEqual (KeyType key, State st = State())
    {
        Ref root = m_root.ref(st);
        if (!root.isNull() && Compare::compareKeyEntry(st, key, root) >= 0) {
            return root;
        }
        
        return Ref::null();
    }
    
    template <typename KeyType>
    AIPSTACK_OPTIMIZE_SIZE
    Ref findNextLesserOrEqual (KeyType key, Ref node, State st = State())
    {
        AIPSTACK_ASSERT(!node.isNull())
        
        for (bool side : {false, true}) {
            Ref child = ac(node).link[side].ref(st);
            if (!child.isNull() && Compare::compareKeyEntry(st, key, child) >= 0) {
                return child;
            }
        }
        
        Ref parent = ac(node).parent.ref(st);
        
        while (!parent.isNull()) {
            if (!(node.link(st) == ac(parent).link[1])) {
                AIPSTACK_ASSERT(node.link(st) == ac(parent).link[0])
                
                Ref sibling = ac(parent).link[1].ref(st);
                if (!sibling.isNull() && Compare::compareKeyEntry(st, key, sibling) >= 0) {
                    return sibling;
                }
            }
            
            node = parent;
            parent = ac(node).parent.ref(st);
        }
        
        return Ref::null();
    }
    
    inline void assertValidHeap (State st = State())
    {
        (void)st;
#if AIPSTACK_LINKED_HEAP_VERIFY
        verifyHeap(st);
#endif
    }
    
    AIPSTACK_OPTIMIZE_SIZE
    void verifyHeap (State st = State())
    {
        if (m_root.isNull()) {
            return;
        }
        
        AssertData ad;
        ad.state = AssertState::NoDepth;
        ad.prev_leaf = Link::null();
        ad.count = 0;
        
        AIPSTACK_ASSERT_FORCE(!m_last.isNull())
        AIPSTACK_ASSERT_FORCE(ac(m_root.ref(st)).parent.isNull())
        
        assert_recurser(st, m_root.ref(st), ad, 0);
        
        AIPSTACK_ASSERT_FORCE(ad.prev_leaf == m_last)
        AIPSTACK_ASSERT_FORCE(ad.count == m_count)
        
        int bits = 0;
        SizeType x = m_count;
        while (x > 0) {
            x /= 2;
            bits++;
        }
        AIPSTACK_ASSERT_FORCE(m_level_bit == ((SizeType)1 << (bits - 1)))
    }
    
private:
    inline static LinkedHeapNode<LinkModel> & ac (Ref ref)
    {
        return Accessor::access(*ref);
    }
    
    AIPSTACK_OPTIMIZE_SIZE
    inline SizeType increment_count ()
    {
        SizeType prev_count = m_count;
        m_count = prev_count + 1;
        SizeType next_level_bit = 2 * m_level_bit;
        if (m_count == next_level_bit) {
            m_level_bit = next_level_bit;
        }
        return prev_count;
    }
    
    AIPSTACK_OPTIMIZE_SIZE
    inline SizeType decrement_count ()
    {
        SizeType prev_count = m_count;
        m_count = prev_count - 1;
        if (prev_count == m_level_bit) {
            m_level_bit = m_level_bit / 2;
        }
        return prev_count;
    }
    
    // Used in insert and remove, determines whether the new last node should
    // be found from the old last node or from the root. The result is the
    // approach which requires least hops preferring not waking from root if it
    // would be the same except that when changing levels it always prefers
    // walking from root.
    AIPSTACK_OPTIMIZE_SIZE
    inline static bool should_walk_from_root (
        SizeType prev_count, SizeType new_count, SizeType new_level_bit)
    {
        // Compute how many bits change in the node count, expressed as the
        // bit index (for rollover_bit=2^n, the number of changed bits is n).
        // Overflow in +1 is possible but handled later.
        SizeType rollover_bit = (prev_count ^ new_count) + 1;
        
        // Compute the cost of walking from the old last node, which is twice
        // the number of changed bits. Expressed as the bit index, this is obtained
        // by squaring rollover_bit. Overflow in multiplication is possible but
        // handled later.
        // Note that when changing levels, this calculation is literally wrong
        // giving too high cost, but the result will ensure that we pick walking
        // from root, which cannot be less efficient in such cases.
        SizeType fromlast_cost_bit = rollover_bit * rollover_bit;
        
        // Compare the cost of walking from the old last node to the cost of
        // walking from root. The cost of the latter is new_level_bit expressed as
        // the bit position just like fromlast_cost_bit. Therefore we want to
        // check whether fromlast_cost_bit > new_level_bit, if overflows could not
        // occur. We handle overflows by instead checking as seen below:
        // - If there was an overflow in +1 above, then rollover_bit is zero,
        //   fromlast_cost_bit is zero, fromlast_cost_bit-1 is the max value and
        //   the result is true (walk from root). This happens when changing
        //   to/from the last representable level, and it is correct to walk from
        //   root as the cost of walking from the last node is the height but
        //   walking from last node is twice that much.
        // - If there was an overflow in the multiplication, then fromlast_cost_bit
        //   is zero, fromlast_cost_bit-1 is the max value and the result is still
        //   correctly true (walk from root).
        return (SizeType)(fromlast_cost_bit - 1) >= new_level_bit;
    }
    
    AIPSTACK_OPTIMIZE_SIZE
    void bubble_up_node (State st, Ref node, Ref parent, Link sibling, bool side)
    {
        Ref gparent;
        
        while (true) {
            gparent = ac(parent).parent.ref(st);
            if (gparent.isNull() || Compare::compareEntries(st, gparent, node) <= 0) {
                break;
            }
            
            bool next_side = parent.link(st) == ac(gparent).link[1];
            Link next_sibling = ac(gparent).link[!next_side];
            
            ac(gparent).link[side] = parent.link(st);
            ac(parent).parent = gparent.link(st);
            
            if (!(ac(gparent).link[!side] = sibling).isNull()) {
                ac(sibling.ref(st)).parent = gparent.link(st);
            }
            
            side = next_side;
            sibling = next_sibling;
            parent = gparent;
        }
        
        ac(node).link[side] = parent.link(st);
        ac(parent).parent = node.link(st);
        
        if (!(ac(node).link[!side] = sibling).isNull()) {
            ac(sibling.ref(st)).parent = node.link(st);
        }
        
        if (!(ac(node).parent = gparent.link(st)).isNull()) {
            ac(gparent).link[parent.link(st) == ac(gparent).link[1]] = node.link(st);
        } else {
            m_root = node.link(st);
        }
    }
    
    AIPSTACK_OPTIMIZE_SIZE
    void connect_and_bubble_down_node (State st, Ref node, Ref parent, int8_t side, Link child0, Link child1)
    {
        while (true) {
            // Find minimum child (if any)
            Ref child = child0.ref(st);
            bool next_side = false;
            Ref child1_ref = child1.ref(st);
            if (!child1_ref.isNull() && Compare::compareEntries(st, child1_ref, child) < 0) {
                child = child1_ref;
                next_side = true;
            }
            
            // If there is a minimum child but it is >= node, clear child
            // so we don't bubble down any further.
            if (!child.isNull() && Compare::compareEntries(st, child, node) >= 0) {
                child = Ref::null();
            }
            
            if (AIPSTACK_UNLIKELY(side < 0)) {
                if (child.isNull()) {
                    return;
                }
                
                parent = ac(node).parent.ref(st);
                side = !parent.isNull() && node.link(st) == ac(parent).link[1];
            } else {
                if (child.isNull()) {
                    break;
                }
            }
            
            Link other_child = next_side ? child0 : child1;
            
            child0 = ac(child).link[0];
            child1 = ac(child).link[1];
            
            if (!(ac(child).parent = parent.link(st)).isNull()) {
                ac(parent).link[side] = child.link(st);
            } else {
                m_root = child.link(st);
            }
            
            if (!(ac(child).link[!next_side] = other_child).isNull()) {
                ac(other_child.ref(st)).parent = child.link(st);
            }
            
            if (m_last == child.link(st)) {
                m_last = node.link(st);
            }
            
            parent = child;
            side = next_side;
        }
        
        if (!(ac(node).parent = parent.link(st)).isNull()) {
            ac(parent).link[side] = node.link(st);
        } else {
            m_root = node.link(st);
        }
        
        if (!(ac(node).link[0] = child0).isNull()) {
            ac(child0.ref(st)).parent = node.link(st);
        }
        
        if (!(ac(node).link[1] = child1).isNull()) {
            ac(child1.ref(st)).parent = node.link(st);
        }
    }
    
    AIPSTACK_OPTIMIZE_SIZE
    void fixup_node (State st, Ref node, Ref srcnode)
    {
        Link child0 = ac(node).link[0];
        Link child1 = ac(node).link[1];
        
        Ref parent = ac(node).parent.ref(st);
        int8_t side = !parent.isNull() && node.link(st) == ac(parent).link[1];
        
        if (!parent.isNull() && Compare::compareEntries(st, srcnode, parent) < 0) {
            Link sibling = ac(parent).link[!side];
            
            if (!(ac(parent).link[0] = child0).isNull()) {
                ac(child0.ref(st)).parent = parent.link(st);
            }
            
            if (!(ac(parent).link[1] = child1).isNull()) {
                ac(child1.ref(st)).parent = parent.link(st);
            }
            
            if (m_last == srcnode.link(st)) {
                m_last = parent.link(st);
            }
            
            bubble_up_node(st, srcnode, parent, sibling, side);
        } else {
            if (node == srcnode) {
                side = -1;
            }
            
            connect_and_bubble_down_node(st, srcnode, parent, side, child0, child1);
        }
    }
    
    template <typename KeyType, typename Func>
    void find_all_lesser_or_equal (State st, KeyType key, Func func, Link node_link)
    {
        Ref node;
        if (node_link.isNull() ||
            Compare::compareKeyEntry(st, key, (node = node_link.ref(st))) < 0)
        {
            return;
        }
        
        func(static_cast<Ref>(node));
        
        find_all_lesser_or_equal(st, key, func, ac(node).link[0]);
        find_all_lesser_or_equal(st, key, func, ac(node).link[1]);
    }
    
    enum class AssertState {NoDepth, Lowest, LowestEnd};
    
    struct AssertData {
        AssertState state;
        int level;
        Link prev_leaf;
        SizeType count;
    };
    
    AIPSTACK_OPTIMIZE_SIZE
    void assert_recurser (State st, Ref n, AssertData &ad, int level)
    {
        ad.count++;
        
        if (ac(n).link[0].isNull() && ac(n).link[1].isNull()) {
            if (ad.state == AssertState::NoDepth) {
                ad.state = AssertState::Lowest;
                ad.level = level;
            }
        } else {
            if (!ac(n).link[0].isNull()) {
                AIPSTACK_ASSERT_FORCE(Compare::compareEntries(st, n, ac(n).link[0].ref(st)) <= 0)
                AIPSTACK_ASSERT_FORCE(ac(ac(n).link[0].ref(st)).parent == n.link(st))
                assert_recurser(st, ac(n).link[0].ref(st), ad, level + 1);
            }
            if (!ac(n).link[1].isNull()) {
                AIPSTACK_ASSERT_FORCE(Compare::compareEntries(st, n, ac(n).link[1].ref(st)) <= 0)
                AIPSTACK_ASSERT_FORCE(ac(ac(n).link[1].ref(st)).parent == n.link(st))
                assert_recurser(st, ac(n).link[1].ref(st), ad, level + 1);
            }
        }
        
        AIPSTACK_ASSERT_FORCE(ad.state == AssertState::Lowest || ad.state == AssertState::LowestEnd)
        
        if (level < ad.level - 1) {
            AIPSTACK_ASSERT_FORCE(!ac(n).link[0].isNull() && !ac(n).link[1].isNull())
        }
        else if (level == ad.level - 1) {
            switch (ad.state) {
                case AssertState::Lowest:
                    if (ac(n).link[0].isNull()) {
                        ad.state = AssertState::LowestEnd;
                        AIPSTACK_ASSERT_FORCE(ac(n).link[1].isNull())
                        AIPSTACK_ASSERT_FORCE(ad.prev_leaf == m_last)
                    } else {
                        if (ac(n).link[1].isNull()) {
                            ad.state = AssertState::LowestEnd;
                            AIPSTACK_ASSERT_FORCE(ad.prev_leaf == m_last)
                        }
                    }
                    break;
                case AssertState::LowestEnd:
                    AIPSTACK_ASSERT_FORCE(ac(n).link[0].isNull() && ac(n).link[1].isNull())
                    break;
                default:
                    AIPSTACK_ASSERT(false);
            }
        }
        else if (level == ad.level) {
            AIPSTACK_ASSERT_FORCE(ad.state == AssertState::Lowest)
            AIPSTACK_ASSERT_FORCE(ac(n).link[0].isNull() && ac(n).link[1].isNull())
            ad.prev_leaf = n.link(st);
        }
        else {
            AIPSTACK_ASSERT_FORCE(false)
        }
    }
};

struct LinkedHeapService {
    template <typename LinkModel>
    using Node = LinkedHeapNode<LinkModel>;
    
    template <typename Accessor, typename Compare, typename LinkModel>
    using Structure = LinkedHeap<Accessor, Compare, LinkModel>;
};

}

#endif

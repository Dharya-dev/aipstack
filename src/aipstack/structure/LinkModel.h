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

#ifndef AIPSTACK_LINK_MODEL_H
#define AIPSTACK_LINK_MODEL_H

#include <cstddef>

#include <aipstack/misc/Assert.h>
#include <aipstack/misc/MinMax.h>

namespace AIpStack {

/**
 * @file
 * 
 * A Link Model defines the model how an intrusive data structure
 * is linked together. Currently, two link models are predefined:
 * using pointers (PointerLinkModel), and by array indices (ArrayLinkModel).
 * 
 * The link model defines the following types:
 * 
 * - Link. This is what is used in the data structure for links
 *   to objects. For example, a pointer or an array index.
 *   The ref() function of Link returns a Ref corresponding to the
 *   linked object. Null links must be supported (Link::null(),
 *   Link::isNull()).
 * 
 * - Ref. This is intended for referencing objects in the short
 *   term while working with the data structure. From the Ref a
 *   C++ reference to the object can be obtained (operator*)
 *   as well as the corresponding Link (link()). A Ref can be
 *   be null.
 * 
 * - State. The state is passed to all data structure manipulation
 *   functions and forwarded to calls of the Link::ref(). The
 *   PointerLinkModel does not use this, but the ArrayLinkModel uses
 *   a State object which must be initialized with the pointer to
 *   the base of the array, so that Link::ref() can work.
 */

class PointerLinkModelState {
public:
    inline PointerLinkModelState () {}
    
    // Convenience constructor that allows using the same way as
    // with ArrayLinkModel, it ignores the argument.
    template<typename T>
    inline PointerLinkModelState (T const &) {}
};

/**
 * Pointer link model for intrusive data structures.
 * 
 * The data structure will be linked using pointers.
 * The State type is defined as an empty class with a
 * default constructor, as it is not really needed.
 */
template<typename Entry>
class PointerLinkModel {
public:
    using State = PointerLinkModelState;
    
    class Ref;
    
    class Link {
        friend PointerLinkModel;
        
        inline Link(Entry *ptr)
        : m_ptr(ptr) {}
        
    public:
        Link() = default;
        
        inline static Link null ()
        {
            return Link(nullptr);
        }
        
        inline bool isNull () const
        {
            return m_ptr == nullptr;
        }
        
        inline Ref ref (State) const
        {
            return Ref(*this);
        }
        
        inline bool operator== (Link const &other) const
        {
            return m_ptr == other.m_ptr;
        }
        
    private:
        Entry *m_ptr;
    };
    
    class Ref {
        friend PointerLinkModel;
        
        inline Ref(Link link)
        : m_link(link) {}
        
    public:
        inline static Ref null ()
        {
            return Ref(Link::null());
        }
        
        Ref() = default;
        
        inline bool isNull () const
        {
            return m_link.isNull();
        }
        
        inline Link link (State) const
        {
            return m_link;
        }
        
        inline Entry & operator* () const
        {
            return *m_link.m_ptr;
        }
        
        inline operator Entry * () const
        {
            return m_link.m_ptr;
        }
        
        inline bool operator== (Ref const &other) const
        {
            return m_link == other.m_link;
        }
        
        // The remaining functions are only for users of data structures.
        
        inline Ref (Entry &entry)
        : m_link(Link(&entry))
        {}
        
        inline Ref (Entry &entry, State)
        : Ref(entry)
        {}
        
    private:
        Link m_link;
    };
};

template<
    typename Entry,
    typename ArrayContainer,
    typename ArrayAccessor
>
class ArrayLinkModelAccessorState {
public:
    ArrayLinkModelAccessorState() = delete;
    
    inline ArrayLinkModelAccessorState (ArrayContainer &container)
    : m_array(&ArrayAccessor::access(container)[0]) {}
    
    inline Entry & getEntryAt (std::size_t index)
    {
        return m_array[index];
    }
    
    inline std::size_t getEntryIndex (Entry &entry)
    {
        return std::size_t(&entry - m_array);
    }
    
private:
    Entry *m_array;
};

/**
 * Array index link model for intrusive data structures.
 * 
 * The data structure will be linked using array incides.
 * This can only work when all elements are contained in
 * the same array.
 * 
 * The IndexType template parameter is the integer type to
 * be used for array indices. The NullIndex is the value to
 * be used for null links. For example, if IndexType is
 * signed, -1 would be a good choice for NullIndex.
 * 
 * To create a Ref for an object (e.g. when inserting into
 * the data structure), use the Ref(Entry &, IndexType)
 * constructor, with the reference to the entry and its
 * array index.
 */
template<
    typename Entry,
    typename IndexType,
    IndexType NullIndex,
    typename State_
>
class ArrayLinkModel {
public:
    using State = State_;
    
    class Ref;
    
    class Link {
        friend ArrayLinkModel;
        
        inline Link(IndexType index)
        : m_index(index) {}
        
    public:
        Link() = default;
        
        inline static Link null ()
        {
            return Link(NullIndex);
        }
        
        inline bool isNull () const
        {
            return m_index == NullIndex;
        }
        
        inline Ref ref (State state) const
        {
            return Ref(isNull() ? nullptr : &state.getEntryAt(m_index));
        }
        
        inline bool operator== (Link const &other) const
        {
            return m_index == other.m_index;
        }
        
    private:
        IndexType m_index;
    };
    
    class Ref {
        friend ArrayLinkModel;
        
        inline Ref (Entry *ptr)
        : m_ptr(ptr) {}
        
    public:
        inline static Ref null ()
        {
            return Ref(nullptr);
        }
        
        Ref() = default;
        
        inline bool isNull () const
        {
            return m_ptr == nullptr;
        }
        
        inline Link link (State state) const
        {
            return Link(getIndex(state));
        }
        
        inline Entry & operator* () const
        {
            return *m_ptr;
        }
        
        inline operator Entry * () const
        {
            return m_ptr;
        }
        
        inline bool operator== (Ref const &other) const
        {
            return m_ptr == other.m_ptr;
        }
        
        // The remaining functions are only for users of data structures.
        
        inline Ref (Entry &entry)
        : m_ptr(&entry)
        {}
        
        inline Ref (Entry &entry, State)
        : Ref(entry)
        {}
        
        inline IndexType getIndex (State state) const
        {
            if (isNull()) {
                return NullIndex;
            } else {
                auto index = state.getEntryIndex(*m_ptr);
                AIPSTACK_ASSERT(index >= 0);
                AIPSTACK_ASSERT(index <= TypeMax<IndexType>);
                return IndexType(index);
            }
        }
        
    private:
        Entry *m_ptr;
    };
};

/**
 * Shortcut for ArrayLinkModel with ArrayLinkModelAccessorState.
 */
template<
    typename Entry,
    typename IndexType,
    IndexType NullIndex,
    typename ArrayContainer,
    typename ArrayAccessor
>
using ArrayLinkModelWithAccessor = ArrayLinkModel<Entry, IndexType, NullIndex,
    ArrayLinkModelAccessorState<Entry, ArrayContainer, ArrayAccessor>>;

}

#endif

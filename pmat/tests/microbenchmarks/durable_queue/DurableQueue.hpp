#pragma once

#include <cstdint>
#include <cstdbool>
#include <vector>
#include <iostream>
#include <set>
#include <atomic>
#include <cassert>
#include <thread>
#include <random>
#include "HazardPointers.hpp"

#define DQ_MAX_THREADS 16

namespace DurableQueue
{
    namespace Memory
    {
        namespace DWord 
        {
            typedef struct uint128 {
                uint64_t lo;
                uint64_t hi;

                uint128() : lo(0), hi(0) {}

                uint64_t get_ptr(void) { return lo; }
                uint64_t get_seq(void) { return hi; }
                void set_ptr(uint64_t uptr) { lo = uptr; }
                void set_seq(uint64_t uptr) { hi = uptr; }
            } uint128_t;

            // src: Address of a 16-byte aligned address or else General Protection Fault (GPF)
            // cmp: Expected value
            // with: New value to replace old
            // Returns: If successful or not
            static inline int cas128bit(uint128_t *src, uint128_t *cmp, uint128_t *with) {
                char result;
                __asm__ __volatile__ ("lock; cmpxchg16b (%6);"
                    "setz %7; "
                    : "=a" (cmp->lo),
                    "=d" (cmp->hi)
                    : "0" (cmp->lo),
                    "1" (cmp->hi),
                    "b" (with->lo),
                    "c" (with->hi),
                    "r" (src),
                    "m" (result)
                    : "cc", "memory"); 
                return result;
            }

            static inline int cas128bit_special(uint128_t *src, uint128_t *cmp, uint128_t *with) {
                with->hi = cmp->hi + 1;
                return cas128bit(src, cmp, with);
            }

            static inline void write128bit(void *srcvp, void *valvp) {
                uint128_t __attribute__ ((aligned (16))) with_val = * static_cast<uint128_t *>(valvp);
                uint128_t __attribute__ ((aligned (16))) cmp_val = * static_cast<uint128_t *>(srcvp);
                uint128_t *src = static_cast<uint128_t *>(srcvp);
                uint128_t *cmp = &cmp_val;
                uint128_t *with = &with_val;
                while (!cas128bit(src, cmp, with)) ;
            }

            // Special-case which will update the ABA count of valvp to be
            // one plus the srcvp. This is needed as ABA count needs to be monotonically
            // increasing.
            static inline void write128bit_special(void *srcvp, void *valvp) {
                uint128_t __attribute__ ((aligned (16))) with_val = * static_cast<uint128_t *>(valvp);
                uint128_t __attribute__ ((aligned (16))) cmp_val = * static_cast<uint128_t *>(srcvp);
                uint128_t *src = static_cast<uint128_t *>(srcvp);
                uint128_t *cmp = &cmp_val;
                uint128_t *with = &with_val;

                with->hi = cmp->hi + 1;
                while (!cas128bit(src, cmp, with)) {
                    with->hi = cmp->hi + 1;
                }
            }

            // srcvp: Address of a 16-byte aligned address or else General Protection Fault (GPF)
            // valvp: New value to replace the old
            // retvalp: Stores the old value
            static inline void exchange128bit(void *srcvp, void *valvp, void *retvalvp) {
                uint128_t __attribute__ ((aligned (16))) with_val = * static_cast<uint128_t *>(valvp);
                uint128_t __attribute__ ((aligned (16))) cmp_val = * static_cast<uint128_t *>(srcvp);
                uint128_t *src = static_cast<uint128_t *>(srcvp);
                uint128_t *cmp = &cmp_val;
                uint128_t *with = &with_val;
                while (!cas128bit(src, cmp, with)) ;
                *(uint128_t *) retvalvp = cmp_val; 
            }

            // Special-case which will update the ABA count of valvp to be
            // one plus the srcvp. This is needed as ABA count needs to be monotonically
            // increasing.
            static inline void exchange128bit_special(void *srcvp, void *valvp, void *retvalvp) {
                uint128_t __attribute__ ((aligned (16))) with_val = * static_cast<uint128_t *>(valvp);
                uint128_t __attribute__ ((aligned (16))) cmp_val = * static_cast<uint128_t *>(srcvp);
                uint128_t *src = static_cast<uint128_t *>(srcvp);
                uint128_t *cmp = &cmp_val;
                uint128_t *with = &with_val;
                
                with->hi = cmp->hi + 1;
                while (!cas128bit(src, cmp, with)) {
                    with->hi = cmp->hi + 1;
                }
                *(uint128_t *) retvalvp = cmp_val; 
            }
            
            static inline void read128bit(void *srcvp, void *dstvp) {
                uint128_t __attribute__ ((aligned (16))) src_val = * static_cast<uint128_t *>(srcvp);
                uint128_t __attribute__ ((aligned (16))) cmp_val = src_val;
                uint128_t __attribute__ ((aligned (16))) with_val = src_val;
                uint128_t *src = static_cast<uint128_t *>(srcvp);
                uint128_t *cmp = &cmp_val;
                uint128_t *with = &with_val;
                cas128bit(src, cmp, with);
                * static_cast<uint128_t *>(dstvp) = cmp_val;
            }

            template <class T>
            class ABA {
                public:
                    ABA() : ptr() {}
                    ABA(uint128_t u128) : ptr(u128) {}
                    ABA(T* t) {
                        ptr.set_seq(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<void *>(t))));
                    }
                    T& operator*() { return *reinterpret_cast<T*>(ptr.get_ptr()); }
                    T* operator->() { return reinterpret_cast<T*>(ptr.get_ptr()); }
                    // operator T*() { return reinterpret_cast<T*>(ptr.get_ptr()); }
                    // operator T() { return *reinterpret_cast<T*>(ptr.get_ptr()); }
                    uint64_t get_seq() { return ptr.get_seq(); }
                    T *to_ptr() { return reinterpret_cast<T*>(ptr.get_ptr()); }
                    uint128_t& to_raw_ptr() { return ptr; }

                    bool operator==(ABA<T> other) {
                        return this->ptr.lo == other.ptr.lo && this->ptr.hi == other.ptr.hi;
                    }

                    bool operator!=(ABA<T> other) {
                        return !operator==(other);
                    }
                private:
                    uint128_t __attribute__ ((aligned (16))) ptr;
            };

            // Anti-ABA pointer; wraps pointer in 128-bit word.
            template <class T>
            class ABAPtr {
                public:
                    bool CAS(ABA<T>& expected, ABA<T>& with) {
                        uint128_t& abaRef = aba.to_raw_ptr();
                        uint128_t& expectedRef = expected.to_raw_ptr();
                        uint128_t& withRef = with.to_raw_ptr();
                        return DWord::cas128bit(&abaRef, &expectedRef, &withRef);
                    }

                    bool CAS(ABA<T>& expected, T *with) {
                        uint128_t& abaRef = aba.to_raw_ptr();
                        uint128_t& expectedRef = expected.to_raw_ptr();
                        uint128_t withRef;
                        withRef.set_ptr(reinterpret_cast<uint64_t>(with));
                        withRef.set_seq(aba.get_seq() + 1);
                        return DWord::cas128bit(&abaRef, &expectedRef, &withRef);

                    }

                    void store(ABA<T>& value) {
                        uint128_t& abaRef = aba.to_raw_ptr();
                        uint128_t& valueRef = value.to_raw_ptr();
                        DWord::write128bit(&abaRef, &valueRef);
                    }

                    void store(T *value) {
                        uint128_t& abaRef = aba.to_raw_ptr();
                        uint128_t valueRef;
                        valueRef.set_ptr(reinterpret_cast<uint64_t>(value));
                        DWord::write128bit_special(&abaRef, &valueRef);
                    }

                    ABA<T> load() {
                        uint128_t& abaRef = aba.to_raw_ptr();
                        ABA<T> aba;
                        DWord::read128bit(&abaRef, &aba);
                        return aba;
                    }

                    void operator=(ABA<T>& other) {
                        store(other);
                    }

                    void operator=(T *other) {
                        uint128_t& abaRef = aba.to_raw_ptr();
                        uint128_t valueRef;
                        valueRef.set_ptr(reinterpret_cast<uint64_t>(other));
                        DWord::write128bit_special(&abaRef, &valueRef);
                    }

                    template <class R>
                    friend std::ostream& operator<<(std::ostream& os, ABA<R> aba);
                private:
                    ABA<T> aba;
            };
            template <class T>
            std::ostream& operator<<(std::ostream& os, ABA<T> aba) {
                os << "(" << aba.get_seq() << ", " << aba.to_ptr() << ")";
                return os;
            }
        }

        template <class T, T NilT = T()>
        class FreeListNode {
            public:
                FreeListNode(T t) : obj(t), next(nullptr) {}
                FreeListNode() : obj(NilT), next(nullptr) {}
                
                template <class R>
                friend std::ostream& operator<<(std::ostream& os, FreeListNode<R>*  freeList);
            
                T obj;
                FreeListNode<T> *next;
        };

        template <class T>
        std::ostream& operator<<(std::ostream& os, FreeListNode<T>* freeList) {
            if (freeList == nullptr) {
                os << "[NULL]";
            } else {
                os << "(" << freeList->obj << ", " << reinterpret_cast<void *>(freeList->next) << ")";
            }
            return os;
        }

        // FreeList is a trieber stack which used tagged/counted pointers.
        // Note: No new nodes will be created (for now...)
        // TODO: Build custom allocator for fun!
        template <class T, T NilT = T()>
        class FreeList {
            public:
                void push(T t) {
                    FreeListNode<T> *node = new FreeListNode<T>(t);
                    Memory::DWord::ABA<FreeListNode<T>> _head;
                    do {
                        _head = head.load();
                        node->next = _head.to_ptr();
                    } while(!head.CAS(_head, node));
                }

                T pop() {
                    Memory::DWord::ABA<FreeListNode<T>> _head;
                    FreeListNode<T> *next;
                    do {
                        _head = head.load();
                        if (_head.to_ptr() == nullptr) {
                            return NilT;
                        }
                        next = _head->next;
                    } while(!head.CAS(_head, next));
                    return _head->obj; // Note this leaks!
                }

            private:
               Memory::DWord::ABAPtr<FreeListNode<T>> head;
        };
    }
    
    template <class T, T NilT = T()>
    class DurableQueueNode {
        public:
            DurableQueueNode() : obj(NilT), deqThreadID(-1) {}
            DurableQueueNode(T& t) : obj(t), deqThreadID(-1) {}

            template <class R, R NilR>
            friend std::ostream& operator<<(std::ostream& os, DurableQueueNode<R, NilR> *node);

            T obj;
            Memory::DWord::ABAPtr<DurableQueueNode<T, NilT>> next;
            std::atomic<int> deqThreadID;
    };

    template <class T, T NilT>
    std::ostream& operator<<(std::ostream& os, DurableQueueNode<T, NilT> *node) {
        if (node == nullptr) {
            os << "[NULL]";
        } else {
            os << "(.obj=" << node->obj << ", .next=" << reinterpret_cast<void *>(node->next.load().to_ptr()) << ", .deqThreadID=" << node->deqThreadID << ")";
        }
        return os;
    }

    template <class T, T EmptyT, T NilT = T()>
    class DurableQueue {
        public:
            // Heap is leftover persistent memory region that we
            // are utilizing to create nodes from.
            DurableQueue(uint8_t *heap, size_t sz) {
                this->heap = heap;
                this->freeList = new Memory::FreeList<DurableQueueNode<T, NilT> *>();
                this->allocList = new std::vector<DurableQueueNode<T, NilT> *>();

                // Initialize allocList
                int allocSz = 0;
                while (allocSz + sizeof(DurableQueueNode<T, NilT>) < sz) {
                    this->allocList->push_back(new (heap + allocSz) DurableQueueNode<T, NilT>());
                    allocSz += sizeof(DurableQueueNode<T, NilT>);
                }
                
                // Initialize freeList
                for (auto node : *allocList) {
                    freeList->push(node);
                }
                
                // Initialize return value array
                for (T& retval : retvals) {
                    retval = NilT;
                }

                // Initialize head and tail with dummy node
                T tmp = NilT;
                DurableQueueNode<T, NilT> *dummy = freeList->pop();
                dummy->obj = NilT;
                this->head.store(dummy);
                this->tail.store(dummy);

                this->hazardPtrs = new HazardPointers<DurableQueueNode<T, NilT>>(
                    [this](DurableQueueNode<T, NilT> *node) { this->freeList->push(node); }, DQ_MAX_THREADS
                );
            }

            size_t size() {
                return head.load().get_seq() - tail.load().get_seq();
            }

            bool enqueue(T t, int tid) {
                DurableQueueNode<T, NilT> *node = freeList->pop();
                if (!node) return false;
                node->deqThreadID = -1;
                node->next = nullptr;
                node->obj = t;
                

                // Randomized backoff...
                int backoffTime = 1;
                int timesBackedOff = 0;
                while (true) {
                    Memory::DWord::ABA<DurableQueueNode<T, NilT>> last = tail.load();
                    hazardPtrs->protectPtr(0, last.to_ptr(), tid);
                    assert(last.to_ptr() != nullptr);
                    Memory::DWord::ABA<DurableQueueNode<T, NilT>> next = last.to_ptr()->next.load();
                    if (last == tail.load()) {
                        if (next.to_ptr() == nullptr) {
                            if (last.to_ptr()->next.CAS(next, node)) {
                                tail.CAS(last, node);
                                return true;
                            }
                        } else {
                            assert(last.to_ptr() != next.to_ptr());
                            tail.CAS(last, next.to_ptr());
                        }
                    }

                    std::chrono::nanoseconds backoff(std::rand() % backoffTime);
                    std::this_thread::sleep_for(backoff);
                    backoff *= 2;
                    timesBackedOff++;
                    if (timesBackedOff > 10) {
                        std::cerr << "Thread " << std::this_thread::get_id() << " has backed off " << timesBackedOff << " times!" << std::endl;
                        std::cerr << "Tail = " << tail.load() << ", last == " << last << "..." << std::endl;
                        // sanity();
                    }
                }

                return true;
            }

            // Sanity checking...
            void sanity() {
                auto node = this->freeList->pop();
                std::set<DurableQueueNode<T, NilT>*> s;
                while (node) {
                    if (s.count(node)) {
                        assert(false && "BAD! Found same node twice!");
                    }
                    s.insert(node);
                    node = this->freeList->pop();
                }
                for (auto n : s) this->freeList->push(n);
            }

            T dequeue(int tid) {
                retvals[tid] = NilT;

                while(true) {
                    Memory::DWord::ABA<DurableQueueNode<T, NilT>> _head = head.load();
                    hazardPtrs->protectPtr(0, _head.to_ptr(), tid);
                    Memory::DWord::ABA<DurableQueueNode<T, NilT>> _tail = tail.load();
                    if (head.load() != _head) {
                        continue;
                    }

                    Memory::DWord::ABA<DurableQueueNode<T, NilT>> next = _head.to_ptr()->next.load();
                    hazardPtrs->protectPtr(1, next.to_ptr(), tid);
                    if (head.load() != _head) {
                        std::this_thread::yield();
                        continue;
                    }
                    if (_head == _tail) {
                        if (next.to_ptr() == nullptr) {
                            retvals[tid] = EmptyT;
                            hazardPtrs->clearOne(0, tid);
                            hazardPtrs->clearOne(1, tid);
                            return EmptyT;
                        } else {
                            tail.CAS(_tail, next.to_ptr());
                        }
                    } else {
                        T retval = next.to_ptr()->obj;
                        int expectedID = -1;
                        if (next.to_ptr()->deqThreadID.compare_exchange_strong(expectedID, tid)) {
                            retvals[tid] = retval;
                            head.CAS(_head, next.to_ptr());head.CAS(_head, next.to_ptr());
                            hazardPtrs->retire(_head.to_ptr(), tid);
                            // hazardPtrs->clearOne(1, tid);
                            return retval;
                        } else {
                            assert(expectedID != -1);
                            T& address = retvals[expectedID];
                            if (head.load() == _head) {
                                address = retval;
                                head.CAS(_head, next.to_ptr());
                            }
                        }
                    }
                    std::this_thread::yield();
                }
            }
            
            template <class R, R EmptyR, R NilR>
            friend std::ostream& operator<<(std::ostream& os, DurableQueue<R, EmptyR, NilR> *dq);

        private: 
            // Persistent head of queue
            Memory::DWord::ABAPtr<DurableQueueNode<T, NilT>> head;
            // Persistent tail of queue
            Memory::DWord::ABAPtr<DurableQueueNode<T, NilT>> tail;
            // Persistent array of return values for threads
            T retvals[DQ_MAX_THREADS];
            // Transient remaining portion of heap
            uint8_t *heap;
            // Transient FreeList
            Memory::FreeList<DurableQueueNode<T, NilT>*> *freeList;
            // Transient AllocList
            std::vector<DurableQueueNode<T, NilT>*>* allocList;
            // Transient HazardPointers
            HazardPointers<DurableQueueNode<T, NilT>>* hazardPtrs;          
    };

    // Note: Not thread safe...
    template <class T, T EmptyT, T NilT>
    std::ostream& operator<<(std::ostream& os, DurableQueue<T, EmptyT, NilT> *dq) {
        DurableQueueNode<T, NilT> *head = dq->head.load().to_ptr();
        DurableQueueNode<T, NilT> *tail = dq->tail.load().to_ptr();
        os << "{ ";
        while (head != tail) {
            os << head->next.load().to_ptr() << " ";
            head = head->next.load().to_ptr();
        }
        os << "}";
        return os;
    }

    template <class T, T EmptyT, T NilT = T()>
    static DurableQueue<T, EmptyT, NilT> *alloc(uint8_t *heap, size_t sz) {
        return new (heap) DurableQueue<T, EmptyT, NilT>(heap + sizeof(DurableQueue<T, EmptyT, NilT>), sz - sizeof(DurableQueue<T, EmptyT, NilT>));
    }
}
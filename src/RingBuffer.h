/**************************************************************************/
/*  ring_buffer.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

// Single-producer/single-consumer variant of the engine's ring_buffer.h — DO NOT
// RE-SYNC BLINDLY. write()/space_left() are producer-side, read()/advance_read()/
// clear() consumer-side; copy()/find()/data_left() are conservative snapshots usable
// from either. resize() is setup-only and NOT concurrency-safe.

#include "godot_cpp/templates/local_vector.hpp"

#include <atomic>

namespace godot
{
    template <typename T>
    class RingBuffer {
        LocalVector<T> data;
        std::atomic<int> read_pos = 0;
        std::atomic<int> write_pos = 0;
        int size_mask = 0;

        inline int mask(int p_pos) const {
            return p_pos & size_mask;
        }

    public:
        // ── Consumer side ────────────────────────────────────────────────────
        T read() {
            ERR_FAIL_COND_V(data_left() < 1, T());
            int pos = read_pos.load(std::memory_order_relaxed);
            T ret = data.ptr()[pos];
            read_pos.store(mask(pos + 1), std::memory_order_release);
            return ret;
        }

        int read(T *p_buf, int p_size, bool p_advance = true) {
            int left = data_left();
            p_size = MIN(left, p_size);
            int start = read_pos.load(std::memory_order_relaxed);
            int pos = start;
            int to_read = p_size;
            int dst = 0;
            while (to_read) {
                int end = pos + to_read;
                end = MIN(end, size());
                int total = end - pos;
                const T *read = data.ptr();
                for (int i = 0; i < total; i++) {
                    p_buf[dst++] = read[pos + i];
                }
                to_read -= total;
                pos = 0;
            }
            if (p_advance) {
                read_pos.store(mask(start + p_size), std::memory_order_release);
            }
            return p_size;
        }

        inline int advance_read(int p_n) {
            p_n = MIN(p_n, data_left());
            int pos = read_pos.load(std::memory_order_relaxed);
            read_pos.store(mask(pos + p_n), std::memory_order_release);
            return p_n;
        }

        // Drops everything pending. Safe while the producer is writing.
        inline void clear() {
            read_pos.store(write_pos.load(std::memory_order_acquire), std::memory_order_release);
        }

        // ── Producer side ────────────────────────────────────────────────────
        Error write(const T &p_v) {
            ERR_FAIL_COND_V(space_left() < 1, FAILED);
            int pos = write_pos.load(std::memory_order_relaxed);
            data[pos] = p_v;
            write_pos.store(mask(pos + 1), std::memory_order_release);
            return OK;
        }

        int write(const T *p_buf, int p_size) {
            int left = space_left();
            p_size = MIN(left, p_size);

            int start = write_pos.load(std::memory_order_relaxed);
            int pos = start;
            int to_write = p_size;
            int src = 0;
            while (to_write) {
                int end = pos + to_write;
                end = MIN(end, size());
                int total = end - pos;

                for (int i = 0; i < total; i++) {
                    data[pos + i] = p_buf[src++];
                }
                to_write -= total;
                pos = 0;
            }

            write_pos.store(mask(start + p_size), std::memory_order_release);
            return p_size;
        }

        inline int decrease_write(int p_n) {
            p_n = MIN(p_n, data_left());
            int pos = write_pos.load(std::memory_order_relaxed);
            write_pos.store(mask(pos + size_mask + 1 - p_n), std::memory_order_release);
            return p_n;
        }

        // ── Read-only snapshots (safe from either side) ───────────────────────
        int copy(T *p_buf, int p_offset, int p_size) const {
            int left = data_left();
            if ((p_offset + p_size) > left) {
                p_size -= left - p_offset;
                if (p_size <= 0) {
                    return 0;
                }
            }
            p_size = MIN(left, p_size);
            int pos = mask(read_pos.load(std::memory_order_relaxed) + p_offset);
            int to_read = p_size;
            int dst = 0;
            while (to_read) {
                int end = pos + to_read;
                end = MIN(end, size());
                int total = end - pos;
                for (int i = 0; i < total; i++) {
                    p_buf[dst++] = data[pos + i];
                }
                to_read -= total;
                pos = 0;
            }
            return p_size;
        }

        int find(const T &t, int p_offset, int p_max_size) const {
            int left = data_left();
            if ((p_offset + p_max_size) > left) {
                p_max_size -= left - p_offset;
                if (p_max_size <= 0) {
                    return 0;
                }
            }
            p_max_size = MIN(left, p_max_size);
            int pos = mask(read_pos.load(std::memory_order_relaxed) + p_offset);
            int to_read = p_max_size;
            while (to_read) {
                int end = pos + to_read;
                end = MIN(end, size());
                int total = end - pos;
                for (int i = 0; i < total; i++) {
                    if (data[pos + i] == t) {
                        return i + (p_max_size - to_read);
                    }
                }
                to_read -= total;
                pos = 0;
            }
            return -1;
        }

        inline int space_left() const {
            int left = read_pos.load(std::memory_order_acquire) - write_pos.load(std::memory_order_relaxed);
            if (left < 0) {
                return size() + left - 1;
            }
            if (left == 0) {
                return size() - 1;
            }
            return left - 1;
        }

        inline int data_left() const {
            int left = write_pos.load(std::memory_order_acquire) - read_pos.load(std::memory_order_relaxed);
            if (left < 0) {
                left += size();
            }
            return left;
        }

        inline int size() const {
            return data.size();
        }

        // Setup only: reallocates, so must run before the ring is shared.
        void resize(int p_power) {
            int old_size = size();
            int new_size = 1 << p_power;
            int new_mask = new_size - 1;
            data.resize(int64_t(1) << int64_t(p_power));
            int r = read_pos.load(std::memory_order_relaxed);
            int w = write_pos.load(std::memory_order_relaxed);
            if (old_size < new_size && r > w) {
                for (int i = 0; i < w; i++) {
                    data[(old_size + i) & new_mask] = data[i];
                }
                w = (old_size + w) & new_mask;
            } else {
                r = r & new_mask;
                w = w & new_mask;
            }

            size_mask = new_mask;
            read_pos.store(r, std::memory_order_relaxed);
            write_pos.store(w, std::memory_order_relaxed);
        }

        RingBuffer(int p_power = 0) {
            resize(p_power);
        }
    };
}

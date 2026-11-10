// Corundum HFT by Euclase: C++17 wrapper over corundum.h.
// RAII endpoints, typed in-place writes and typed zero-copy reads.
// The hot path stays inline and allocation-free; only setup throws.
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Euclase
#ifndef CORUNDUM_HPP
#define CORUNDUM_HPP

#include "corundum.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace corundum {

class error : public std::runtime_error {
public:
    explicit error(crd_status s, const std::string &what)
        : std::runtime_error(what + ": " + crd_strerror(s)), status_(s) {}
    crd_status status() const noexcept { return status_; }

private:
    crd_status status_;
};

inline void check(crd_status s, const std::string &what) {
    if (s < 0) throw error(s, what);
}

/// Creates a ring segment. Throws corundum::error on failure.
inline void create(const std::string &name, std::uint64_t capacity, unsigned mode = 0600, unsigned flags = 0) {
    check(crd_create(name.c_str(), capacity, mode, flags), "crd_create(" + name + ")");
}

/// Removes a ring name. Returns false if it did not exist.
inline bool unlink(const std::string &name) noexcept { return crd_unlink(name.c_str()) == CRD_OK; }

template <class T>
constexpr bool is_wire_type_v = std::is_trivially_copyable<T>::value && alignof(T) <= CRD_RECORD_ALIGN;

/// A zero-copy view of one record in shared memory, valid until Reader::release().
class Message {
public:
    Message() = default;
    explicit Message(const crd_msg &m) noexcept : m_(m) {}

    const void *data() const noexcept { return m_.data; }
    std::uint32_t size() const noexcept { return m_.size; }
    std::uint32_t type() const noexcept { return m_.type; }

    /// Typed in-place view, or nullptr if the size does not match sizeof(T).
    template <class T>
    const T *as() const noexcept {
        static_assert(is_wire_type_v<T>, "T must be trivially copyable with alignment <= 8");
        return m_.size == sizeof(T) ? static_cast<const T *>(m_.data) : nullptr;
    }

private:
    crd_msg m_{};
};

/// The single producer of a ring.
class Writer {
public:
    Writer() noexcept { std::memset(&w_, 0, sizeof w_); }
    Writer(const std::string &name, unsigned flags = 0) : Writer() {
        check(crd_writer_open(&w_, name.c_str(), flags), "crd_writer_open(" + name + ")");
    }
    ~Writer() { crd_writer_close(&w_); }
    Writer(Writer &&o) noexcept : w_(o.w_) { std::memset(&o.w_, 0, sizeof o.w_); }
    Writer &operator=(Writer &&o) noexcept {
        if (this != &o) {
            crd_writer_close(&w_);
            w_ = o.w_;
            std::memset(&o.w_, 0, sizeof o.w_);
        }
        return *this;
    }
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;

    explicit operator bool() const noexcept { return w_.shm != nullptr; }
    std::uint32_t max_payload() const noexcept { return crd_max_payload(&w_); }

    /// Reserves `size` bytes; returns nullptr if the ring is full or size is too big.
    void *reserve(std::uint32_t size) noexcept {
        void *p = nullptr;
        return crd_reserve(&w_, size, &p) == CRD_OK ? p : nullptr;
    }
    /// Publishes the reservation with its final size and type.
    bool commit(std::uint32_t size, std::uint32_t type) noexcept { return crd_commit(&w_, size, type) == CRD_OK; }
    /// Like commit, but stays invisible until publish(): batch several records.
    bool stage(std::uint32_t size, std::uint32_t type) noexcept { return crd_stage(&w_, size, type) == CRD_OK; }
    void publish() noexcept { crd_publish(&w_); }
    /// Wakes a reader sleeping in Reader::wait().
    void notify() noexcept { crd_notify(&w_); }

    /// Copies raw bytes in. Returns false if full (or too big: check max_payload()).
    bool try_write(std::uint32_t type, const void *src, std::uint32_t size) noexcept {
        return crd_write(&w_, type, src, size) == CRD_OK;
    }

    /// Copies a trivially copyable value in.
    template <class T>
    bool try_write(std::uint32_t type, const T &value) noexcept {
        static_assert(is_wire_type_v<T>, "T must be trivially copyable with alignment <= 8");
        return try_write(type, &value, static_cast<std::uint32_t>(sizeof(T)));
    }

    /// Constructs T directly inside shared memory: no intermediate copy.
    template <class T, class... Args>
    bool try_emplace(std::uint32_t type, Args &&...args) noexcept(std::is_nothrow_constructible<T, Args...>::value) {
        static_assert(is_wire_type_v<T>, "T must be trivially copyable with alignment <= 8");
        void *p = reserve(static_cast<std::uint32_t>(sizeof(T)));
        if (!p) return false;
        if constexpr (std::is_aggregate<T>::value)
            ::new (p) T{std::forward<Args>(args)...}; // aggregates: brace init (C++17 has no paren init)
        else
            ::new (p) T(std::forward<Args>(args)...);
        return commit(static_cast<std::uint32_t>(sizeof(T)), type);
    }

    crd_writer *native() noexcept { return &w_; }

private:
    crd_writer w_;
};

/// The single consumer of a ring.
class Reader {
public:
    enum class wait_result { ready, timeout, closed };

    Reader() noexcept { std::memset(&r_, 0, sizeof r_); }
    Reader(const std::string &name, unsigned flags = 0) : Reader() {
        check(crd_reader_open(&r_, name.c_str(), flags), "crd_reader_open(" + name + ")");
    }
    ~Reader() { crd_reader_close(&r_); }
    Reader(Reader &&o) noexcept : r_(o.r_) { std::memset(&o.r_, 0, sizeof o.r_); }
    Reader &operator=(Reader &&o) noexcept {
        if (this != &o) {
            crd_reader_close(&r_);
            r_ = o.r_;
            std::memset(&o.r_, 0, sizeof o.r_);
        }
        return *this;
    }
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;

    explicit operator bool() const noexcept { return r_.shm != nullptr; }

    /// Reads the next record into `out`. Returns false when the ring is empty.
    /// Throws corundum::error if the shared state is corrupt.
    bool try_read(Message &out) {
        crd_msg m;
        crd_status s = crd_read(&r_, &m);
        if (s == CRD_OK) {
            out = Message(m);
            return true;
        }
        if (s == CRD_EMPTY) return false;
        throw error(s, "crd_read");
    }

    /// Returns the space of all records read so far to the writer.
    void release() noexcept { crd_release(&r_); }

    /// Sleeps until data arrives, the writer closes, or the timeout passes.
    template <class Rep, class Period>
    wait_result wait(std::chrono::duration<Rep, Period> timeout) {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count();
        return to_result(crd_wait(&r_, static_cast<std::int64_t>(ns)));
    }
    wait_result wait() { return to_result(crd_wait(&r_, -1)); }

    bool writer_closed() const noexcept { return crd_writer_closed(&r_) != 0; }
    crd_reader *native() noexcept { return &r_; }

private:
    static wait_result to_result(crd_status s) {
        switch (s) {
        case CRD_OK: return wait_result::ready;
        case CRD_TIMEOUT: return wait_result::timeout;
        case CRD_CLOSED: return wait_result::closed;
        default: throw error(s, "crd_wait");
        }
    }
    crd_reader r_;
};

} // namespace corundum

#endif // CORUNDUM_HPP

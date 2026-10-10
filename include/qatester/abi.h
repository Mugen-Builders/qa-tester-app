// Minimal, bounds-checked reader/writer for the subset of the Solidity ABI the tester uses.
// Written in-house so that malformed payloads can never read out of bounds and so that bytes[]
// (batch) is supported, which libcmt's helpers do not cover.
#pragma once

#include "types.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace qatester::abi {

[[nodiscard]] uint32_t selector(std::string_view signature); // keccak256(signature)[0..4], big-endian

class Reader {
public:
    explicit Reader(bytes_view buf) : m_buf(buf) {}
    [[nodiscard]] bool read_selector(uint32_t &out);      // first 4 bytes
    [[nodiscard]] bool word(U256 &out);                   // next 32-byte head word
    [[nodiscard]] bool uint_fits(uint64_t &out, unsigned bits); // word that must fit `bits`
    [[nodiscard]] bool address(Address &out);             // 12 zero bytes + 20
    [[nodiscard]] bool dyn_bytes(bytes_view &out);        // head offset -> tail bytes
    [[nodiscard]] bool dyn_bytes_array(std::vector<bytes_view> &out, size_t max_items);
    [[nodiscard]] bool done() const;                      // head fully consumed
    [[nodiscard]] size_t position() const { return m_pos; }

private:
    [[nodiscard]] bool word_at(size_t at, U256 &out) const;
    [[nodiscard]] bool bytes_at(size_t frame, size_t offset, bytes_view &out) const;
    bytes_view m_buf;
    size_t m_pos{0};   // cursor in the head
    size_t m_frame{0}; // start of the parameter frame (after the selector)
};

class Writer {
public:
    void selector(uint32_t sel);
    void word(const U256 &v);
    void uint(uint64_t v);
    void address(const Address &a);
    void dyn_bytes(bytes_view data); // appended lazily: call finish() to resolve offsets
    [[nodiscard]] bytes finish();

private:
    bytes m_head;
    std::vector<std::pair<size_t, bytes>> m_tail; // (head position of the offset word, data)
};

} // namespace qatester::abi

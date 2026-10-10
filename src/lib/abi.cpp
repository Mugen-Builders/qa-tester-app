#include "qatester/abi.h"

extern "C" {
#include <libcmt/keccak.h>
}

#include <algorithm>
#include <cstring>

namespace qatester::abi {

uint32_t selector(std::string_view signature) {
    uint8_t md[CMT_KECCAK_LENGTH];
    cmt_keccak_t st;
    cmt_keccak_init(&st);
    cmt_keccak_update(&st, signature.size(), signature.data());
    cmt_keccak_final(&st, md);
    return (uint32_t(md[0]) << 24) | (uint32_t(md[1]) << 16) | (uint32_t(md[2]) << 8) | uint32_t(md[3]);
}

bool Reader::read_selector(uint32_t &out) {
    if (m_buf.size() < 4) {
        return false;
    }
    out = (uint32_t(m_buf[0]) << 24) | (uint32_t(m_buf[1]) << 16) | (uint32_t(m_buf[2]) << 8) | uint32_t(m_buf[3]);
    m_pos = 4;
    m_frame = 4;
    return true;
}

bool Reader::word_at(size_t at, U256 &out) const {
    if (at > m_buf.size() || m_buf.size() - at < 32) {
        return false;
    }
    std::copy_n(m_buf.begin() + static_cast<std::ptrdiff_t>(at), 32, out.b.begin());
    return true;
}

bool Reader::word(U256 &out) {
    if (!word_at(m_pos, out)) {
        return false;
    }
    m_pos += 32;
    return true;
}

bool Reader::uint_fits(uint64_t &out, unsigned bits) {
    U256 w;
    if (!word(w) || !w.fits_u64()) {
        return false;
    }
    uint64_t v = w.to_u64();
    if (bits < 64 && (v >> bits) != 0) {
        return false;
    }
    out = v;
    return true;
}

bool Reader::address(Address &out) {
    U256 w;
    if (!word(w)) {
        return false;
    }
    if (!std::all_of(w.b.begin(), w.b.begin() + 12, [](uint8_t x) { return x == 0; })) {
        return false;
    }
    std::copy(w.b.begin() + 12, w.b.end(), out.b.begin());
    return true;
}

bool Reader::bytes_at(size_t frame, size_t offset, bytes_view &out) const {
    if (offset > m_buf.size() - frame) {
        return false;
    }
    size_t at = frame + offset;
    U256 len_w;
    if (!word_at(at, len_w) || !len_w.fits_u64()) {
        return false;
    }
    uint64_t len = len_w.to_u64();
    size_t data_at = at + 32;
    if (len > m_buf.size() - data_at) {
        return false;
    }
    out = m_buf.subspan(data_at, static_cast<size_t>(len));
    return true;
}

bool Reader::dyn_bytes(bytes_view &out) {
    uint64_t offset = 0;
    if (!uint_fits(offset, 64)) {
        return false;
    }
    if (offset > m_buf.size()) {
        return false;
    }
    return bytes_at(m_frame, static_cast<size_t>(offset), out);
}

bool Reader::dyn_bytes_array(std::vector<bytes_view> &out, size_t max_items) {
    uint64_t offset = 0;
    if (!uint_fits(offset, 64) || offset > m_buf.size() - m_frame) {
        return false;
    }
    size_t arr = m_frame + static_cast<size_t>(offset);
    U256 n_w;
    if (!word_at(arr, n_w) || !n_w.fits_u64()) {
        return false;
    }
    uint64_t n = n_w.to_u64();
    if (n > max_items) {
        return false;
    }
    size_t elems = arr + 32; // element offsets are relative to the start of the element area
    out.clear();
    for (uint64_t i = 0; i < n; ++i) {
        U256 off_w;
        if (!word_at(elems + static_cast<size_t>(i) * 32, off_w) || !off_w.fits_u64()) {
            return false;
        }
        uint64_t off = off_w.to_u64();
        if (off > m_buf.size()) {
            return false;
        }
        bytes_view item;
        if (!bytes_at(elems, static_cast<size_t>(off), item)) {
            return false;
        }
        out.push_back(item);
    }
    return true;
}

bool Reader::done() const {
    return m_pos <= m_buf.size();
}

void Writer::selector(uint32_t sel) {
    m_head.push_back(static_cast<uint8_t>(sel >> 24));
    m_head.push_back(static_cast<uint8_t>(sel >> 16));
    m_head.push_back(static_cast<uint8_t>(sel >> 8));
    m_head.push_back(static_cast<uint8_t>(sel));
}

void Writer::word(const U256 &v) {
    m_head.insert(m_head.end(), v.b.begin(), v.b.end());
}

void Writer::uint(uint64_t v) {
    word(U256::from_u64(v));
}

void Writer::address(const Address &a) {
    U256 w;
    std::copy(a.b.begin(), a.b.end(), w.b.begin() + 12);
    word(w);
}

void Writer::dyn_bytes(bytes_view data) {
    m_tail.emplace_back(m_head.size(), bytes(data.begin(), data.end()));
    word(U256{}); // placeholder for the offset
}

bytes Writer::finish() {
    size_t frame = (m_head.size() >= 4 && (m_head.size() - 4) % 32 == 0 && !m_head.empty() && m_tail.empty())
        ? 4
        : 4; // the selector is always present in this writer
    bytes out = m_head;
    for (auto &[pos, data] : m_tail) {
        U256 off = U256::from_u64(out.size() - frame);
        std::copy(off.b.begin(), off.b.end(), out.begin() + static_cast<std::ptrdiff_t>(pos));
        U256 len = U256::from_u64(data.size());
        out.insert(out.end(), len.b.begin(), len.b.end());
        out.insert(out.end(), data.begin(), data.end());
        size_t pad = (32 - data.size() % 32) % 32;
        out.insert(out.end(), pad, 0);
    }
    return out;
}

} // namespace qatester::abi

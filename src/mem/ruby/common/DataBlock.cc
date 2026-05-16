/*
 * Copyright (c) 2021 ARM Limited
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 1999-2008 Mark D. Hill and David A. Wood
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/ruby/common/DataBlock.hh"

#include <cstdint>
#include <cstring>

#include "mem/ruby/common/Address.hh"
#include "mem/ruby/common/WriteMask.hh"

namespace gem5
{

namespace ruby
{

DataBlock::DataBlock(const DataBlock &cp)
{
    assert(cp.isAlloc());
    assert(cp.getBlockSize() > 0);
    assert(!m_alloc);

    uint8_t *block_update;
    m_block_size = cp.getBlockSize();
    m_data = new uint8_t[m_block_size];
    memcpy(m_data, cp.m_data, m_block_size);
    m_alloc = true;
    // If this data block is involved in an atomic operation, the effect
    // of applying the atomic operations on the data block are recorded in
    // m_atomicLog. If so, we must copy over every entry in the change log
    for (size_t i = 0; i < cp.m_atomicLog.size(); i++) {
        block_update = new uint8_t[m_block_size];
        memcpy(block_update, cp.m_atomicLog[i], m_block_size);
        m_atomicLog.push_back(block_update);
    }
}

void
DataBlock::alloc()
{
    assert(!m_alloc);

    if (!m_block_size) {
        return;
    }

    m_data = new uint8_t[m_block_size];
    m_alloc = true;
    clear();
}

void
DataBlock::realloc(int blk_size)
{
    m_block_size = blk_size;
    assert(m_block_size > 0);

    if (m_alloc) {
        delete [] m_data;
        m_alloc = false;
    }
    alloc();
}

void
DataBlock::clear()
{
    assert(m_alloc);
    assert(m_block_size > 0);
    memset(m_data, 0, m_block_size);
}

bool
DataBlock::equal(const DataBlock& obj) const
{
    assert(m_alloc);
    assert(m_block_size > 0);
    size_t block_bytes = m_block_size;
    // Check that the block contents match
    if (memcmp(m_data, obj.m_data, block_bytes)) {
        return false;
    }
    if (m_atomicLog.size() != obj.m_atomicLog.size()) {
        return false;
    }
    for (size_t i = 0; i < m_atomicLog.size(); i++) {
        if (memcmp(m_atomicLog[i], obj.m_atomicLog[i], block_bytes)) {
            return false;
        }
    }
    return true;
}

void
DataBlock::copyPartial(const DataBlock &dblk, const WriteMask &mask)
{
    assert(m_alloc);
    assert(m_block_size > 0);
    for (int i = 0; i < m_block_size; i++) {
        if (mask.getMask(i, 1)) {
            m_data[i] = dblk.m_data[i];
        }
    }
}

void
DataBlock::atomicPartial(const DataBlock &dblk, const WriteMask &mask,
        bool isAtomicNoReturn)
{
    assert(m_alloc);
    assert(m_block_size > 0);
    for (int i = 0; i < m_block_size; i++) {
        m_data[i] = dblk.m_data[i];
    }
    mask.performAtomic(m_data, m_atomicLog, isAtomicNoReturn);
}

void
DataBlock::print(std::ostream& out) const
{
    assert(m_alloc);
    assert(m_block_size > 0);
    int size = m_block_size;
    out << "[ ";
    for (int i = 0; i < size; i++) {
        out << std::setw(2) << std::setfill('0') << std::hex
            << "0x" << (int)m_data[i] << " " << std::setfill(' ');
    }
    out << std::dec << "]" << std::flush;
}

int
DataBlock::numAtomicLogEntries() const
{
    return m_atomicLog.size();
}
uint8_t*
DataBlock::popAtomicLogEntryFront()
{
    assert(m_atomicLog.size() > 0);
    auto ret = m_atomicLog.front();
    m_atomicLog.pop_front();
    return ret;
}
void
DataBlock::clearAtomicLogEntries()
{
    assert(m_alloc);
    for (auto log : m_atomicLog) {
        delete [] log;
    }
    m_atomicLog.clear();
}

const uint8_t*
DataBlock::getData(int offset, int len) const
{
    assert(m_alloc);
    assert(m_block_size > 0);
    assert(offset + len <= m_block_size);
    return &m_data[offset];
}

uint8_t*
DataBlock::getDataMod(int offset)
{
    assert(m_alloc);
    return &m_data[offset];
}

void
DataBlock::setData(const uint8_t *data, int offset, int len)
{
    assert(m_alloc);
    memcpy(&m_data[offset], data, len);
}

void
DataBlock::setData(PacketPtr pkt)
{
    assert(m_alloc);
    assert(m_block_size > 0);
    int offset = getOffset(pkt->getAddr(), floorLog2(m_block_size));
    assert(offset + pkt->getSize() <= m_block_size);
    pkt->writeData(&m_data[offset]);
}

DataBlock &
DataBlock::operator=(const DataBlock & obj)
{
    // Reallocate if needed
    if (m_alloc && m_block_size != obj.getBlockSize()) {
        delete [] m_data;
        m_block_size = obj.getBlockSize();
        alloc();
    } else if (!m_alloc) {
        m_block_size = obj.getBlockSize();
        alloc();

        // Assume this will be realloc'd later if zero.
        if (m_block_size == 0) {
            return *this;
        }
    } else {
        assert(m_alloc && m_block_size == obj.getBlockSize());
    }
    assert(m_block_size > 0);

    uint8_t *block_update;
    size_t block_bytes = m_block_size;
    // Copy entire block contents from obj to current block
    memcpy(m_data, obj.m_data, block_bytes);
    // If this data block is involved in an atomic operation, the effect
    // of applying the atomic operations on the data block are recorded in
    // m_atomicLog. If so, we must copy over every entry in the change log
    for (size_t i = 0; i < obj.m_atomicLog.size(); i++) {
        block_update = new uint8_t[block_bytes];
        memcpy(block_update, obj.m_atomicLog[i], block_bytes);
        m_atomicLog.push_back(block_update);
    }
    return *this;
}

void
DataBlock::networkAtomicOp(int chiSubOp, const DataBlock& operand,
                            int byteOffset)
{
    // The CVA6 RTL places the atomic operand at the sub-word position within
    // the FIRST 8 bytes of the NCBWrData flit (replicated across both 32-bit
    // slots for .w accesses), not at byteOffset within the cache line.
    // sub_off = byteOffset & 7 selects the correct 32-bit slot:
    //   sub_off=0 → 8-byte-aligned access → 64-bit operation from operand[0..7]
    //   sub_off=4 → .w in high slot → 32-bit operation from operand[4..7]
    int sub_off = byteOffset & 7;

    if (sub_off != 0) {
        // 32-bit access at a non-8-byte-aligned offset (e.g. amoadd.w at byte 52).
        // Operand is in the high 32-bit slot (bytes 4-7) of the first 8 bytes.
        uint32_t src32 = 0, dst32 = 0;
        int word_off = byteOffset & ~3;
        std::memcpy(&src32, operand.getData(sub_off, 4), sizeof(src32));
        std::memcpy(&dst32, getData(word_off, 4), sizeof(dst32));
        uint32_t result32 = dst32;
        switch (chiSubOp) {
        case 0x28: case 0x30: result32 = dst32 + src32; break;
        case 0x29: case 0x31: result32 = dst32 & ~src32; break;
        case 0x2a: case 0x32: result32 = dst32 ^ src32; break;
        case 0x2b: case 0x33: result32 = dst32 | src32; break;
        case 0x2c: case 0x34:
            if (static_cast<int32_t>(src32) > static_cast<int32_t>(dst32))
                result32 = src32;
            break;
        case 0x2d: case 0x35:
            if (static_cast<int32_t>(src32) < static_cast<int32_t>(dst32))
                result32 = src32;
            break;
        case 0x2e: case 0x36: if (src32 > dst32) result32 = src32; break;
        case 0x2f: case 0x37: if (src32 < dst32) result32 = src32; break;
        case 0x38: result32 = src32; break;
        default: break;
        }
        setData(reinterpret_cast<const uint8_t*>(&result32), word_off, 4);
    } else {
        // 8-byte-aligned access: operand is in bytes 0-7 of the flit.
        int alignedOff = byteOffset & ~7;
        uint64_t src = 0, dst = 0;
        std::memcpy(&src, operand.getData(0, 8), sizeof(src));
        std::memcpy(&dst, getData(alignedOff, 8), sizeof(dst));
        uint64_t result = dst;
        switch (chiSubOp) {
        case 0x28: case 0x30: result = dst + src; break;
        case 0x29: case 0x31: result = dst & ~src; break;
        case 0x2a: case 0x32: result = dst ^ src; break;
        case 0x2b: case 0x33: result = dst | src; break;
        case 0x2c: case 0x34:
            if (static_cast<int64_t>(src) > static_cast<int64_t>(dst)) result = src;
            break;
        case 0x2d: case 0x35:
            if (static_cast<int64_t>(src) < static_cast<int64_t>(dst)) result = src;
            break;
        case 0x2e: case 0x36: if (src > dst) result = src; break;
        case 0x2f: case 0x37: if (src < dst) result = src; break;
        case 0x38: result = src; break;
        default: break;
        }
        setData(reinterpret_cast<const uint8_t*>(&result), alignedOff, 8);
    }
}

} // namespace ruby
} // namespace gem5

/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CPU_INTERVAL_BRANCH_MODEL_HH__
#define __CPU_INTERVAL_BRANCH_MODEL_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

#include "base/types.hh"

namespace gem5
{

/**
 * O3's default branch predictor (BaseO3CPU.branchPred) for a trace whose
 * branch outcomes are known: TournamentBP, a 4096-entry direct-mapped
 * SimpleBTB, a 16-entry ReturnAddrStack and SimpleIndirectPredictor, sized
 * and indexed as gem5 configures them for X86O3CPU (instShiftAmt 0).
 *
 * A trace has no wrong path, so the speculative history updates gem5 makes
 * at prediction and repairs at a squash reduce to updating with the actual
 * outcome. The counters, as in gem5, train only when the branch commits
 * (commit()), so predictions made meanwhile see the older state; the BTB and
 * the indirect target table are written when a misprediction is resolved.
 */
class BranchModel
{
  public:
    enum Kind : uint8_t
    {
        None = 0, Cond = 1, Jump = 2, Call = 3, IndCall = 4, Return = 5,
        IndJump = 6
    };

    /** What predict() decided, kept until the branch commits. */
    struct Pending
    {
        uint64_t seq = 0;
        Addr pc = 0;
        bool cond = false;
        bool taken = false;
        bool localPred = false, globalPred = false;
        unsigned globalHist = 0;   ///< global history before this branch
        unsigned localIdx = 0;     ///< local predictor counter index
    };

    struct Result
    {
        bool mispredict = false;
        bool btbMiss = false;      ///< taken, target not provided
        /** gem5's decode recomputes PC-relative targets: a direct jump or
         *  call, or a direct conditional predicted taken, with a wrong
         *  target is redirected from decode instead of execute. */
        bool decodeFix = false;
        Addr predNext = 0;         ///< where fetch went (the wrong path)
    };

    BranchModel()
    {
        localHist.fill(0);
        localCtrs.fill(0);    // gem5's SatCounter8 starts at 0; 2-bit: 0..3
        globalCtrs.fill(0);
        choiceCtrs.fill(0);
        for (auto &b : btb)
            b = {false, 0, 0};
        for (auto &s : ind)
            for (auto &w : s)
                w = {false, 0, 0, 0};
    }

    /** Predict the branch at pc (length len); actual outcome and target
     *  from the trace. */
    Result
    predict(uint64_t seq, Addr pc, unsigned len, Kind kind, bool taken,
            Addr target)
    {
        Result r;
        Pending p;
        p.seq = seq;
        p.pc = pc;
        p.cond = kind == Cond;
        p.taken = taken;
        p.globalHist = ghist;

        // Direction: TournamentBP::lookup for conditional branches only.
        bool pred_taken = true;
        if (p.cond) {
            const unsigned lh_idx = pc & (LocalHistSize - 1);
            p.localIdx = localHist[lh_idx] & (LocalSize - 1);
            p.localPred = localCtrs[p.localIdx] > 1;
            p.globalPred = globalCtrs[ghist & (GlobalSize - 1)] > 1;
            const bool use_global = choiceCtrs[ghist & (ChoiceSize - 1)] > 1;
            pred_taken = use_global ? p.globalPred : p.localPred;
        } else {
            p.localPred = p.globalPred = true;
        }

        // Target: BTB, then RAS for returns, indirect table for indirect
        // jumps and calls. No target means fall through (not taken).
        bool have_target = false;
        Addr pred_target = 0;
        const BtbEntry &b = btb[pc & (BtbSize - 1)];
        const bool btb_hit = b.valid && b.tag == (pc >> BtbBits);
        if (btb_hit && pred_taken) {
            have_target = true;
            pred_target = b.target;
        }
        if (kind == Call || kind == IndCall) {
            ras[rasTop] = pc + len;
            rasTop = (rasTop + 1) % RasSize;
            if (rasUsed < RasSize)
                ++rasUsed;
        } else if (kind == Return) {
            if (rasUsed) {
                rasTop = (rasTop + RasSize - 1) % RasSize;
                --rasUsed;
                have_target = true;
                pred_target = ras[rasTop];
            }
        }
        unsigned ind_set = 0;
        const bool indirect = kind == IndCall || kind == IndJump;
        if (indirect && pred_taken) {
            ind_set = indSet(pc);
            for (const auto &w : ind[ind_set]) {
                if (w.valid && w.tag == (pc & 0xffff)) {
                    have_target = true;
                    pred_target = w.target;
                    break;
                }
            }
        } else if (indirect) {
            ind_set = indSet(pc);
        }
        if (!have_target)
            pred_taken = false;

        r.mispredict = pred_taken != taken ||
                       (taken && pred_target != target);
        r.predNext = pred_taken ? pred_target : pc + len;
        r.btbMiss = r.mispredict && taken && !btb_hit;
        const bool direct = kind == Cond || kind == Jump || kind == Call;
        r.decodeFix = r.mispredict && direct && taken &&
                      (kind != Cond || pred_taken);

        // Histories with the actual outcome (gem5's speculative update,
        // repaired on the squash). Local history: conditional only.
        if (p.cond) {
            const unsigned lh_idx = pc & (LocalHistSize - 1);
            localHist[lh_idx] = (localHist[lh_idx] << 1) | taken;
        }
        ghist = ((ghist << 1) | taken) & (GlobalSize - 1);
        indGhr = ((indGhr << 1) | taken) & ((1u << 13) - 1);

        // Resolution of a mispredict: BTB and indirect target learn.
        if (r.mispredict && taken) {
            BtbEntry &w = btb[pc & (BtbSize - 1)];
            w = {true, pc >> BtbBits, target};
            if (indirect)
                indRecord(ind_set, pc, target);
        }
        if (indirect && taken) {
            path.push_back(target);
            if (path.size() > PathLen)
                path.pop_front();
        }

        pending.push_back(p);
        return r;
    }

    /** The branch with this sequence number committed: train counters. */
    void
    commit(uint64_t seq)
    {
        while (!pending.empty() && pending.front().seq <= seq) {
            const Pending p = pending.front();
            pending.pop_front();
            if (p.seq != seq)
                continue;          // should not happen: commits in order
            // TournamentBP::update
            if (p.cond && p.localPred != p.globalPred) {
                uint8_t &c = choiceCtrs[p.globalHist & (ChoiceSize - 1)];
                if (p.localPred == p.taken) {
                    if (c) --c;
                } else if (p.globalPred == p.taken) {
                    if (c < 3) ++c;
                }
            }
            uint8_t &g = globalCtrs[p.globalHist & (GlobalSize - 1)];
            if (p.taken) {
                if (g < 3) ++g;
                if (p.cond && localCtrs[p.localIdx] < 3)
                    ++localCtrs[p.localIdx];
            } else {
                if (g) --g;
                if (p.cond && localCtrs[p.localIdx])
                    --localCtrs[p.localIdx];
            }
        }
    }

  private:
    static constexpr unsigned LocalHistSize = 2048;   // localHistoryTableSize
    static constexpr unsigned LocalSize = 2048;       // localPredictorSize
    static constexpr unsigned GlobalSize = 8192;      // globalPredictorSize
    static constexpr unsigned ChoiceSize = 8192;      // choicePredictorSize
    static constexpr unsigned BtbSize = 4096, BtbBits = 12;
    static constexpr unsigned RasSize = 16;
    static constexpr unsigned IndSets = 256, IndWays = 2, PathLen = 3;

    struct BtbEntry { bool valid; Addr tag; Addr target; };
    struct IndEntry { bool valid; Addr tag; Addr target; uint64_t lru; };

    unsigned
    indSet(Addr pc) const
    {
        // SimpleIndirectPredictor::getSetIndex: pc ^ ghr ^ path targets
        Addr h = pc ^ indGhr;
        const unsigned shift = 8 / PathLen;   // floorLog2(numSets)/pathLen
        unsigned p = 0;
        for (auto it = path.rbegin(); it != path.rend() && p < PathLen;
             ++it, ++p)
            h ^= *it >> (p * shift);
        return h & (IndSets - 1);
    }

    void
    indRecord(unsigned set, Addr pc, Addr target)
    {
        auto &s = ind[set];
        IndEntry *victim = &s[0];
        for (auto &w : s) {
            if (w.valid && w.tag == (pc & 0xffff)) {
                victim = &w;
                break;
            }
            if (!w.valid || w.lru < victim->lru)
                victim = &w;
        }
        *victim = {true, pc & 0xffff, target, ++indClock};
    }

    std::array<unsigned, LocalHistSize> localHist;
    std::array<uint8_t, LocalSize> localCtrs;
    std::array<uint8_t, GlobalSize> globalCtrs;
    std::array<uint8_t, ChoiceSize> choiceCtrs;
    unsigned ghist = 0;
    std::array<BtbEntry, BtbSize> btb;
    std::array<Addr, RasSize> ras{};
    unsigned rasTop = 0, rasUsed = 0;
    std::array<std::array<IndEntry, IndWays>, IndSets> ind;
    unsigned indGhr = 0;
    uint64_t indClock = 0;
    std::deque<Addr> path;
    std::deque<Pending> pending;
};

} // namespace gem5

#endif // __CPU_INTERVAL_BRANCH_MODEL_HH__

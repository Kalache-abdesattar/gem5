/*
 * Periodic MSHR sampling for a CHI cache controller.
 *
 * In CHI every request a cache accepts gets a TBE, hits included. The TBEs
 * of requests that missed in this cache are the ones that act as MSHRs:
 * they stay allocated until the line arrives from downstream. A request
 * that arrives for a line with such a TBE outstanding is a secondary miss;
 * CHI does not merge it, it waits (stall_and_wait) until the TBE is
 * released and is then replayed.
 *
 * The protocol reports these events (missAllocated, missDeallocated,
 * secondaryMiss); every `period` controller cycles this samples
 *   - the request TBEs in use (hits and misses),
 *   - the outstanding misses (MSHRs in use),
 *   - the secondary misses seen since the previous sample (demand and
 *     prefetch requests counted apart),
 * and writes one row per sample to <controller>.mshr_samples.csv.gz in the
 * output directory, plus per-sample distributions to the stats.
 *
 * A period of 0 disables it (no event, no file, no stats).
 */

#ifndef __MEM_RUBY_STRUCTURES_MSHRSAMPLER_HH__
#define __MEM_RUBY_STRUCTURES_MSHRSAMPLER_HH__

#include <memory>

#include "base/output.hh"
#include "base/statistics.hh"
#include "sim/eventq.hh"

namespace gem5
{

namespace ruby
{

class AbstractController;
class TBEStorage;

class MSHRSampler
{
  public:
    MSHRSampler(AbstractController *ctrl, TBEStorage *req_tbes,
                int period_cycles);
    ~MSHRSampler();

    bool enabled() const { return period > 0; }

    // A request TBE's transaction missed in this cache.
    void missAllocated();
    // That TBE was released.
    void missDeallocated();
    // A request stalled behind an outstanding miss for the same line.
    void secondaryMiss(bool is_prefetch);

  private:
    void sample();
    void close();

    AbstractController *ctrl;
    TBEStorage *reqTBEs;
    const int period;

    int missesOutstanding = 0;
    int secondaryWindow = 0;
    int secondaryPfWindow = 0;

    EventFunctionWrapper sampleEvent;
    OutputStream *out = nullptr;

    struct SamplerStats : public statistics::Group
    {
        SamplerStats(statistics::Group *parent, int capacity);

        statistics::Scalar samples;
        statistics::Scalar primaryMisses;
        statistics::Scalar secondaryMisses;
        statistics::Scalar secondaryPfMisses;
        statistics::Scalar mshrSum;
        statistics::Distribution mshrsInUse;
        statistics::Distribution tbesInUse;
        statistics::Distribution secondaryPerSample;
        statistics::Formula avgMshrUtil;
    };
    std::unique_ptr<SamplerStats> stats;
};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_STRUCTURES_MSHRSAMPLER_HH__

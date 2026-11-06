#include "mem/ruby/structures/MSHRSampler.hh"

#include "base/logging.hh"
#include "mem/ruby/slicc_interface/AbstractController.hh"
#include "mem/ruby/structures/TBEStorage.hh"
#include "sim/core.hh"

namespace gem5
{

namespace ruby
{

MSHRSampler::MSHRSampler(AbstractController *_ctrl, TBEStorage *req_tbes,
                         int period_cycles)
    : ctrl(_ctrl), reqTBEs(req_tbes), period(period_cycles),
      sampleEvent([this]{ sample(); }, _ctrl->name() + ".mshrSampler")
{
    if (!enabled())
        return;
    fatal_if(period < 0, "%s: negative MSHR sample period", ctrl->name());

    stats = std::make_unique<SamplerStats>(ctrl, reqTBEs->capacity());

    out = simout.create(ctrl->name() + ".mshr_samples.csv.gz");
    std::ostream &os = *out->stream();
    os << "# " << ctrl->name() << ": one row every " << period
       << " controller cycles (" << ctrl->clockPeriod() * period
       << " ticks); request TBE capacity " << reqTBEs->capacity() << "\n"
       << "# tbes = request TBEs in use (hits and misses); mshrs = TBEs of"
          " requests that missed here (outstanding misses);\n"
       << "# secondary / secondary_pf = demand / prefetch requests that"
          " arrived for a line with an outstanding miss since the previous"
          " row\n"
       << "tick,tbes,mshrs,secondary,secondary_pf\n";

    ctrl->schedule(sampleEvent, ctrl->clockEdge(Cycles(period)));
    // gzip needs its trailer written: close the file when gem5 exits.
    registerExitCallback([this]{ close(); });
}

MSHRSampler::~MSHRSampler()
{
    close();
}

void
MSHRSampler::missAllocated()
{
    if (!enabled())
        return;
    ++missesOutstanding;
    ++stats->primaryMisses;
}

void
MSHRSampler::missDeallocated()
{
    if (!enabled())
        return;
    panic_if(missesOutstanding <= 0, "%s: MSHR count underflow",
             ctrl->name());
    --missesOutstanding;
}

void
MSHRSampler::secondaryMiss(bool is_prefetch)
{
    if (!enabled())
        return;
    if (is_prefetch) {
        ++secondaryPfWindow;
        ++stats->secondaryPfMisses;
    } else {
        ++secondaryWindow;
        ++stats->secondaryMisses;
    }
}

void
MSHRSampler::sample()
{
    int tbes = reqTBEs->size();
    *out->stream() << curTick() << ',' << tbes << ',' << missesOutstanding
                   << ',' << secondaryWindow << ',' << secondaryPfWindow
                   << '\n';
    ++stats->samples;
    stats->mshrSum += missesOutstanding;
    stats->mshrsInUse.sample(missesOutstanding);
    stats->tbesInUse.sample(tbes);
    stats->secondaryPerSample.sample(secondaryWindow + secondaryPfWindow);
    secondaryWindow = secondaryPfWindow = 0;

    ctrl->schedule(sampleEvent, ctrl->clockEdge(Cycles(period)));
}

void
MSHRSampler::close()
{
    if (out) {
        simout.close(out);
        out = nullptr;
    }
}

MSHRSampler::SamplerStats::SamplerStats(statistics::Group *parent,
                                        int capacity)
    : statistics::Group(parent, "mshrSampler"),
      ADD_STAT(samples, statistics::units::Count::get(),
               "Number of MSHR samples taken"),
      ADD_STAT(primaryMisses, statistics::units::Count::get(),
               "Requests that missed here and held a TBE as an MSHR"),
      ADD_STAT(secondaryMisses, statistics::units::Count::get(),
               "Demand requests that arrived for a line with an "
               "outstanding miss"),
      ADD_STAT(secondaryPfMisses, statistics::units::Count::get(),
               "Prefetch requests that arrived for a line with an "
               "outstanding miss"),
      ADD_STAT(mshrSum, statistics::units::Count::get(),
               "Sum of outstanding misses over all samples"),
      ADD_STAT(mshrsInUse, statistics::units::Count::get(),
               "Outstanding misses (MSHRs in use) per sample"),
      ADD_STAT(tbesInUse, statistics::units::Count::get(),
               "Request TBEs in use (hits and misses) per sample"),
      ADD_STAT(secondaryPerSample, statistics::units::Count::get(),
               "Secondary misses (demand + prefetch) per sample period"),
      ADD_STAT(avgMshrUtil, statistics::units::Ratio::get(),
               "Mean MSHR utilization (outstanding misses / TBE capacity)")
{
    mshrsInUse.init(0, capacity, 1);
    tbesInUse.init(0, capacity, 1);
    secondaryPerSample.init(0, 63, 1);
    avgMshrUtil = mshrSum / samples / capacity;
}

} // namespace ruby
} // namespace gem5

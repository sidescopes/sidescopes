#pragma once

namespace sidescopes::benchmark {

// A publication completed during shutdown belongs to both the final counters
// and the measured interval. Keep the join before every endpoint read.
template <typename Worker, typename Measure>
void stopAndMeasure(Worker& worker, const Measure& measure)
{
    worker.stop();
    measure();
}

}  // namespace sidescopes::benchmark

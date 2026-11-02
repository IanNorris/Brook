#pragma once
#include <stdint.h>
namespace brook {
// Queue observations, not audible-drop or producer-starvation measurements.
// Gap reservations include future guard space and overlap empty-tail advances.
struct HdaDellQueueDiagnostics {
    bool enabled=false,saturated=false;
    uint64_t starts=0,stops=0,faults=0,startupGapBytes=0,laterGapEvents=0,laterGapBytes=0;
    uint64_t emptyBeforeBytes=0,emptyAfterBytes=0,publishedBytes=0,backpressure=0,publicationFailures=0;
};
}

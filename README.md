# Weave — Stage 0

## Goal

Test whether a separate Java process can observe
another JVM through JMX.

## Experiment

Target JVM:
- creates short-lived allocations
- exposes JMX on port 9999

Observer JVM:
- connects through JMX
- discovers GarbageCollectorMXBeans
- repeatedly reads collection count and collection time

## Result

The Observer successfully observed changes in the
Target JVM's G1 Young Generation collection metrics.

Example:

G1 Young Generation
Collection count: 17 → 20 → 23 → ...
Collection time: 50 ms → 53 ms → 56 ms → ...

## What we learned

- JMX allows one JVM to observe another JVM.
- ManagementFactory without a remote connection accesses
  the local JVM.
- ManagementFactory.getPlatformMXBeans(connection, ...)
  accesses the remote JVM.
- Collection count means number of collection events,
  not number of objects collected.
- Collection time is cumulative reported collection time.
- GC metrics are cumulative counters.
- A workload causing allocation does not necessarily mean
  GC happens immediately.
- Short-lived allocation primarily produced young
  generation collections in this experiment.

## Next

Adversarial test:
What happens when the Target JVM becomes unavailable?
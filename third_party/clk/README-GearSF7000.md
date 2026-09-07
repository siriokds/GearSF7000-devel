# CLK Z80 subset

This directory contains the minimal Z80 processor subset imported from
[Clock Signal](https://github.com/TomHarte/CLK), revision
`096de57445920ecf16cf066979422e34aceb843a`.

Imported files:

- `ClockReceiver/ClockReceiver.hpp`
- `ClockReceiver/ForceInline.hpp`
- `Numeric/RegisterSizes.hpp`
- `Processors/Z80/Z80.hpp`
- `Processors/Z80/Implementation/Z80Storage.hpp`
- `Processors/Z80/Implementation/Z80Implementation.hpp`
- `Processors/Z80/Implementation/Z80Storage.cpp`
- `Processors/Z80/Implementation/Z80Base.cpp`
- `Processors/Z80/Implementation/PartialMachineCycle.cpp`

The optional reflection-based mid-instruction state layer is deliberately not
included yet.  GearSF7000 first integrates the bus contract while retaining
its current `Processor` as the active CPU.

The upstream code is licensed under the MIT licence in `LICENCE`.  Keep the
upstream copyright headers and this revision record when updating the subset.

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

## Local changes

Four of the imported files differ from the upstream revision above.  The
changes are Copyright (C) 2026 Saverio Russo and, like the rest of this
subset, are licensed under the MIT licence in `LICENCE`, so they can be offered
back to Clock Signal unchanged.

- `Processors/Z80/Z80.hpp`: `is_at_execution_boundary()`, which keeps an
  interrupt acknowledge separate from the preceding instruction when single
  stepping.
- `Processors/Z80/Implementation/Z80Implementation.hpp`: no zero-time state
  change from the next opcode once the time budget ends on an execution
  boundary; IM 0 advances PC normally after the supplied opcode byte; EI and DI
  discard the IRQ sample taken during their own fetch; P/V is cleared when an
  interrupt is accepted right after LD A,I or LD A,R (NMOS behaviour).
- `Processors/Z80/Implementation/Z80Storage.hpp`: the LD A,I/R latch, the
  `InstructionTableStorage` wrapper and the `GearSF7000StateAccess` friend used
  by mid-instruction save states.
- `Processors/Z80/Implementation/Z80Storage.cpp`: R is incremented by NMI and
  by every interrupt acknowledge, IM 0 gets its 2T refresh tail, and the
  instruction tables are built on the heap instead of the stack.

Comparing each file with `git show 096de574:<path>` in a Clock Signal checkout
shows the exact changes.

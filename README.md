---
title: Web-based RISC-V Instruction Set Simulation and Debugger

---

# Web-based RISC-V Instruction Set Simulation and Debugger
> 唐萱, 萬宸維

## Goals
* Enhance the [ARES project](https://github.com/ldlaur/ares) so it can execute more sophisticated RISC-V programs and support VGA peripheral output that is capable of rendering the nyancat animation, similar to the [MyCPU](https://github.com/sysprog21/ca2025-mycpu) project. Ensure that RISC-V instruction decoding and execution remain implemented in the C core under the [src/exec directory](https://github.com/ldlaur/ares/tree/master/src/exec).
* Remove the current RV32IM instruction limitations in ARES and extend the ISA coverage. Provide an MMIO interface that enables RISC-V programs to access UART and VGA peripherals in a manner consistent with hardware-style device interactions.
* Recognize that ARES is fundamentally a software-centric environment tailored for developers. It excels as a modern IDE and RISC-V debugger, combining a high-performance C/WASM execution engine with a SolidJS interface. To evolve ARES toward the functionality demonstrated by the reference project emulsiV, close the gap between simply running code and visualizing how hardware behaves.

- [ ] Implement a Visual Datapath Interface

Introduce a datapath visualization pane similar to [emulsiV](https://github.com/ESEO-Tech/emulsiV)'s animated block diagram.
* Render a simplified RV32IM datapath including the program counter, instruction memory, register file, ALU, data memory, multiplexer paths, and control signals.
* Animate the datapath to highlight active components during execution. For example, when executing an LW instruction, visually trace the flow from the ALU address calculation to data memory and then into the writeback multiplexer and register file.
* Expose internal control signals or micro-operations from the C core through the WASM boundary. Modify the core so each executed instruction returns control information that the frontend can bind to the visualization layer. Use SVG or canvas-based drawing to animate state transitions.

- [ ] Add Sub-Instruction Execution Granularity

Extend the simulator to present micro-steps for instructional phases such as Fetch, Decode, Execute, Memory, and Write Back.
* Introduce a Micro-Step mode that visually progresses through each stage of instruction processing even if the C core remains non-cycle-accurate.
* Use this staged animation to clarify pipeline concepts and instruction flow for learners.

- [ ] Provide Memory-Mapped I/O and Peripherals

Define a standard MMIO region (for example, 0xFFFF0000–0xFFFFFFFF) and attach peripherals to this space.
* Implement a bitmap display by mapping a memory region (such as 0xFF000000) to a drawable canvas.
* Add simple LED or seven-segment widgets mapped to specific bytes in memory.
* Implement a UART/console interface that writes characters to an output pane and receives keyboard input through a memory-backed buffer.
* The nyancat animation should be rendered to VGA peripheral through MMIO.

- [ ] Strengthen the C Core Back-End

Refactor the C execution kernel so it exposes richer execution state to the frontend.
* Modify the step function to return a structured record capturing the program counter, current instruction, operand values, and key control signals such as ALU source selection, memory access direction, and register write enable.
* Use this record to drive accurate frontend visualization and to support future tooling enhancements.

- [ ] Improve Educational Usability

Enhance ARES as a teaching platform.
* Display explanatory text when stepping through an instruction, describing its semantic effect using actual register values.
* Allow users to toggle between ABI register names (`sp`, `ra`, `t0`) and architectural names (`x2`, `x1`, `x5`).
* Provide a gallery of example programs such as a Hello-World UART demo, recursive factorial, and array copy tasks.

- [ ] Modernize Repository and Tooling

Add continuous-integration capabilities that compile the WASM backend and deploy the SolidJS frontend automatically.
* Configure a GitHub Action pipeline that builds the simulator and publishes the updated interface to GitHub Pages upon each push to the master branch.

See also:
* [Enhance visualized RISC-V simulation](https://hackmd.io/@sysprog/H1hH_y_Syx)
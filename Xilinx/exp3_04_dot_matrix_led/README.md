# exp3-04 dot-matrix baseline

This directory preserves the source used for the hardware baseline verified on
2026-09-17:

- XC7A100T configuration completes (`DONE=HIGH`) when the bitstream is loaded
  directly through JTAG into SRAM.
- With the STM32 diagnostic route forcing the original BSW mask `0x01B3`, F5
  lights most of the 16x16 matrix.
- Two rows in each left 8x8 tile remain dark.  This is a known unresolved
  hardware-route/mapping issue and must not be described as fully fixed.
- The STM32 diagnostic switch is `PANEL_FORCE_DOT_MATRIX_DIAG` in
  `STM32/Core/Src/board_panel.c`; it intentionally bypasses FMC/F10 mode
  reconciliation while isolating the matrix route.

Build with Vivado 2018.3:

```powershell
vivado.bat -mode batch -source build.tcl
```

`build.tcl` uses the repository-local `src` and `constraints` directories and
writes generated files to `output`.  Program SRAM with `program_sram.tcl` after
the Xilinx JTAG cable is connected and the bridge board is removed.

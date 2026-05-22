# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Half-bridge LLC converter firmware for STM32G474RET6 (Cortex-M4F, 170MHz, 512KB Flash, 128KB RAM). Currently in early bring-up phase — HRTIM PWM generation is configured and functional, UART printf works, but no closed-loop control exists yet.

## Build commands

```bash
# Configure (once after CMakeLists.txt changes)
cmake --preset Debug -B build/Debug

# Build
cmake --build build/Debug

# Clean build
cmake --build build/Debug --clean-first
```

Toolchain: `arm-none-eabi-gcc` (must be in PATH). CMake toolchain file at `cmake/gcc-arm-none-eabi.cmake`.

## CMake file roles — critical to understand

| File | Role | Editable? |
|---|---|---|
| `CMakeLists.txt` (root) | User code, `App/*.c` glob, user defines/libs/include-paths | **Yes — never regenerated** |
| `cmake/stm32cubemx/CMakeLists.txt` | CubeMX-generated sources, HAL drivers, MX defines | **No — regenerated every CubeMX code-gen** |
| `cmake/gcc-arm-none-eabi.cmake` | Compiler/linker flags, `-u _printf_float` | **Yes — rarely changed** |

**Rule**: Anything that must survive CubeMX regeneration goes into root `CMakeLists.txt` user sections (target_compile_definitions, target_include_directories, target_link_directories, target_link_libraries). Currently ARM_MATH defines, DSP include path, and DSP .a library link all live there.

## Code organization

```
Src/  Inc/   ← CubeMX territory (main.c, HAL config, syscalls, startup)
App/          ← User code (.c + .h). GLOB'd automatically by CMakeLists.txt
```

`App/*.c` is auto-discovered via `file(GLOB)`. Drop new `.c`/`.h` files there and they compile without touching any CMake file.

## Current App modules

- `io_retarget.c/.h` — `__io_putchar`/`__io_getchar` via USART1 (PC4/TX, PC5/RX, 115200-8N1)
- `driver.c/.h` — stub: `DRIVER_Run(ref, fb)` returns `ref - fb`
- `test.c/.h` — stub: `PID_Run(ref, fb)` returns `ref - fb`

## printf support

Float printing is enabled via `-Wl,-u,_printf_float` in `cmake/gcc-arm-none-eabi.cmake`. The output chain is:

```
printf → _write() [syscalls.c] → __io_putchar() [App/io_retarget.c] → HAL_UART_Transmit(&huart1, ...)
```

USART1 is initialized by CubeMX `MX_USART1_UART_Init()`. No extra init needed before printf.

## ARM_MATH / CMSIS-DSP

Library source: `X-CUBE-ALGOBUILD/1.4.0` pack in STM32Cube repository (external to project). Precompiled `libarm_cortexM4lf_math.a` (hard-float Cortex-M4F variant).

Defines: `ARM_MATH_CM4` + `ARM_MATH_LOOPUNROLL` (in root CMakeLists.txt, survives CubeMX regen).

## HRTIM configuration summary

- **Base clock**: 170MHz (HSE 8MHz → PLL ×85 /2)
- **Period**: 850 → PWM freq = 200kHz, period = 5µs
- **Prescaler**: MUL32 → 184ps resolution
- **Dead time**: MUL8, 340 counts rising/falling → ~250ns
- **Timer A** (PA8/PA9): Primary half-bridge. Outputs had dead-time, TA2 = complement of TA1 via DT insertion.
- **Timer C** (PB12/PB13): Secondary sync rect. Push-pull = disabled (Dead Time Insertion controls complement).
- **Repetition counter**: 3 → update events every 4th PWM cycle

## Git notes

`.settings/` and machine-specific files in `.vscode/` use `git update-index --skip-worktree` to ignore local changes. STM32CubeIDE extension modifies bundle store files based on local toolchain installation.

## Key constraints

- `core.autocrlf = true` (Windows Git). LF in repo, CRLF in working tree.
- CubeMX regenerates `cmake/stm32cubemx/CMakeLists.txt` with absolute paths containing `C:/Users/ywjAv`. These paths break on other machines.
- Heap: 512B, Stack: 1024B (in linker script). Tight — float printf may need more stack.

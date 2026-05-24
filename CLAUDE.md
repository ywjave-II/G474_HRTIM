# G474_HRTIM 工程实现总结

## 目标硬件

| 项目 | 参数 |
|---|---|
| MCU | STM32G474RET6 |
| 内核 | Cortex-M4F（硬件浮点 FPv4-SP-D16） |
| 主频 | 170 MHz（HSE 12 MHz → PLLM=3, PLLN=85, PLLR=2） |
| Flash | 512 KB |
| RAM | 128 KB（Heap 512 B，Stack 1024 B） |

应用场景：**半桥 LLC 谐振变换器**驱动固件，硬件 bring-up 已完成。

---

## 已实现的功能

### 1. 时钟系统配置

- 使用外部高速晶振（HSE **12 MHz**）作为 PLL 输入
- PLL 配置：PLLM=3，PLLN=85，PLLR=2，SYSCLK = 170 MHz
- 电压调节器工作在 `SCALE1_BOOST` 模式（支持 170 MHz 满速运行）
- Flash 延迟配置为 4 个等待周期（FLASH_LATENCY_4）

### 2. HRTIM 高分辨率 PWM 输出（核心功能）

**时基配置：**

| 参数 | 值 | 说明 |
|---|---|---|
| 预分频 | MUL32 | 等效时钟 5440 MHz，分辨率 ≈ 184 ps |
| 周期寄存器 | **27200** | PWM 频率 200 kHz（5440 MHz / 200 kHz） |
| 重复计数器 | 0 | 每周期更新 |
| 工作模式 | CONTINUOUS | Master + Timer A/C 均连续运行 |

**Timer A — 原边半桥驱动（PA8 / PA9）：**

- TA1（PA8）：`TIMPER` 置位（计数器复位后拉高），`CMP1=13600` 复位，占空比 50%
- TA2（PA9）：由死区模块硬件自动生成互补信号，**不配置 Set/Reset 源**
- Timer A 由 `MASTER_PER` 事件复位同步（`ResetTrigger = MASTER_PER`）

**Timer C — 副边同步整流驱动（PB12 / PB13）：**

- TC1（PB12）：`CMP1=13600` 置位，`CMP2=27199` 复位，与 TA1 形成 180° 移相
- TC2（PB13）：由死区模块硬件自动生成互补信号，**不配置 Set/Reset 源**
- 移相量通过调整 TC1 的 CMP1/CMP2 值控制

**死区配置（实测验证 250 ns）：**

| 参数 | 值 | 说明 |
|---|---|---|
| 预分频宏 | `HRTIM_TIMDEADTIME_PRESCALERRATIO_MUL8` | 死区时钟 = f_HRTIM × 8 = **1360 MHz** |
| 死区 tick | **0.735 ns**（= 1/1360 MHz） | 注意：基准是 f_HRTIM=170 MHz，非等效时钟 |
| RisingValue / FallingValue | **340** | 340 × 0.735 ns ≈ 250 ns |

> ⚠️ **关键陷阱**：死区模块时钟基准是 `f_HRTIM`（170 MHz），不是高分辨率等效时钟（5440 MHz）。
> `MUL8` 宏含义：死区时钟 = 170 MHz × 8 = 1360 MHz，tick ≈ 0.735 ns。

**死区插入规则：**
- 启用 `DeadTimeInsertion` 后，**互补通道（TA2/TC2）完全由死区硬件接管**
- 只需配置主通道（TA1/TC1）的 Set/Reset 源
- **不能**再对 TA2/TC2 调用 `HAL_HRTIM_WaveformOutputConfig`，否则会覆盖死区控制

**DLL 校准：** 启动时执行（`CALIBRATIONRATE_3`，超时 100 ms），保证高分辨率精度。

**输出启动顺序（main.c）：**
```c
HAL_HRTIM_WaveformOutputStart(&hhrtim1,
    HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 |
    HRTIM_OUTPUT_TC1 | HRTIM_OUTPUT_TC2);

HAL_HRTIM_WaveformCountStart(&hhrtim1,
    HRTIM_TIMERID_MASTER |
    HRTIM_TIMERID_TIMER_A |
    HRTIM_TIMERID_TIMER_C);
```

### 3. UART 调试串口（USART1）

- 引脚：PC4（TX）/ PC5（RX），波特率 115200-8N1
- 通过 `App/io_retarget.c` 将 `printf` / `scanf` 重定向到 USART1
- 支持浮点数打印：链接器选项 `-Wl,-u,_printf_float`

### 4. ARM CMSIS-DSP 数学库

- 库版本：X-CUBE-ALGOBUILD 1.4.0
- 预编译库：`libarm_cortexM4lf_math.a`（硬浮点 Cortex-M4F 专用）
- 编译宏：`ARM_MATH_CM4` + `ARM_MATH_LOOPUNROLL`

### 5. 开发环境

| 项目 | 说明 |
|---|---|
| 工具链 | `arm-none-eabi-gcc`，硬浮点 `-mfloat-abi=hard -mfpu=fpv4-sp-d16` |
| 构建 | CMake + Ninja，`Ctrl+Shift+B` 一键编译烧录 |
| 调试器 | DAPLink（CMSIS-DAP），OpenOCD `D:/openocd/daplink.cfg` |
| GDB | `C:/Users/ywjAv/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin` |
| 烧录 | tasks.json `Flash: OpenOCD`，编译后自动 program/verify/reset |
| 调试 | launch.json `STM32G474 Debug`，F5 启动 Cortex-Debug |

---

## 踩坑记录

### HRTIM Period 计算
- **错误**：Period = 850（对应频率 ~6.4 MHz，不是 200 kHz）
- **正确**：Period = f_HRTIM_等效 / f_PWM = 5440 MHz / 200 kHz = **27200**

### 死区时间计算
- **错误**：以为死区基准时钟是等效时钟（5440 MHz），算出 170 ticks
- **正确**：死区基准是 f_HRTIM = 170 MHz，MUL8 → 1360 MHz，需要 **340 ticks**

### 死区互补通道配置
- **错误**：启用死区后仍对 TA2/TC2 调用 `WaveformOutputConfig`（SetSource=NONE）
- **正确**：启用死区后 TA2/TC2 由硬件接管，**跳过** TA2/TC2 的输出配置

### Timer A/C 模式
- **错误**：Timer A/C 设为 `SINGLESHOT_RETRIGGERABLE`
- **正确**：应为 `CONTINUOUS`，靠 `ResetTrigger=MASTER_PER` 做周期同步

### TA1 置位源
- **错误**：`SetSource = MASTERPER`，与 `ResetTrigger=MASTER_PER` 同时触发冲突
- **正确**：`SetSource = TIMPER`（Timer A 自身周期事件，即复位后置位）

---

## 尚未实现的功能（占位桩）

| 模块 | 文件 | 当前状态 |
|---|---|---|
| 闭环控制驱动 | `App/driver.c` | `DRIVER_Run(ref, fb)` 仅返回 `ref - fb` |
| PID 控制器 | `App/test.c` | `PID_Run(ref, fb)` 仅返回 `ref - fb` |
| ADC 采样 | — | 未配置 |
| 保护逻辑 | — | HRTIM 故障输入未使用 |
| 闭环调节 | — | 主循环为空，无控制代码 |

---

## 工程结构

```
G474_HRTIM/
├── Src/            CubeMX 生成（main.c, hrtim.c, gpio.c, syscalls.c, ...）
├── Inc/            CubeMX 头文件
├── App/            用户代码（自动 GLOB 编译）
│   ├── io_retarget.c/h    printf → USART1 重定向
│   ├── driver.c/h         控制驱动桩
│   └── test.c/h           PID 控制桩
├── .vscode/
│   ├── launch.json        Cortex-Debug 调试配置
│   ├── tasks.json         编译 + 烧录任务
│   └── settings.json      GDB 路径、CMake 配置
├── cmake/
│   ├── gcc-arm-none-eabi.cmake   工具链 + 链接器标志
│   └── stm32cubemx/CMakeLists.txt
├── CMakeLists.txt
└── CMakePresets.json
```

---

## 当前状态

> **硬件驱动层已完成并实测验证**：
> - PA8/PA9：200 kHz 互补 PWM，50% 占空比，死区 250 ns ✓
> - PB12/PB13：200 kHz 互补 PWM，与原边 180° 移相，死区 250 ns ✓
> - UART 调试接口可用，DSP 库已集成
>
> **下一步**：实现 ADC 采样 → PID 控制算法 → 将控制输出写入 HRTIM 比较寄存器实现闭环调节。

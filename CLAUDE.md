# G474_HRTIM 工程实现总结

> 本文档于 2026-06-06 依据实际源码全面校订。工程已从"固定 200kHz 互补 PWM"
> 演进为 **LLC 变频软启动（PFM 扫频）+ 三路比较器硬件保护 + 串口诊断上报 + VOUT ADC 采样**。
> 历史 200kHz / test.c(PID) 等内容已被取代，保留于"踩坑记录"与"历史沿革"中。
>
> **2026-06-06 当日进展（v4）**：实现串口故障上报 + 状态/寄存器心跳（`Fault_Report_Poll`）；
> 新增 VOUT ADC 采样（`vout_adc`，ADC1 规则组 + TIM3 10kHz 周期中断软件触发 + 分压还原 + 标定校正）；
> 串口由 USART3 改为 **USART1(PB6/PB7)**。当日多处硬件调试见"踩坑记录"。
>
> **2026-06-12 进展（v5）—— 辅助电源安全监测 + 安全重入**：原 VOUT(ADC1/PA1) 采样路**飞线改接
> 辅助电源 24V 轨**，语义由 VOUT 改为 **VAUX**（`vout_adc`→`vaux_adc`，旧文件 `#if 0` 停用保留）；
> 新增 **安全重入状态机** `App/safe_sm.c`（INIT→WAIT_AUX→SOFTSTART→RUN→FAULT）消除"辅源衰减期重启
> 误开通半桥"隐患：上电默认封波 + DIS 失能，必须 VAUX≥23V 稳定才启 PWM（FAULT 后亦须 VAUX≥23V
> 才重启）；软件 22V 优雅封波 + COMP2/FLT1 硬件 21V 低有效锁存兜底；新增 **PVD(代码)** 与
> **BOR(代码自动配置 option byte，无需外部工具)** MCU 自保护。详见 §5c。
>
> **2026-06-13 校订（v6）—— CubeMX 待办全部落地 + 参数对齐源码**：§5c 原列的 CubeMX 待办已在
> 重新生成的源码里完成：**HRTIM Fault1=`LOW` + `Filter=FAULTFILTER_9`**（`hrtim.c:50-51`）、
> **COMP2 迟滞=`40MV`**（`comp.c:46`）、**DIS 上电默认 SET+NOPULL**（`gpio.c:57/69`）。另对齐若干源码
> 参数：**时钟改 HSE 12 MHz/PLLM=DIV3**（仍 170 MHz）、**软启动目标 39100=139.5 kHz**（非 41846/130k）、
> **COMP4/6 DAC 阈值=3500**、COMP2 阈值=`VAUX_HW_TRIP_DAC`(2606,21V)、串口 [BOOT] 已是 USART1。
> ⚠️ 仅剩可选项：COMP4/6 迟滞仍 20MV（OCP/OVP 抗扰加固）。

## 目标硬件

| 项目 | 参数 |
|---|---|
| MCU | STM32G474RET6 |
| 内核 | Cortex-M4F（硬件浮点 FPv4-SP-D16） |
| 主频 | 170 MHz（实际代码用 **HSE 12 MHz** → PLLM=DIV3, PLLN=85, PLLR=DIV2；12/3×85/2=170；见 main.c `SystemClock_Config`）|
| Flash | 512 KB |
| RAM | 128 KB（Heap 512 B，Stack 1024 B） |

应用场景：**半桥 LLC 谐振变换器**驱动固件，采用 **PFM 变频控制**。当前为**开环变频软启动**，闭环未实现。

---

## 系统架构总览

```
main()
 ├─ SystemClock_Config()                 170 MHz
 ├─ MX_GPIO_Init()                        LED1/2/3 = PC1/PC2/PC3（开漏，低电平点亮）
 ├─ MX_HRTIM1_Init()                      PWM + 死区 + 3 路 Fault
 ├─ MX_DAC1/2/4_Init()                    比较器阈值源
 ├─ MX_ADC1/2_Init()                      ADC1=VAUX(PA1/IN2,规则组,原VOUT飞线)；ADC2=IOU/I_CYCLE
 ├─ MX_COMP2/4/6_Init()                   保护检测
 ├─ MX_USART1_UART_Init()                 调试串口（PB6/PB7）
 ├─ MX_TIM3_Init()                        10kHz 周期中断（驱动 VAUX 采样）
 ├─ [安全启动序列] 屏蔽 Fault → 启 COMP/DAC → 延时 → 清标志 → 使能 Fault
 ├─ HAL_HRTIM_WaveformOutputStart/CountStart()
 ├─ LLC_SoftStart_Init()  ──┐
 ├─ Fault_IRQ_Enable()      │            使能 FLT 中断(IER)；NVIC 由 CubeMX MspInit 开
 └─ VAUX_ADC_Init()         │            ADC1 自校准 + 启动 TIM3 周期中断
                            ↓
        ⚠️ 注：上电不再无条件启 PWM/软启动；这些已移入 safe_sm 的 WAIT_AUX→SOFTSTART（见 §5c）

HRTIM1_Master_IRQHandler ─→ LLC_SoftStart_Step()   // 每次 MREP 中断扫频
   (stm32g4xx_it.c)            (App/freq_skip.c)

TIM3_IRQHandler ──────────→ HAL_TIM_PeriodElapsedCallback()   // 10kHz 软件触发 ADC1 读 VAUX → 喂 safe_sm
   (stm32g4xx_it.c)            (App/vaux_adc.c)

COMP2/4/6 ──(内部 Fault 线)──→ HRTIM Fault1/2/3 ──→ 硬件强制 PWM 输出 INACTIVE
                                      └─→ HRTIM1_FLT_IRQHandler → Fault_OnIRQ()  // 软件记录+点灯
                                            (App/fault_log.c)

while(1) ─→ Fault_Report_Poll()   // 故障边沿打印 + 每秒 [STAT] 心跳 + 一次 [REGS] 寄存器 dump
              (App/fault_log.c)
```

> **核心控制循环是中断驱动的**：`while(1)` 只跑 `Fault_Report_Poll()`（串口诊断打印）。
> 真正反复执行的功率控制逻辑是中断里的 `LLC_SoftStart_Step()` 与 `HAL_TIM_PeriodElapsedCallback()`。

---

## 已实现的功能

### 1. 时钟系统配置

- **HSE 12 MHz** → PLLM=DIV3，PLLN=85，PLLR=DIV2，SYSCLK = 170 MHz（12MHz/3=4MHz ×85=340MHz /2=170MHz）
- 电压调节器 `SCALE1_BOOST`，Flash 延迟 `FLASH_LATENCY_4`
- APB1 = HCLK/1（170 MHz，TIM3 时基时钟）；APB2 = HCLK/2

### 2. HRTIM 高分辨率 PWM（功率级硬件层，`Src/hrtim.c`）

**时基配置：**

| 参数 | 值 | 说明 |
|---|---|---|
| 预分频 | MUL32 | 等效时钟 5440 MHz，分辨率 ≈ 184 ps |
| 周期寄存器（初值）| **27200** | 200 kHz（上电瞬时值，随即被软启动覆盖为 300kHz）|
| 重复计数器 | **3** | 每 **4** 个 PWM 周期触发一次更新/MREP 中断 |
| Master 模式 | CONTINUOUS | 主时基连续运行 |
| Timer A/C 模式 | SINGLESHOT_RETRIGGERABLE | 由 `ResetTrigger=MASTER_PER` 周期重触发 |

> ⚠️ Timer A 配置了 `InterleavedMode = DUAL`（Timer C 为 DISABLED）。

**Timer A — 原边半桥驱动（PA8 / PA9）：**
- TA1（PA8）：`SetSource = MASTERPER` 置位，`ResetSource = TIMCMP1` 复位
- TA2（PA9）：由死区硬件自动生成互补信号，**不配置 Set/Reset 源**

**Timer C — 副边同步整流驱动（PB12 / PB13）：**
- TC1（PB12）：`SetSource = TIMCMP1`（CMP1=13600）置位，`ResetSource = TIMCMP4`（CMP4=26858）复位，与原边 180° 移相
- TC2（PB13）：由死区硬件自动生成互补信号

**死区（实测验证 250 ns）：**

| 参数 | 值 | 说明 |
|---|---|---|
| 预分频宏 | `HRTIM_TIMDEADTIME_PRESCALERRATIO_MUL8` | 死区时钟 = f_HRTIM × 8 = **1360 MHz** |
| 死区 tick | **0.735 ns** | 基准是 f_HRTIM=170 MHz，**非**等效时钟 5440 MHz |
| Rising/FallingValue | **340** | 340 × 0.735 ns ≈ 250 ns |

**死区插入规则：** 启用 `DeadTimeInsertion` 后，互补通道（TA2/TC2）完全由死区硬件接管，只需配置主通道（TA1/TC1）的 Set/Reset 源；TA2/TC2 的 `WaveformOutputConfig` 设为 `SetSource=NONE/ResetSource=NONE`。

**DLL 校准：** 启动时执行（`CALIBRATIONRATE_3`，超时 10 ms）。

### 3. LLC 变频软启动（核心控制，`App/freq_skip.c` / `freq_skip.h`）

通过周期性增大 HRTIM 周期寄存器，把开关频率从 **300 kHz 缓降到 139.5 kHz**（开环测试，限制 FSW>FR
防止进入容性区；非谐振点 130 kHz），实现软启动。

| 宏 | 值 | 物理含义 |
|---|---|---|
| `LLC_FREQ_START_PER` | 18133 | 起始周期 = **300 kHz**（高频 → 低增益）|
| `LLC_FREQ_TARGET_PER` | 39100 | 目标周期 = **139.5 kHz**（开环测试，限制 FSW>FR，防容性区；原 41846/130k 已注释保留）|
| `LLC_SOFTSTART_STEP` | 10 | 每次扫频的周期增量（越大降频越快）|
| `LLC_SKIP_COUNT` | 10 | 每 10 次 MREP 中断扫频一次 |

> 频率换算：5440 MHz ÷ 18133 = 300 kHz；5440 MHz ÷ 39100 = 139.1 kHz（标称 139.5 kHz）。

**状态机（隐式，单向不可逆）：**

| 状态 | `softstart_done` | 转换条件 | 动作 |
|---|---|---|---|
| RAMPING（扫频中）| 0 | 上电 `LLC_SoftStart_Init()` | 每 10 次中断 `llc_period += 10`，更新 MPER / TimerA PER / TimerC PER / CMP1(=period/2，180°移相) / CMP4(=period-342，关断点) |
| DONE（到位）| 1 | `llc_period >= 39100` | 钳位到目标周期，停止扫频 |

软启动直接写寄存器（`HRTIM1->sMasterRegs.MPER` 等），不走 HAL。

### 4. 硬件保护（三路比较器 → HRTIM Fault，`comp.c` + `dac.c`）

| 通道 | 检测引脚 | 阈值源 | 触发 | 动作 |
|---|---|---|---|---|
| COMP2 → Fault1 | PA3 | DAC1_CH2 = **`VAUX_HW_TRIP_DAC`≈2606** (21V VAUX 欠压闸)，迟滞 **40mV**，极性 **LOW(低有效)** | VAUX<21V→COMP 输出低=故障 | HRTIM 硬件封锁全部 PWM 输出 → `FAULTLEVEL_INACTIVE` |
| COMP4 → Fault2 | PB0 | DAC1_CH1 = **3500**，迟滞 20mV，极性 HIGH | INP > 阈值（高电平）| 同上 |
| COMP6 → Fault3 | PB11 | DAC4_CH2 = **3500**，迟滞 20mV，极性 HIGH | 同上 | 同上 |

> ⚠️ COMP2/Fault1 语义已变：v5 起由"过压保护"改为 **VAUX 24V 辅源欠压闸**（低有效），见 §5c。
> COMP4/6 仍是原边 **过流/过压硬件快速保护（OCP/OVP）**（高有效）。

- 硬件封锁是**纳秒级、不依赖软件**的；软件记录由 `fault_log` 模块在中断里补做（见下）。
- **消抖已加（v6）**：`hrtim.c` 三路 Fault 已用 `FAULTFILTER_9`（fSAMPLING=fHRTIM/8 一档，抗开关噪声尖峰），
  Fault1=LOW、Fault2/3=HIGH。COMP2 迟滞已提到 40MV。
  - ⚠️ STM32G4 的 **COMP 外设本身没有数字滤波器**；滤波靠 **HRTIM Fault 的 Filter**（`pFaultCfg.Filter`）。
  - 剩余可选抗扰：① COMP4/6 Hysteresis 仍 20MV，可调高；② PB11 输入加 RC 低通最对症（仍未做）。

**上电防误触发序列（main.c）：**
1. `FaultModeCtl(DISABLED)` 屏蔽 Fault1/2/3
2. 清 `ICR` 故障标志
3. `HAL_COMP_Start` + `HAL_DAC_SetValue`（COMP2=`VAUX_HW_TRIP_DAC`≈2606，COMP4/6=3500）建立阈值
4. `HAL_Delay(1)` 等 DAC 上拉稳定
5. 再清一次 `ICR`
6. `FaultModeCtl(ENABLED)` 正式使能保护
7. （软启动后）`Fault_IRQ_Enable()` 使能 FLT 中断 + NVIC

### 4b. 故障软件记录（`App/fault_log.c` / `fault_log.h`）

把"纯硬件锁死、软件无感知"升级为"硬件锁死 + 软件可记录可恢复"。

- **中断**：`HRTIM1_FLT_IRQHandler`（中断号 `HRTIM1_FLT_IRQn`，**独立于 Master**）→ `Fault_OnIRQ()`。
- **判别哪一路**：必须用 `__HAL_HRTIM_GET_FLAG`（读 ISR）。⚠️ 本 HAL 版本 `__HAL_HRTIM_GET_ITSTATUS` 只查 IER（是否使能），不查 ISR，不能用来判触发。
- **记录**：全局 `volatile fault_record_t g_fault` —— 各路次数 `flt1/2/3_cnt`、`total_cnt`、`last_fault`（哪一路）、`last_tick`（HAL_GetTick 时刻）、`tripped`（锁死标志）。
- **指示灯**：FLT1/2/3 → **LED1/LED2/LED3 = PC1/PC2/PC3**（开漏，低电平点亮）。`Fault_IRQ_Enable()` 先熄灭三灯做基线（gpio.c 上电默认拉低=点亮）。
- **防中断风暴**：FAULT 无滤波，比较器电平保持高时清标志会立刻重置 → 记录一次后**关闭本路 FLT 中断**（`__HAL_HRTIM_DISABLE_IT`），恢复时再开。
- **锁死不自动恢复**：ISR 只记录+点灯，PWM 保持硬件封锁。恢复必须显式调 `Fault_Rearm()`（清 ICR → `WaveformOutputStart` 重启输出 → 重新武装中断 + 熄灯）。对 LLC 过流/过压这是安全做法，**不要自动恢复**。

**串口上报（`Fault_Report_Poll()`，在 `while(1)` 里轮询，非中断上下文，可安全 printf）：**
- **故障边沿打印**：检测到 `g_fault.total_cnt` 变化时打印一次——哪一路(FLT1/2/3 + COMP/引脚)、`last_tick`、各路次数、`tripped`、故障瞬时 period/fsw。
- **每秒状态心跳 `[STAT]`**：`state / period / fsw(=5440MHz/period) / softstart_done / tripped / flt_total / VAUX raw/filt(mV)`。是核对实测开关频率、软启动与安全状态机的主要手段。
- **一次性寄存器 dump `[REGS]`**：软启动完成后打印一次 Master/TimerA/TimerC 的 `CR/PER/CMP1/CMP4/SET1/RST1/RSTR`，用于定位 PWM 波形问题（如对比两路是否一致）。

**接线方式（已采用 regen-safe 方案）：**
- CubeMX System Core → NVIC **已勾选** "HRTIM1 fault global interrupt"，故：
  - NVIC（`HRTIM1_FLT_IRQn`）由 CubeMX 在 `HAL_HRTIM_MspInit` 自动开启；
  - CubeMX 在 `stm32g4xx_it.c` 标准区生成 `HRTIM1_FLT_IRQHandler`，`Fault_OnIRQ()` 写在它的 `USER CODE BEGIN HRTIM1_FLT_IRQn 0` 块内（在 `HAL_HRTIM_IRQHandler(...COMMON)` 之前）。
  - `Fault_OnIRQ()` 已读/清 ISR 并关本路 IT，故其后的 `HAL_HRTIM_IRQHandler` 对故障部分为空操作，不重复处理。
- **以后重新生成代码不再冲突**（USER CODE 块内容被保留，且不再有手写的同名 handler）。
- ⚠️ 仍必须保留 `Fault_IRQ_Enable()` 里的 `__HAL_HRTIM_ENABLE_IT(...FLT...)`：工程用 `WaveformCountStart`（非 `_IT`），`Init.HRTIMInterruptResquests` 不会被 HAL 写入 IER；CubeMX 只开了 NVIC，没开 HRTIM 级别的中断使能。

### 5. UART 调试串口（USART1）

- 引脚：**PB6（TX）/ PB7（RX）**，波特率 115200-8N1，时钟源 PCLK2
- `App/io_retarget.c` 将 `printf` / `scanf` 重定向到 **USART1（`huart1`）**
- 浮点打印：链接器选项 `-Wl,-u,_printf_float`
- ⚠️ **必须在第一次 printf 前 `setvbuf(stdout, NULL, _IONBF, 0)`**：裸机 newlib 默认全缓冲，`\n` 不 flush，否则表现为"串口不打印"（见踩坑记录）。
- 注：`[BOOT]` 打印字符串已修正为 "USART1"（旧版曾误写 USART3）。

### 5b. ~~VOUT ADC 采样~~（`App/vout_adc.c` —— **已停用，被 §5b' VAUX 取代**）

> ⚠️ v5 起 PA1 已飞线改接辅源 24V 轨，本节内容由 **§5c / `App/vaux_adc.c`** 接管；`vout_adc.c`
> 两个函数已 `#if 0` 关闭（避免与 vaux_adc.c 的 `HAL_TIM_PeriodElapsedCallback` 重复定义）。
> 下文保留原 VOUT 描述仅供历史参考。

- **通道**：ADC1 规则组 Rank1 = IN2 = **PA1**（原 VOUT，现 VAUX），采样时间 247.5 cycles，12bit。
- **触发**：方案 A —— **TIM3 10kHz 周期中断软件触发**（`HAL_TIM_PeriodElapsedCallback` → `HAL_ADC_Start` + 自旋等 EOC + `HAL_ADC_GetValue`）。ADC 触发源保持 `ADC_SOFTWARE_START`，未用 TRGO 硬件触发。
  - 自旋等 EOC（不依赖 `HAL_GetTick`），中断里安全、ADC 异常也不死锁。
- **启动**：`VOUT_ADC_Init()`（main USER CODE 2 末尾）做 ADC1 单端自校准 + `HAL_TIM_Base_Start_IT(&htim3)`。
- **换算**：`g_vout_raw`(0..4095) → 引脚 mV(`raw×3300/4095`) → ×分压比(`VOUT_DIV_NUM/DEN`，默认 10/1) → ×标定 `VOUT_CAL_GAIN` + `VOUT_CAL_OFFSET_MV` → `g_vout_mv`。标定方法见头文件注释（两点法/单点增益法）。
- 结果随 `[STAT]` 串口心跳打印。**慢电压量用规则组足矣**；需与开关周期同步的电流采样才用注入组。

### 6. ARM CMSIS-DSP 数学库

- X-CUBE-ALGOBUILD 1.4.0，预编译 `libarm_cortexM4lf_math.a`（硬浮点 Cortex-M4F）
- 编译宏：`ARM_MATH_CM4` + `ARM_MATH_LOOPUNROLL`

### 7. 开发环境

| 项目 | 说明 |
|---|---|
| 工具链 | `arm-none-eabi-gcc`，硬浮点 `-mfloat-abi=hard -mfpu=fpv4-sp-d16` |
| 构建 | CMake + Ninja，`Ctrl+Shift+B` 一键编译烧录 |
| 调试器 | DAPLink（CMSIS-DAP），OpenOCD `D:/openocd/daplink.cfg` |
| GDB | `C:/Users/ywjAv/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin` |
| 烧录 | tasks.json `Flash: OpenOCD`，编译后自动 program/verify/reset |
| 调试 | launch.json `STM32G474 Debug`，F5 启动 Cortex-Debug |

---

## 中断服务函数（ISR）

| ISR | 触发条件 | 作用 |
|---|---|---|
| `HRTIM1_Master_IRQHandler` | HRTIM Master 重复事件 MREP（每 4 个 PWM 周期）| 清 MREP 标志 → `LLC_SoftStart_Step()` 扫频 |
| `HRTIM1_FLT_IRQHandler` | HRTIM Fault1/2/3（COMP2/4/6 越限）| `Fault_OnIRQ()`：记录通道/次数/时刻、点亮对应 LED、关本路中断防风暴 |
| `TIM3_IRQHandler` | TIM3 更新事件（10 kHz）| `HAL_TIM_PeriodElapsedCallback()` → 软件触发 ADC1 采 VAUX + EWMA + 喂 safe_sm（`App/vaux_adc.c`）|
| `SysTick_Handler` | 1 ms 节拍 | `HAL_IncTick()`（供 `HAL_Delay`）|
| `NMI / HardFault / MemManage / BusFault / UsageFault` | CPU 异常 | 死循环挂起 |

---

## 关键参数汇总表

| 参数 | 值 | 单位 | 位置 | 含义 |
|---|---|---|---|---|
| PLLN | 85 | — | main.c:235 | SYSCLK 170 MHz（HSE 12M/DIV3）|
| LLC_FREQ_START_PER | 18133 | tick | freq_skip.h:12 | 软启动起始 300 kHz |
| LLC_FREQ_TARGET_PER | 39100 | tick | freq_skip.h:13 | 软启动目标 139.5 kHz |
| LLC_SOFTSTART_STEP | 10 | tick | freq_skip.h:14 | 扫频步进 |
| LLC_SKIP_COUNT | 10 | 次 | freq_skip.h:16 | 中断分频 |
| Period（初值）| 27200 | tick | hrtim.c:81 | 200 kHz 上电瞬时值 |
| RepetitionCounter | 3 | — | hrtim.c:82 | 4 周期更新 |
| 死区 Rising/Falling | 340 | tick | hrtim.c:158/162 | 250 ns |
| TC1 CMP1 | 13600 | tick | hrtim.c:211 | 180° 移相置位点 |
| TC1 CMP4 | 26858 | tick | hrtim.c:151 | 关断点 |
| 软启动 CMP1 | period/2 | tick | freq_skip.c:50 | 移相跟随 |
| 软启动 CMP4 | period-342 | tick | freq_skip.c:51 | 关断点跟随 |
| DAC 阈值(COMP2) | 2606 | LSB | main.c:157 | VAUX 21V 欠压闸(`VAUX_HW_TRIP_DAC`) |
| DAC 阈值(COMP4/6) | 3500 | LSB | main.c:160/163 | OCP/OVP 阈值 |
| COMP2 迟滞 | 40 | mV | comp.c:46 | 抗抖动 |
| HRTIM Fault Filter | FAULTFILTER_9 | — | hrtim.c:51 | 抗噪声消抖 |
| HRTIM Fault1 极性 | LOW | — | hrtim.c:50 | VAUX 低有效；Fault2/3=HIGH |

---

## 踩坑记录

### HRTIM Period 计算
- Period = f_HRTIM_等效 / f_PWM = 5440 MHz / f。例：200 kHz→27200，300 kHz→18133，139.5 kHz→39100，130 kHz→41846。

### 死区时间计算
- **错误**：以为死区基准是等效时钟（5440 MHz）→ 算出 170 ticks。
- **正确**：死区基准是 f_HRTIM = 170 MHz，MUL8 → 1360 MHz，需要 **340 ticks**。

### 死区互补通道配置
- 启用死区后 TA2/TC2 由硬件接管，主通道之外不要再赋有效 Set/Reset 源（设为 NONE）。

### 软启动直接写寄存器
- `LLC_SoftStart_Step()` 绕过 HAL 直接写 `MPER/PERxR/CMPxR`，并通过预装载（`PreloadEnable`）在 MREP 时刻安全生效。修改频率参数时务必同步更新 CMP1（移相）与 CMP4（关断点），否则波形错乱。

### 故障防误触发
- DAC 阈值建立需要时间，**必须**先屏蔽 Fault、`HAL_Delay(1)` 后再清标志使能，否则上电瞬间会误锁死。

### FLT 中断判别 / 防风暴
- 判"哪一路触发"用 `__HAL_HRTIM_GET_FLAG`（读 ISR）；`__HAL_HRTIM_GET_ITSTATUS` 只查 IER（是否使能），不可用于判触发。
- FAULT 无滤波，比较器电平保持高时清标志会立刻重置 → ISR 记录后须 `__HAL_HRTIM_DISABLE_IT` 关本路中断防风暴。
- 工程用 `WaveformCountStart`（非 `_IT`），故 FLT 中断须在 `Fault_IRQ_Enable()` 里手动 `__HAL_HRTIM_ENABLE_IT`；`Init.HRTIMInterruptResquests` 字段不会被消费。

### CubeMX 重新生成 FLT handler 冲突（已解决）
- 教训：把中断处理写成独立的 `HRTIM1_FLT_IRQHandler`（USER CODE 1）后，一旦在 CubeMX 勾选该 NVIC，会另生成同名 handler → 链接重定义报错。
- 现行做法：handler 调用统一放进生成 handler 的 `USER CODE BEGIN HRTIM1_FLT_IRQn 0` 块（见 §4b），不再手写同名函数 → regen-safe。

### printf 全缓冲导致"串口不打印"（2026-06-06）
- 现象：代码在跑、PWM 正常，但串口一个字都没有。
- 真因：裸机 newlib 的 stdout **默认全缓冲**（非行缓冲），`\r\n` 不触发 flush，要攒满 ~1KB 才一次性吐出。
- 解法：第一次 printf 前 `setvbuf(stdout, NULL, _IONBF, 0)` 关缓冲。
- 链路依赖：`printf → _write(syscalls.c) → __io_putchar(io_retarget.c) → HAL_UART_Transmit(huart1)`；`syscalls.c` 必须在编译列表里，否则 `_write` 是空桩、全丢。

### Timer C "260kHz" 假象 = PB12/PB13 焊接短路（2026-06-06）
- 现象：示波器单路测 Timer C(PB12) 为 260k，Timer A 130k；而软件 `[REGS]` 显示两路 `PER=41846` 都应是 130k。
- 关键逻辑：TimerC `RSTR=MSTPER`，与驱动 TA1 的主周期同源；TA1 既是 130k，TimerC 不可能复位更快 → 物理上出不了 260k。
- 真因：**PB12 与 PB13 焊接短路**——两路死区互补、相差半周期的 130k 信号叠加，呈现 260k。重新焊接后恢复 130k。教训：寄存器与实测矛盾时，先查硬件（短路/虚焊），别急着改配置或怀疑芯片烫坏（精确翻倍不是热损伤特征）。

### COMP/PB11 噪声误触发（2026-06-06）
- 现象：运行几秒~十几秒后 FLT3 触发、PWM 锁死；用示波器探头一碰 PB11 反而不触发。
- 真因：PB11 高阻 + COMP6 无迟滞 + HRTIM Fault 无滤波，开关噪声尖峰冲过阈值即锁死；探头的 ~10pF 电容把尖峰滤平了 → 不触发。属噪声误触发，非真过流。
- 解法见 §4 抗扰三件套 + PB11 加 RC 低通。

### 外置电源供电串口乱码 = 没共地（2026-06-06）
- USB 供电正常，换外置电源后串口"一直打印且乱码"。真因：串口适配器 GND 与板子/外置电源没共地（地环路/地弹使 TX 电平失真）。**把串口适配器 GND 与 MCU GND 接到一起即恢复。** 功率地与信号地应单点汇接；必要时用隔离 USB-TTL。

---

### 5c. 辅助电源(VAUX)安全监测 + 安全重入状态机（`App/safe_sm.c` / `vaux_adc.c`）

**动机（事故）**：连续多次重启时独立辅助电源缓慢衰减，MCU 在欠压/不确定态仍输出 PWM，再次
上电瞬间疑似半桥误开通 → MOS 全部击穿。本子系统消除这个"重启窗口"。

**硬件前提（已飞线，软件按此适配，不新增通道/不改分压）**：
- 原采样 VOUT 的 **ADC1_IN2 = PA1** 已飞线改接辅助电源 **24V 轨**（经原分压，比例 10:1 不变）。
- 同一分压点并接 **COMP2 的 IN+ = PA3**，IN− = DAC1_CH2，做 21V 硬件欠压闸。
- 分压比 / 伏特↔ADC码↔DAC码 全部沿用原 VOUT 换算（`vaux_adc.h`），不重算。

**门限（集中在 `safe_sm.h`，伏特→码用 `VAUX_MV_TO_CODE` 同源换算）**：

| 宏 | 值 | 码(≈) | 作用 |
|---|---|---|---|
| `VAUX_SW_TRIP_MV` | 22.0 V | ADC 2730 | 软件检测：跌破→优雅封波+记 log+进 FAULT |
| `VAUX_HW_TRIP_MV` | 21.0 V | DAC 2606 | 硬件 COMP2/DAC1_CH2 闸（比软件低一档，软件优先、硬件兜底）|
| `VAUX_REARM_MV` | 23.0 V | ADC 2854 | 迟滞回升点；须稳定 `VAUX_STABLE_MS`(50ms) 才放行重启 |

> **2026-06-13/v6 移除「断透 latch」重启互锁**：原 `VAUX_CLEAR_MV`(5V)「确认断透」门限 + `aux_dropped_latch`
> 标志已删除。原因：MCU 与 UCC21520 VCCI 共用同一供电链（24V→DCDC(4~30V)→5V→①LDO→3.3V MCU；②5V VCCI），
> VAUX 真正断透时 MCU 必然掉电、下次为冷上电从 INIT 天然安全重入，「跌破 5V 才确认断透」MCU 根本活不到执行，
> 是存不住的死逻辑。现重启条件仅为 **VAUX 回升≥REARM(23V) 且 WAIT_AUX 稳定 50ms 自检通过**。

**分层防护（由软到硬，互为兜底）**：
1. **软件优雅封波**：10kHz ISR(`vaux_adc.c`) 读 VAUX → 一阶 EWMA(α=1/8,τ≈0.8ms) → `SafeSM_OnSample()`；
   仅在 SOFTSTART/RUN 跌破 22V 时 `SafeSM_EnterFault()`。临界路径全整数。
2. **硬件兜底封波**：COMP2(PA3) vs DAC1_CH2(21V) → **HRTIM Fault1「低有效 + latched」**（VAUX<21V 时
   COMP 输出低=故障，纳秒级硬封 PWM，不依赖 ISR 节拍）。方向矛盾用"低有效"解决，不动硬件接法。
3. **器件 UVLO 兜底**：UCC21520A(5V版) VDD_OFF=5.4/5.7/6.0V、VCCI_OFF=2.35/2.5/2.65V；从 22V 降到 6V
   这一大段 UVLO 够不着，必须靠 1)+2)+DIS 默认失能。
4. **MCU 自保护**：PVD(`PWR_PVDLEVEL_6`≈2.9V，运行期 HAL，主循环轮询 `PWR_FLAG_PVDO`，**不挂中断**以
   regen-safe) + **BOR**（`OB_BOR_LEVEL_4`≈2.8V，`SafeSM_EnsureBOR()` 首启自动编程 option byte 并
   `OB_Launch` 复位一次，**无需任何外部工具**，之后跳过）。

**状态机（`g_safe_state`，`SafeSM_Poll()` 在主循环推进，`SafeSM_OnSample()` 在 10kHz ISR 喂数据）**：

| 状态 | 进入/动作 | 转出条件 |
|---|---|---|
| INIT | 复位后第一件事：HRTIM 输出 inactive + **DIS 失能(PA12=SET)** + 首启配 BOR | 立即→WAIT_AUX |
| WAIT_AUX | 保持封波等辅源 | VAUX≥23V 且稳定 50ms → 清锁存故障 + DIS 使能 + `LLC_SoftStart_Init()` → SOFTSTART |
| SOFTSTART | 复用现有 300k→139.5k 扫频（不重写）| `softstart_done`→RUN；`g_fault.tripped`→FAULT |
| RUN | 开环定频运行 | 任何 `g_fault.tripped` 或 22V/PVD → FAULT |
| FAULT | 封波 + DIS 失能，停留 | VAUX 回升≥23V(REARM) → WAIT_AUX（由其稳定50ms自检+清锁存+重走完整软启动，严禁直接 RUN）。真正断透由 MCU 掉电冷启动从 INIT 自然处理 |

**关键点**：
- PWM 启动序列（`WaveformOutputStart`+`CountStart`+`LLC_SoftStart_Init`+DIS 使能）**全部从 main 移到
  SafeSM 的 WAIT_AUX→SOFTSTART 转移**，上电不再无条件启 PWM。
- 上电若 VAUX<21V，FLT1(低有效)立即锁存属预期；SM 启动时 `SafeSM_ClearLatchedFault()` 统一清。
- `SafeSM_EnterFault()` 幂等，可从 ISR/主循环任意调用。
- `[STAT]` 心跳增打 `state=` 与 `VAUX raw/filt/mV`。

**✅ 已完成 / 实测验证（DIS 控制，2026-06-13 终态）—— PA12 IO 直驱 DIS（已去掉 2N7002）**：
硬件演进：原设计经 2N7002 反相驱动（曾为此在 v5 反转过软件宏）；**调试中已拆除 2N7002，改为 PA12
直接驱动 UCC21520A 的 DIS 脚**（非反相，PA12 电平 = DIS 电平）。直驱后无反相环节，现有软件极性
（`ENABLE=RESET`、`DISABLE=SET`）**恰好正确，无需再改**：
- `App/safe_sm.h:54-55`：`HALF_BRIDGE_ENABLE()=RESET(低)` → DIS 低 = 使能；`DISABLE()=SET(高)` → DIS 高 = 失能。
- `Src/gpio.c:57`：上电默认 `SET(高)=失能`（安全默认，消除 MX_GPIO_Init→SafeSM_Init 窗口）；`gpio.c:69` `NOPULL`。
- `App/safe_sm.c`：复位/INIT/WAIT_AUX/SOFTSTART/FAULT 一律失能(PA12 高)；仅 RUN 主动使能(PA12 低)；`SafeSM_EnterFault()` 内失能。
- ⚠️ 注意安全默认前提变化：拆 2N7002 后，**掉电/复位/GPIO 高阻时 DIS 不再被外部上拉钳在失能态**——
  现靠 MCU GPIO 上电即 `SET` + UCC21520A 自身 DIS 引脚特性兜底；若需"MCU 失电也强制失能"，应在 DIS 网络保留一只对 VCCI 的上拉电阻。

**实测验证（直流源 0.1V 步进）**：VAUX **>23.1V → PA12=0V（使能）**；VAUX **<22V → PA12=3.3V（失能）**。
正好落在固件迟滞带 `REARM(23V)`↔`SW_TRIP(22V)` 内，逻辑确认正确。

**✅ 已完成（CubeMX 已改并重新生成，2026-06-13/v6）**：
1. ✅ **HRTIM Fault1 → 低有效 + 锁存**：`hrtim.c:50` `Polarity=HRTIM_FAULTPOLARITY_LOW`；`hrtim.c:51`
   `Filter=HRTIM_FAULTFILTER_9`；输出 `FAULTLEVEL_INACTIVE` 为锁存型。VAUX<21V 硬件欠压闸已真正生效。
2. ✅ **COMP2 迟滞**：`comp.c:46` `Hysteresis=COMP_HYSTERESIS_40MV`。
3. ✅ **DIS 默认电平**：`gpio.c:57` 上电默认 `GPIO_PIN_SET`(=失能) + `gpio.c:69` `GPIO_NOPULL`，已在
   CubeMX 生成区（重生成不再被覆盖）。

**剩余（可选，非阻塞）**：
- COMP4/6 迟滞仍 `20MV`（`comp.c:73/100`），原 OCP/OVP 抗扰可调高 + PB11 加 RC 低通；FLT2/3 不动。

## 尚未实现 / 风险点

| 项 | 文件 | 状态 |
|---|---|---|
| 闭环控制 | `App/driver.c` | `DRIVER_Run(ref,fb)` 仅返回 `ref-fb`，**从未被调用**（孤立桩）；本版开环测增益，无闭环 |
| ADC 反馈采样 | `App/vaux_adc.c` | ✅ ADC1/PA1 已改采 **VAUX(辅源24V)**（TIM3 10kHz + EWMA），有 `g_vaux_mv`；IOU/I_CYCLE(ADC2) 仍未启动；原 VOUT 采样已停用(`vout_adc.c` `#if 0` 保留) |
| VAUX 标定 | `vaux_adc.h` | 分压比沿用 10/1，`VAUX_CAL_GAIN/OFFSET` 默认未校正——按实测两点法/单点法填 |
| HRTIM Fault1 极性 | `hrtim.c:50` | ✅ 已改 **`LOW`(低有效)** + Filter_9，VAUX 欠压硬件闸已生效（v6）|
| 故障恢复 | `App/fault_log.c` | 已有 `Fault_Rearm()` 可手动恢复；**无自动恢复**（对 OCP/OVP 是有意为之）；尚无触发入口（如串口指令） |
| 故障上报 | `App/fault_log.c` | ✅ 已实现 `Fault_Report_Poll()` 串口打印（故障详情 + [STAT] + [REGS]）|
| Fault 消抖 | `hrtim.c:51` | ✅ 已用 `FAULTFILTER_9` + COMP2 迟滞 40MV（v6）；剩 COMP4/6 迟滞 + PB11 RC 可选 |
| 主循环 | `main.c` | `while(1)` 跑 `Fault_Report_Poll()`（仅诊断打印，无控制逻辑）|

---

## 工程结构

```
G474_HRTIM/
├── Src/            CubeMX 生成（main.c, hrtim.c, comp.c, dac.c, adc.c, usart.c, gpio.c, ...）
├── Inc/            CubeMX 头文件
├── App/            用户代码（自动 GLOB 编译，App/*.c）
│   ├── io_retarget.c/h    printf → USART1 重定向（huart1）
│   ├── freq_skip.c/h      LLC 变频软启动（核心控制）
│   ├── fault_log.c/h      HRTIM 故障中断记录 + 恢复 + 串口上报（g_fault / Fault_Rearm / Fault_Report_Poll）
│   ├── vaux_adc.c/h       VAUX(辅源24V) ADC 采样（TIM3 10kHz 触发 + EWMA + 分压还原 + 标定）→ 喂 safe_sm
│   ├── safe_sm.c/h        辅源安全监测 + 安全重入状态机（门限/状态机/enter_fault/PVD/BOR）见 §5c
│   ├── vout_adc.c/h       【已停用·保留】原 VOUT 采样，被 vaux_adc 取代（函数 #if 0）
│   └── driver.c/h         控制驱动桩（未使用）
├── cmake/
│   ├── gcc-arm-none-eabi.cmake   工具链 + 链接器标志
│   └── stm32cubemx/CMakeLists.txt
├── CMakeLists.txt
├── CMakePresets.json
└── HRTIM.ioc       CubeMX 工程配置
```

> 注：`.vscode/` 与旧 `App/test.c·test.h`（PID 桩）已移除。

---

## 历史沿革

- **v1（已被取代）**：固定 200 kHz 互补 PWM，50% 占空比，副边 180° 移相，死区 250 ns，UART 走 USART1(PC4/PC5)，用 `App/test.c` 做 PID 桩。CLAUDE.md 早期版本描述此状态。
- **v2**：变频软启动（300→130 kHz PFM），新增 COMP2/4/6 + DAC1/2/4 硬件保护，UART 改为 USART3(PB9/PB8)，删除 test.c，新增 freq_skip.c。
- **v3**：新增 `fault_log` 故障中断记录（`HRTIM1_FLT_IRQHandler` → `Fault_OnIRQ`，三路 LED 指示 + `Fault_Rearm` 恢复）；引脚重排（LED1/2/3=PC1/2/3，ADC 增配 VOUT/IOU/I_CYCLE + ADC2）。
- **v4（当前，2026-06-06）**：串口诊断上报 `Fault_Report_Poll`（故障详情 + `[STAT]` + `[REGS]`）；新增 `vout_adc`（ADC1 规则组 VOUT + TIM3 10kHz 触发 + 标定校正）；串口 USART3→**USART1(PB6/PB7)**；当日完成多项硬件调试（PB12/13 短路、PB11 噪声误触发、串口共地、printf 全缓冲）。

---

## 当前状态与下一步

> **已完成**：
> - PA8/PA9、PB12/PB13 互补 PWM，180° 移相，死区 250 ns ✓（实测，PB12/13 短路已排除）
> - 300→139.5 kHz 变频软启动（中断驱动）✓
> - 三路比较器硬件 OCP/OVP 保护（锁死型）✓
> - 故障软件记录 + **串口上报** `Fault_Report_Poll`（故障详情 + `[STAT]` + `[REGS]`）✓
> - **VOUT ADC 采样**（ADC1/PA1，TIM3 10kHz 触发，分压还原 + 标定）✓（FLASH ~10.4%/RAM 2.5%）
> - UART(USART1) 调试 + DSP 库集成 ✓
>
> - **辅源安全监测 + 安全重入**（v5/v6）：VAUX 采样(原 VOUT 飞线) + 软件 22V 优雅封波 + COMP2/FLT1 硬件
>   21V 低有效兜底 + 状态机(INIT→WAIT_AUX→SOFTSTART→RUN→FAULT) + VAUX≥23V 重启 + PVD/BOR ✓
>   **CubeMX 待办已全部落地**（Fault1=LOW+Filter_9、COMP2=40MV、DIS 默认 SET）✓（v6）
>
> **下一步**：
> 1. **VAUX 标定 + 门限实测验证**：示波器/万用表核对 [STAT] 的 VAUX mV，按需填 `VAUX_CAL_GAIN/OFFSET`；
>    实测确认 22V 软封波、21V 硬封波、23V 重启各门限动作正确。
> 2. **抗扰加固（可选）**：COMP4/6 迟滞（+ PB11 硬件 RC）——消除原 OCP/OVP 噪声误触发。
> 3. **闭环**：反馈 → 主循环/定时中断跑 PID/`DRIVER_Run` → 动态调 HRTIM 周期（PFM 调压）；启动 IOU/I_CYCLE(ADC2)。
> 4. 可选：给 `Fault_Rearm()`/重启流程 加串口指令触发入口。

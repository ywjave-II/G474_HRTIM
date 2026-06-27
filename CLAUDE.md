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
> **2026-06-14 进展（v6.1）—— 统一 ADC 采样模块**：原分散的 `vaux_adc.c/h` + `vout_adc.c/h` 整合为
> 单一 `App/adc_app.c/h`，所有 ADC 采样通道通过头文件 `#if` 开关集中管理。通道切换只需改
> `adc_app.h` 的 `CHANNEL`/`HANDLE` 宏，无需重生成 CubeMX。旧文件 `#if 0` 保留备用。
> ADC2 已确认 ScanConvMode=DISABLE + NbrOfConversion=1（软件重配通道，不依赖硬件扫描）。
> VOUT(ADC2/IN12/PB2) 已具备即改即用条件，待硬件接线。
>
> **2026-06-17 进展（v6.2）—— VOUT 通道正式启用**：硬件改接 VOUT 分压点至 **ADC2/IN12/PB2**；
> `adc_app.h` 开关 `ADC_APP_ENABLE_VOUT=1`；VOUT 采样数据加入 `[STAT]` 串口心跳打印；
> 旧文件 `vout_adc.c` / `vaux_adc.c` 的 `#include` 统一收进 `#if 0`，杜绝宏/声明冲突。
>
> **2026-06-20 进展（v8）—— PI 重构为增量式 + 闭环实测通过**：v7 位置式 PI 因"双重积分"导致
> bang-bang 饱和（VOUT<23.7V→120kHz，>24.7V→300kHz，中间死区）。**重构为增量式**：
> `delta_p = Kp×(error−prev_error)`（仅响应误差变化）、`delta_i = Ki×error`（每拍独立，不累积历史）、
> `period += delta_u`（单层累积），新增 Anti-Windup + Slew Rate 限制 + CMP4 下溢保护。**调参关键
> 发现**：增量式 PI 中 **Ki 必须 ≥ Kp×2**，否则误差趋向目标时 P 项"恢复力"反超 I 项，瞬态修正方向
> 反转（实测 Kp=1024/Ki=128 时空载直冲 30V）。最终 Kp=256(1.00)/Ki=512(2.00)，无死区
> （|error|≥1 码即输出），带载闭环稳定于 23.9V。空载 Burst Mode 待实现。详见 §5c。
>
> **2026-06-20 进展（v8.1）—— PI/OVP 移入 TIM3 ISR + 状态机瘦身**：PI_CTRL_Step() 从主循环
> SafeSM_Poll 移入 TIM3 10kHz ISR（1kHz 分频执行），消除主循环 printf 阻塞导致的控制节拍不确定。
> VOUT OVP 检测同步移入 ISR（VOUT 采样后立即比较），覆盖 SOFTSTART + RUN 全状态，不再局限于 RUN。
> OVP 触发留痕迹：`g_ovp_cnt` 计数器 + `Fault_Report_Poll` 边沿打印 + `[STAT]` 心跳可见。
> SafeSM_Poll 退化为纯安全调度（状态转移 + PVD/FLT 检测），不再执行控制算法。
> PI_CTRL_Step 删除内部限速器（`HAL_GetTick`/`PI_UPDATE_MS`/`last_update`），节拍由 ISR 分频保证。
> 命令行编译路径写入 CLAUDE.md。
>
> **2026-06-27 进展（v10）—— IOUT 输出电流采样 + PI 调参**：IOUT(ADC2/IN5/PC4)
> 加入 ADC2 DMA 扫描序列（与 VOUT 同为 Rank0/Rank1，TIM3 TRGO 硬件触发）；
> CubeMX ADC2 ContinuousConvMode 改为 DISABLE（每个 TRGO 触发一个完整扫描序列）；
> 全工程 `IOU→IOUT` 重命名；IOUT 换算参数 (R=20mΩ/G=50/Vofs=0, 3300/4095)；
> `[STAT]` 串口心跳增加 IOUT 字段；PI 调参：`PI_DECIMATION=10`(1→10 bugfix) +
> EWMA α 0.03 + Kp 0.5/0.3/0.15 + `PI_PERIOD_MAX=47000`(115.7kHz)。
>
> **2026-06-24 进展（v9）—— PI 浮点化重写 + VOUT ADC2 DMA 硬件触发**：
> **PI 控制器**：Q8.8 定点 → 全 float 实现（利用 M4F FPU）；`pi_ctrl_t` 结构体重构（mV/tick
> 物理单位 + 累计统计）；分段 Kp 新增 200 tick 滞回防 chattering；死区（±30mV）内持续更新
> prev_error 防退出冲击；Anti-Windup 正确禁 delta_i（而非操作不参与输出的 integral 累加器）；
> I 项浮点无截断（小误差也能产生非零增量）；OVP 快慢分离（ISR 置 g_fault_request + 直接关
> HRTIM 输出，主循环 SafeSM_Poll 补 DIS + 状态转移）；新增 PI_CTRL_GetDiagSnapshot()
> （PRIMASK 临界区保护）；9 个 _Static_assert 编译期检查。
> **VOUT ADC**：CubeMX 配置 TIM3 TRGO 硬件触发 ADC2 + DMA1_Channel1 循环搬运到
> `g_adc_dma_buf[0]`；`PI_CTRL_Step()` 15 步执行流（Step 0 OVP 10kHz → Step 1 分频 →
> Step 2~15 1kHz PI），直接从 DMA buffer 读取，不再软件轮询；buffer 定义在 adc_app.c、
> extern 引用在 pi_ctrl.c，所有权清晰。
> **HRTIM 写入统一**：`HRTIM_SetLLCPeriod()` 提取到 `freq_skip.c`，PI 和软启动共用。
> **首次闭环实测**：RLOAD=55Ω，VOUT 稳定在 ~24V（period≈47k/115kHz），PI err/dP/dI
> 响应正常。**发现 LLC 工作频率 115kHz 远低于原假设谐振点 130kHz**，真实 fr 可能在 135~145kHz；
> 匝比 8:1（24:3）+ 381V 母线 + 0.525V 二极管压降：Vout(fr)≈23.29V，需增益 1.03 才到 24V；
> 120kHz(PI_MAX=45300) 已无法满足 → 放宽至 50000(108.8kHz) → 进入容性区 → FLT3 OCP
> 反复触发（已知问题，见踩坑记录）。下一步：实测真实 fr + 修正 PI_PERIOD_MAX + FAULT 锁死。

## 目标硬件

| 项目 | 参数 |
|---|---|
| MCU | STM32G474RET6 |
| 内核 | Cortex-M4F（硬件浮点 FPv4-SP-D16） |
| 主频 | 170 MHz（实际代码用 **HSE 12 MHz** → PLLM=DIV3, PLLN=85, PLLR=DIV2；12/3×85/2=170；见 main.c `SystemClock_Config`）|
| Flash | 512 KB |
| RAM | 128 KB（Heap 512 B，Stack 1024 B） |

应用场景：**半桥 LLC 谐振变换器**驱动固件，采用 **PFM 变频控制**。已实现**增量式 PI 闭环 VOUT 稳压**（带载实测 23.9V 稳定）；空载 Burst Mode 待实现。

---

## 系统架构总览

```
main()
 ├─ SystemClock_Config()                 170 MHz
 ├─ MX_GPIO_Init()                        LED1/2/3 = PC1/PC2/PC3（开漏，低电平点亮）
 ├─ MX_HRTIM1_Init()                      PWM + 死区 + 3 路 Fault
 ├─ MX_DAC1/2/4_Init()                    比较器阈值源
 ├─ MX_ADC1/2_Init()                      ADC1=VAUX(PA1/IN2,规则组,原VOUT飞线)；ADC2=VOUT/IOUT/I_CYCLE
 ├─ MX_COMP2/4/6_Init()                   保护检测
 ├─ MX_USART1_UART_Init()                 调试串口（PB6/PB7）
 ├─ MX_TIM3_Init()                        10kHz 周期中断（驱动 VAUX 采样）
 ├─ [安全启动序列] 屏蔽 Fault → 启 COMP/DAC → 延时 → 清标志 → 使能 Fault
 ├─ LLC_SoftStart_Init()  ──┐
 ├─ Fault_IRQ_Enable()      │            使能 FLT 中断(IER)；NVIC 由 CubeMX MspInit 开
 └─ ADC_APP_Init()          │            统一 ADC 启动：自校准 + TIM3 10kHz 周期中断
                            ↓
        ⚠️ 注：上电不再无条件启 PWM/软启动；这些已移入 safe_sm 的 WAIT_AUX→SOFTSTART（见 §5c）

HRTIM1_Master_IRQHandler ─→ LLC_SoftStart_Step()   // 每次 MREP 中断扫频
   (stm32g4xx_it.c)            (App/freq_skip.c)

TIM3 (10kHz) ──TRGO脉冲──→ ADC2 硬件触发扫描 ──DMA──→ g_adc_dma_buf[0]=VOUT, [1]=IOUT
   │
   └──TIM3_IRQHandler ──→ HAL_TIM_PeriodElapsedCallback()             // 10kHz: VAUX轮询采样
        (stm32g4xx_it.c)     (App/adc_app.c)                          //       + IOUT DMA buffer 消费 + PI_CTRL_Step()(内部分频)

COMP2/4/6 ──(内部 Fault 线)──→ HRTIM Fault1/2/3 ──→ 硬件强制 PWM 输出 INACTIVE
                                      └─→ HRTIM1_FLT_IRQHandler → Fault_OnIRQ()  // 软件记录+点灯
                                            (App/fault_log.c)

while(1) ─→ SafeSM_Poll()          // 纯安全状态机：转移判断 + PVD/FLT 检测，不执行控制算法
              (App/safe_sm.c)
         ─→ Fault_Report_Poll()   // 故障边沿 + OVP边沿 + 每秒 [STAT] 心跳(含PI/OVP) + [REGS] dump
              (App/fault_log.c)
```

> **v8.1 架构变更**：PI_CTRL_Step() 从主循环移入 **TIM3 ISR**（与 VOUT 采样同节拍，1kHz 精确分频），
> 消除主循环 printf 阻塞导致的节拍不确定性。OVP 检测也移入 TIM3 ISR，覆盖 SOFTSTART+RUN 全状态。
> 主循环退化为纯诊断 + 安全调度，不再执行实时控制。

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

### 5b. 统一 ADC 采样模块（`App/adc_app.c/h` — 2026-06-14 整合）

> **v6.1**：原分散的 `vaux_adc.c/h` + `vout_adc.c/h` 整合为单一 `adc_app.c/h`。
> 所有 ADC 采样通道（VAUX/VOUT/I_CYCLE/IOUT）通过头文件 `#if` 开关集中管理，
> `HAL_TIM_PeriodElapsedCallback`（全工程唯一实现）在 `adc_app.c` 中。

**设计原则：**
- **通道切换纯改头文件**：每个通道采样前一律 `HAL_ADC_ConfigChannel` 重配寄存器，
  换通道只需改 `adc_app.h` 的 `CHANNEL`/`HANDLE` 宏，无需重生成 CubeMX。
- **条件编译开关**：`ADC_APP_ENABLE_VAUX=1`（默认开启），`ADC_APP_ENABLE_VOUT=1`（已启用），`I_CYCLE/IOUT` 默认 0，
  按需改为 1 即生效。
- **同一 ADC 多通道顺序采样**：每个通道独立 `ConfigChannel → Start → 自旋 EOC → Read → Stop`，
  块之间无耦合，增删互不影响。10kHz(100µs) 周期下 3 通道约 24µs，完全可接受。

**当前通道：**

| 通道 | 开关 | ADC/引脚 | 状态 | 用途 |
|------|------|----------|------|------|
| VAUX | `ADC_APP_ENABLE_VAUX=1` | ADC1/IN2/PA1 | ✅ 活跃 | 辅源 24V 采样 → 喂 SafeSM（22V 软封波 + 23V 重入）|
| VOUT | `ADC_APP_ENABLE_VOUT=1` | ADC2/IN12/PB2 | ✅ 活跃 | LLC 输出电压反馈 |
| I_CYCLE | `ADC_APP_ENABLE_ICYCLE=0` | ADC2/IN12/PB2 | 🔒 待启用 与 VOUT 互斥| 谐振腔电流 |
| IOUT | `ADC_APP_ENABLE_IOUT=0` | ADC2/IN5/PC4 | 🔒  | 输出电流 |

**旧文件状态：**
- `vaux_adc.c`：已 `#if 0` 禁用（全局变量 + 函数均停用），`vaux_adc.h` 保留供参数参考
- `vout_adc.c`：已 `#if 0` 禁用（2026-06-12），`vout_adc.h` 保留

**信号链（与旧版一致）：**
- **触发**：TIM3 10kHz 周期中断软件触发
- **滤波**：一阶 EWMA（α=1/8, τ≈0.8ms@10kHz），首样预置避免上电爬升误判
- **换算**：raw(0..4095) → 引脚 mV(`raw×3300/4095`) → ×分压比 → ×标定 GAIN/OFFSET → mV（浮点仅串口显示用）
- **安全链路**：VAUX 的 EWMA 滤波码直接整数比较 22V/23V 门限，浮点不参与控制

### 5c. PI 闭环控制器（`App/pi_ctrl.c/h` — 2026-06-20，v8 重构）

基于 VOUT ADC 反馈的 **增量式定点整数 PI 控制器**，通过调节 HRTIM 周期实现 PFM 调压闭环。

#### 背景：为什么旧版（位置式 PI）不行

v7 初版采用**位置式 PI**（position form），形式为：

```
P_term = (error × Kp) ÷ 256       ← 正比于误差绝对值
I_acc  += error × Ki               ← 积分累加
I_term = I_acc ÷ 256               ← 正比于累积误差
period  += P_term + I_term         ← period 自身是累积器
```

问题在于 **period 已经是一个累积器**（每次 `+=`），而 P_term 又是 error 的绝对值（也是累积量）。
数学上这等价于 `period = Σ Σ(error)` —— **两个积分器串在一起**，加上 I 项就是三重积分。
结果是：30~40 码的误差在 ~150ms 内就把 period 推到饱和限幅值，表现为 bang-bang：
VOUT<目标 → 立即冲到 120kHz（下限），VOUT>目标 → 立即冲到 300kHz（上限），中间几乎无过渡。

此外，位置式 PI 的 P 项即使误差不变也会每拍都输出相同值（`P=error×Kp`），相当于给积分器
又喂了一个恒定增量——即使误差已经很小且稳定，period 仍然在变。

#### 增量式 PI（Velocity Form）原理

v8 重构为**增量式 PI**，核心思想：**不计算"period 应该是多少"，而是计算"period 应该改变多少"**。

```
delta_p = ((error − prev_error) × Kp) ÷ 256    ← 仅响应误差的「变化量」
delta_i = (error × Ki) ÷ 256                    ← 每拍独立计算，不累积历史
delta_u = delta_p + delta_i                      ← 本轮总修正量
period  += delta_u                               ← 单层累积
prev_error = error                               ← 存起来给下拍用
```

**关键区别：** 误差不变 → `error == prev_error` → `delta_p = 0` → period 冻住。
只有误差变化时才产生 P 修正。这天然避免了"双重积分"。

**用具体数字走一遍（Kp=256, Ki=512）：**

假设软启动刚结束，period=40300(135kHz)，VOUT=25.3V，target=24V。误差约 −147 码（VOUT 偏高，需降 period 升频率来降压）。

| 拍数 | error | prev_error | delta_p 计算 | delta_p | delta_i 计算 | delta_i | delta_u | 新 period | 频率 |
|------|-------|------------|-------------|---------|-------------|---------|---------|-----------|------|
| 1 | −147 | 0 | (−147−0)×256÷256 | **−147** | −147×512÷256 | **−294** | −441 | 39859 | 136.5kHz |
| 2 | −110 | −147 | (−110−(−147))×1 = +37 | **+37** | −110×2 | **−220** | −183 | 39676 | 137.1kHz |
| 3 | −70 | −110 | (−70−(−110))×1 = +40 | **+40** | −70×2 | **−140** | −100 | 39576 | 137.5kHz |
| 4 | −30 | −70 | (−30−(−70))×1 = +40 | **+40** | −30×2 | **−60** | −20 | 39556 | 137.6kHz |
| 5 | −5 | −30 | (−5−(−30))×1 = +25 | **+25** | −5×2 | **−10** | +15 | 39571 | 137.5kHz |

解读：
- **第 1 拍**：prev_error=0（刚初始化），误差大 → delta_p + delta_i 双大 → 大力降 period（升频降压）。
- **第 2~4 拍**：VOUT 正在下降，误差减小。delta_p 变成 **正值**（因为 error 改善了 37 码），
  这是 P 项的"恢复力"——VOUT 在朝目标靠近，P 项往回拉防止过冲。但 delta_i（−220, −140, −60）
  仍然主导 → 净效果还是降 period，方向正确。
- **第 5 拍**：误差已经很接近 0（−5 码），delta_i=−10，P 项恢复力 +25 **反超** →
  period 微升。这就是为什么 **Ki 必须 ≥ Kp × 2**：在误差收敛阶段，I 项驱动力需要压过 P 项的恢复力。
- 如果 Kp 是 Ki 的 8 倍（如旧版 Kp=1024/Ki=128），第 2 拍 delta_p=+224、delta_i=−85，**净+139**——
  period 不降反升，VOUT 继续冲高，直到 30V。这就是空载过冲的根因。

**为什么最后几拍 period 不再变化：** 当 error → 0，delta_i → 0，error 也可能不再变 → delta_p → 0 →
delta_u = 0 → period 稳定。这就是收敛——系统找到了合适的稳态频率，不再振荡。

#### 保护机制（全部在 `PI_CTRL_Step()` 内）

| 层级 | 机制 | 触发条件 | 动作 |
|------|------|---------|------|
| 0 | OVP 过压 | `g_vout_filt > 27V 对应码` | 立即 `SafeSM_EnterFault()` 封波 |
| 1 | Slew Rate 限制 | `\|delta_u\| > PI_PERIOD_SLEW_MAX` | 钳到 ±1000 tick/次 |
| 2 | Period 硬限幅 | `period ∉ [PERIOD_MIN, PERIOD_MAX]` | 钳到边界 |
| 3 | Anti-Windup | period≥MAX 且 error>0（或 period≤MIN 且 error<0）| 冻结积分累加器 + delta_i=0 |
| 4 | 积分限幅 | `\|integral\| > INTEGRAL_MAX<<SHIFT` | 钳到 ±512000 |
| 5 | CMP4 下溢保护 | period ≤ 342 | CMP4=1（防 uint32_t 回绕）|

#### 当前工作参数（v8 终态）

| 宏 | 值 | 实际含义 |
|---|---|---|
| `PI_KP_INT` | **256** | Kp = 1.00 tick/ADC码（增量式 P 增益）|
| `PI_KI_INT` | **512** | Ki = 2.00 tick/ADC码（增量式 I 增益，无死区）|
| `PI_SHIFT` | 8 | Q8.8 定点移位 |
| `PI_VOUT_TARGET_MV` | 24000 | 24V 目标 |
| `PI_VOUT_OVP_MV` | 27000 | 27V 过压封波 |
| `PI_PERIOD_MIN` | 18133 | ≈300kHz 上限 |
| `PI_PERIOD_MAX` | ~49455（用户调） | ≈110kHz 下限（带载需足够增益裕量）|
| `PI_PERIOD_SLEW_MAX` | 1000 | 单次 delta_u 上限 |
| `PI_INTEGRAL_MAX_TICK` | ±2000 | 积分累加器硬限幅 |
| `PI_UPDATE_MS` | 1 | 1kHz 控制率 |

#### `[STAT]` 串口诊断

每秒打印 `PI err=%ld P=%ld I=%ld`，其中：
- **err**：当前误差（ADC 码），正=VOUT 偏低需降频增增益，负=VOUT 偏高需升频降增益
- **P**：本拍 delta_p（tick），误差变化大时值大，误差稳定时为 0
- **I**：本拍 delta_i（tick），正比于当前误差

> 注意：v8 的 P/I 是**单次增量**而非累积值，正常情况下在 −100~+100 范围波动，
> 误差趋零时双双趋零。如果 P 和 I 持续为 0 但 err 非 0 → 参数可能需调整。

#### 状态机集成（不变）

- `SOFTSTART→RUN` 转移时 `PI_CTRL_Init()`（清零 integral / prev_error / last_update）
- `SAFE_RUN` 内 `PI_CTRL_Step()`（每次主循环迭代，内部 1kHz 限速）
- FAULT 时 `SafeSM_EnterFault()` 封波停止调节

### 6. ARM CMSIS-DSP 数学库

- X-CUBE-ALGOBUILD 1.4.0，预编译 `libarm_cortexM4lf_math.a`（硬浮点 Cortex-M4F）
- 编译宏：`ARM_MATH_CM4` + `ARM_MATH_LOOPUNROLL`

### 7. 开发环境

| 项目 | 说明 |
|---|---|
| 工具链 | `arm-none-eabi-gcc`，硬浮点 `-mfloat-abi=hard -mfpu=fpv4-sp-d16` |
| 构建 | CMake + Ninja，IDE `Ctrl+Shift+B` 一键编译烧录 |
| 调试器 | DAPLink（CMSIS-DAP），OpenOCD `D:/openocd/daplink.cfg` |
| 烧录 | tasks.json `Flash: OpenOCD`，编译后自动 program/verify/reset |
| 调试 | launch.json `STM32G474 Debug`，F5 启动 Cortex-Debug |

**命令行编译（非 IDE）：**

```powershell
# 设置工具链 PATH（按实际版本调整）
$env:PATH = "C:\Users\ywjAv\AppData\Local\stm32cube\bundles\gnu-tools-for-stm32\14.3.1+st.2\bin;" +
            "C:\Users\ywjAv\AppData\Local\stm32cube\bundles\cmake\4.3.1+st.1\bin;" +
            "C:\Users\ywjAv\AppData\Local\stm32cube\bundles\ninja\1.13.2+st.1\bin;" +
            $env:PATH

# 新增 .c 文件后需重新 configure（CMake file(GLOB) 在 configure 时评估）
cmake --preset Debug

# 日常增量编译
cmake --build build/Debug
```

> **注意**：CMakeLists.txt 用 `file(GLOB App/*.c)` 收集源文件，该 glob 在 **configure 时** 评估。
> 新增 .c 文件后必须重新 `cmake --preset Debug`，否则链接报 `undefined reference`。
> 仅修改已有文件时直接 `cmake --build build/Debug` 即可。

---

## 中断服务函数（ISR）

| ISR | 触发条件 | 作用 |
|---|---|---|
| `HRTIM1_Master_IRQHandler` | HRTIM Master 重复事件 MREP（每 4 个 PWM 周期）| 清 MREP 标志 → `LLC_SoftStart_Step()` 扫频 |
| `HRTIM1_FLT_IRQHandler` | HRTIM Fault1/2/3（COMP2/4/6 越限）| `Fault_OnIRQ()`：记录通道/次数/时刻、点亮对应 LED、关本路中断防风暴 |
| `TIM3_IRQHandler` | TIM3 更新事件（10 kHz）| `HAL_TIM_PeriodElapsedCallback()` → 统一 ADC 采样（VAUX+VOUT+…）+ EWMA + 喂 safe_sm（`App/adc_app.c`）|
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
| PI_KP_HIGH | 0.5 | tick/mV | pi_ctrl.h | 高频段（period < SEG1=24000/~227kHz）|
| PI_KP_MID | 0.3 | tick/mV | pi_ctrl.h | 中频段（SEG1~SEG2=24000~36000）|
| PI_KP_LOW | 0.15 | tick/mV | pi_ctrl.h | 低频段（period > SEG2=36000/~151kHz）|
| PI_KI | 0.05 | tick/(mV·ms) | pi_ctrl.h | 积分增益（控制周期固定 1ms）|
| PI_VOUT_TARGET_MV | 24000.0 | mV | pi_ctrl.h | 目标输出电压 |
| PI_VOUT_OVP_MV | 28000.0 | mV | pi_ctrl.h | VOUT 过压保护阈值 |
| PI_PERIOD_MIN | 18133.0 | tick | pi_ctrl.h | 300kHz 上限（周期下限）|
| PI_PERIOD_MAX | 47000.0 | tick | pi_ctrl.h | 115.7kHz 下限（v10；fr之下 M>1，距容性区~6kHz 余量）|
| PI_SLEW_MAX | 300.0 | tick/次 | pi_ctrl.h | 单次 delta_u 上限 |
| PI_DEADBAND_MV | 30.0 | mV | pi_ctrl.h | 死区 ±30mV |
| PI_EWMA_ALPHA | 0.03 | — | pi_ctrl.h | VOUT EWMA 滤波系数（τ≈33ms@1kHz）|
| PI_DECIMATION | 10U | — | pi_ctrl.h | 10kHz→1kHz 分频比 |
| PI_SEG_HYST | 200.0 | tick | pi_ctrl.h | Kp 分段滞回带宽度 |
| ADC2 DMA buffer | `g_adc_dma_buf[2]` | — | adc_app.c | ADC2 扫描序列 DMA 循环：[0]=VOUT(CH12), [1]=IOUT(CH5) |
| IOUT 采样电阻 | 20 | mΩ | 硬件 | Rshunt |
| IOUT 运放增益 | 50 | 倍 | 硬件 | G, Voffset=0 |
| IOUT 换算 | 3300/4095 | — | adc_app.h | `g_iout_ma = raw × SCALE_NUM / SCALE_DEN` |

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

### re-arm 重入 SOFTSTART 前几拍漏旧 period（2026-06-13，已修）
- 现象：CH1=DIS、CH2=PWM。DIS 放行后 PWM 前 1~3 拍约 140k，到第 5~6 拍才变 300k；冷启动正常，仅 re-arm（FAULT→WAIT_AUX→SOFTSTART，**MCU 未掉电、HRTIM 未复位**）这条路径有。
- 真因：关断前 RUN 定频(~140k)，有效 MPER 残留旧值；`LLC_SoftStart_Init` 用 `__HAL_HRTIM_SETPERIOD` 写的是**预装载**寄存器，旧 active MPER 要等下一个更新事件(MREP，`RepetitionCounter=3` → 每 4 拍)才被 18133 替换；而输出在 latch 之前就被 `WaveformOutputStart` 使能 → 前几拍用旧 140k。叠加 `SafeSM_EnterFault()` 只停输出、**没停计数器**（FAULT 期间 Master/TimerA 还在以旧周期空跑），残留更确定。
- 修复（仅改 `LLC_SoftStart_Init` 入口时序，不动扫频算法/方向）：① 复位扫频变量到 300k；② 停计数器(`WaveformCountStop`)拿确定起点；③ 写预装载 18133；④ `HAL_HRTIM_SoftwareUpdate`(强制预装载→有效寄存器立即生效) + `HAL_HRTIM_SoftwareReset`(清零 CNT)，并轮询 `CR2` 的 `MSWU/TASWU/TCSWU` 自清确认 latch 完成；⑤ **确认生效后**才 `WaveformOutputStart` + `WaveformCountStart(MASTER|TIMER_A)`。
- 关键点：杜绝"先使能输出、period 随后才更新"；该"预加载→强制 latch→清零计数→再使能"的顺序对冷启动与 re-arm 两条路径都适用。验证：re-arm 后 CH2 第一个完整周期即 ≈300k（最密），随后逐拍变疏单调降到 140k。
- 注：`LLC_SoftStart_Step` 的 `static skip_cnt` 未在 Init 里归零——只影响"首次降频步进时机"（早/晚几拍），不影响起始 300k 值，有意不动以守住"只改一处"。

### re-arm 起振首拍高电平偏宽（频率已对，2026-06-13 续修）
- 现象：上一条修复后频率已是 300k（实测 304.9k），但**偶发第一拍高电平偏宽**，次拍起规整。
- 真因（两点叠加，均在热重启路径）：
  ① **TimerA `InterleavedMode=DUAL` → 硬件自动 CMP1=PER/2**（导通时间=半周期；全工程从不手写 TimerA CMP1）。该 CMP1 在"PER 更新事件"时刻重算；若首个 `MASTERPER` 置位发生在 CMP1 还停留在旧 PER/2(≈19550) 的窗口，而新周期仅 18133<19550 → TA1 第一周期内**等不到复位点** → 首拍高电平拖到次拍（≈3 倍宽）。
  ② `WaveformOutputStop` 后 TA1 的 **SR 锁存残留态**，重使能时若残留为高也会让首拍偏宽。"偶发"=相位+残留竞态。
- 修复（仍只动 `LLC_SoftStart_Init` 入口，不碰扫频/占空算法）：① 把 `SoftwareReset(CNT=0)` 调到 `SoftwareUpdate` **之前**——在 CNT=0 干净点做更新，使 period 与 interleaved 自动 CMP1=PER/2 在**同一更新事件**里一起 latch；② 使能输出前用 `HAL_HRTIM_WaveformSetOutputLevel(TA1/TC1 → INACTIVE)` 把主通道 SR 锁存强制清失活（互补 TA2/TC2 由死区单元派生，无需也无法单独强制）。
- 验证：热重启起振后第一拍的频率与高电平宽度即与第二拍一致（300k、占空正常），多次复现不再偏宽。

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

### 位置式 PI 的"双重积分"导致 bang-bang 饱和（2026-06-20，v7→v8）
- 现象：VOUT<23.7V → 频率立即冲至 120kHz（PI_PERIOD_MAX），VOUT>24.7V → 立即冲至 300kHz（PI_PERIOD_MIN）。中间电压频率几乎不变，完全没有平滑调节。
- 真因：旧代码 `P_term = (error × Kp) >> SHIFT`（位置式，正比于 error 绝对值）+ `llc_period += P_term + I_term`（period 自身是累积器）。数学上等价于 `period = Σ(error + Σ(error))` —— P 项给 period 累积器又喂了一个与 error 成正比的量，形成**双重积分**。30~40 码的误差在 ~150ms 内就把 period 推到饱和值。
- 修复：重构为增量式 PI（velocity form）。`delta_p = Kp × (error − prev_error)`（仅响应误差变化量，误差不变则 delta_p=0）+ `delta_i = Ki × error`（每拍独立，不累积历史）+ `period += delta_u`。误差稳定时 delta_p=0，period 冻住，天然避免双重积分。详见 §5c 增量式 PI 原理。

### PI_PERIOD_MAX 过低导致 LLC 进入容性区 → FLT3 过流反复触发（2026-06-23，v8.2）

- **现象**：上电闭环测试中 FLT3(COMP6/PB11, OCP) 反复触发。FAULT 后输出关闭 → VAUX≥23V → 自动跳 WAIT_AUX → 50ms 后清锁存重开 PWM → FLT3 立即再次触发 → 死循环。
- **调试手法**：关闭 VOUT 采样（`ADC_APP_ENABLE_VOUT=0`，同时切断 PI_CTRL_Step 编译和 VOUT OVP 检测）→ 系统回到纯开环 300k→139.5kHz 定频。开环测试 FLT3 **不再触发**，确认问题与 PI 闭环相关，非硬件噪声。
- **根因**：`PI_PERIOD_MAX = 50000`（≈108.8kHz），而 LLC 谐振频率 ≈130kHz（period≈41846）。PI 检测到 VOUT 偏低时增大 period（降频靠近谐振点增增益），若负载较重，PI 可把频率一路压到 108kHz——**远低于谐振点，进入容性区**，引发：
  - 硬开关（ZVS 丢失）→ 原边电流尖峰
  - PB11（原边电流采样）超过 COMP6 阈值 2.82V → FLT3 过流保护正确触发
  - 开环频率 139.5kHz(period=39100) 在谐振点之上感性区，ZVS 正常工作，故不触发
- **附带发现**：
  1. `FAULT` 状态机出口逻辑有误：`SAFE_FAULT` 在 `g_vaux_filt >= VAUX_REARM_CODE` 时无条件跳 `SAFE_WAIT_AUX`，原意是为 VAUX 欠压(FLT1)恢复留重启路径，但实际上对 FLT2/FLT3(OCP/OVP) 也放行，造成"故障→自动清锁存→重启→再次故障"的死循环。**正确做法**：FAULT 应锁死无软件出口，仅掉电冷启恢复。
  2. `main.c:169-170`：`HAL_DAC_Start(&hdac4, DAC1_CHANNEL_2)` 和 `HAL_DAC_SetValue(&hdac4, DAC1_CHANNEL_2, ...)` 写错了通道宏（应为 `DAC4_CHANNEL_2`）。由于 STM32G4 HAL 中 `DAC1_CHANNEL_2` 与 `DAC4_CHANNEL_2` 恰好同值 `DAC_CHANNEL_2=0x10`，功能上碰巧正确，但属复制粘贴错误。
  3. `comp.c:100`：COMP6 迟滞仍为 `COMP_HYSTERESIS_20MV`，而 COMP2 已升到 40mV。PB11 高阻模拟输入，噪声容限偏低。
  4. `main.c` 上电时序：`HAL_COMP_Start(&hcomp6)` 早于 `HAL_DAC_SetValue(&hdac4, ..., 3500)`，COMP6 先启动时 DAC 输出可能为不确定值。虽然此时 Fault 整体已屏蔽，但顺序上应先设 DAC 阈值再启 COMP。
- **修复方向**：
  1. `PI_PERIOD_MAX` 从 50000 修改为 40000（≈136kHz），保留谐振点之上安全余量（≥6kHz 裕度）
  2. `SAFE_FAULT` 状态改为锁死无出口（删掉 `g_vaux_filt >= VAUX_REARM_CODE` 的跳转），仅掉电冷启恢复
  3. （可选）`main.c` DAC4 通道宏修正、COMP6 迟滞升 40mV、上电时序调整

### LLC 硬件参数与谐振点实测（2026-06-24，v9 闭环调试）

- **工况**：55Ω 负载，24V 目标 ≈ 10.5W。软启动 300k→135kHz 后 PI 接手闭环。

- **关键硬件参数**：
  - 变压器匝比：Np:Ns = 24:3 → N = 8:1
  - 半桥母线电压 Vbus = 381V
  - 副边二极管导通压降 Vf ≈ 0.525V（推测为 SiC SBD）
  - 设计串联谐振点 fr = 130kHz

- **谐振点理论输出电压**：
  ```
  Vout(fr, M=1) = Vbus / (2 × N) − Vf
                = 381 / 16 − 0.525
                = 23.81 − 0.525
                = 23.29V
  ```
  理论值差 0.71V 到 24V，需额外增益 1.03。

- **实测运行点**：
  - `PI_PERIOD_MAX = 45300(120kHz)`：VOUT 仅能到 23.5V，增益不够
  - `PI_PERIOD_MAX = 50000(108.8kHz)`：VOUT 可达 24V，但稳态运行在 ~47k(115kHz)
  - 120kHz 时增益 > 1（VOUT=23.5V > 23.29V 理论值）→ **120kHz 已低于真实 fr**
  - 推测真实 fr ≈ 135~145kHz，远高于原设计 130kHz

- **FLT3 反复触发链条**：
  ```
  负载瞬态 → VOUT 下跌 → PI 增 period（降频）
  → period → 50000(108.8kHz)
  → 穿过真实 fr → 进入容性区 → ZVS 丢失 → 硬开关
  → 原边电流尖峰 → COMP6/PB11 超阈值 → FLT3 锁死
  ```
  FAULT 后 VAUX≥23V → 自动重启 → PI 再次推 period → 再次 FLT3（每 ~3s 循环一次）

- **根因总结**：
  1. 真实 fr 可能比设计值 130kHz 高 10~15kHz（Cr/Lr 实际偏差或 MLCC DC bias）
  2. 匝比 8:1 在 381V 母线 + 二极管压降下，谐振点增益 1 时理论 VOUT 仅 23.29V
  3. `PI_PERIOD_MAX=50000` 允许频率降到 fr 以下 → 容性区
  4. `SAFE_FAULT` 自动重启机制让 OCP 故障陷入死循环

- **下一步**：
  - 空载开环扫频，示波器看原边电流与电压同相点 → 确认真实 fr
  - `PI_PERIOD_MAX` 设在真实 fr 之上 3~5kHz
  - FAULT 状态改为锁死不自动恢复
  - 如 VOUT 真实 fr 下仍不够 24V，需改匝比或调母线电压

### 增量式 PI 的 Kp/Ki 比例约束（2026-06-20，v8 调参）
- 现象：Kp=1024/Ki=128 时，空载上电 VOUT 直冲 30V（过压），而带载后又能稳定。示波器看频率：前几拍正确升频，随后**方向反转**（period 不降反升）。
- 真因：增量式 P 项 `delta_p = (Δerror × Kp)` 在误差缩小时产生**正值**（"恢复力"）。当 VOUT 从高空向 target 回落，error 从 −200 变到 −150（改善 50 码），Kp=1024 时 `delta_p = +200`（反向推 period ↑），而 `delta_i = −150 × 0.5 = −75`（正向推 period ↓），净 `delta_u = +125`——**period 不降反升，频率降低，VOUT 继续冲高**。带载时负载自身消耗能量缓冲了过冲，故现象不如空载明显。
- 解决：Ki/Kp ≥ 2:1（最终 Kp=256/Ki=512）。此时同样场景：`delta_p = +50`，`delta_i = −300`，净 `delta_u = −250`，方向始终正确。**核心原则：增量式 PI 中 I 项驱动力必须 ≥ P 项恢复力的 2 倍，否则瞬态修正方向可能反转。**
- 空载过冲的根治方案不是再调 PI 参数，而是实现 **Burst Mode**（VOUT>OVP 阈值时直接间歇停波），因为空载时即使 period 推到 300kHz（PI_PERIOD_MIN），LLC 增益仍然太高，PI 已经饱和无力降压。

---

### 5c. 辅助电源(VAUX)安全监测 + 安全重入状态机（`App/safe_sm.c` / `adc_app.c`）

**动机（事故）**：连续多次重启时独立辅助电源缓慢衰减，MCU 在欠压/不确定态仍输出 PWM，再次
上电瞬间疑似半桥误开通 → MOS 全部击穿。本子系统消除这个"重启窗口"。

**硬件前提（已飞线，软件按此适配，不新增通道/不改分压）**：
- 原采样 VOUT 的 **ADC1_IN2 = PA1** 已飞线改接辅助电源 **24V 轨**（经原分压，比例 10:1 不变）。
- 同一分压点并接 **COMP2 的 IN+ = PA3**，IN− = DAC1_CH2，做 21V 硬件欠压闸。
- 分压比 / 伏特↔ADC码↔DAC码 全部沿用原 VOUT 换算（现统一在 `adc_app.h` 中，`VAUX_MV_TO_CODE` 宏），不重算。

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
1. **软件优雅封波**：10kHz ISR(`adc_app.c`) 读 VAUX → 一阶 EWMA(α=1/8,τ≈0.8ms) → `SafeSM_OnSample()`；
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
| 闭环控制 | `App/pi_ctrl.c` | ✅ 已实现：float 增量式 PI，分段 Kp 滞回 + 死区 + Anti-Windup + Slew Rate，1kHz ISR 驱动 |
| VOUT ADC DMA | `adc_app.c` / CubeMX | ✅ 已实现：TIM3 TRGO 硬件触发 ADC2 + DMA1_Ch1 Circular → `g_adc_dma_buf[0]` |
| HRTIM 统一写入 | `freq_skip.c` | ✅ 已实现：`HRTIM_SetLLCPeriod()` 供 PI 和软启动共用 |
| VOUT 采样 | `adc_app.c/h` | ✅ 统一模块：VAUX(ADC1/PA1) 活跃，VOUT(ADC2 DMA) 活跃，I_CYCLE/IOUT 待启用 |
| **真实谐振频率 fr 未知** | 硬件 | ⚠️ 设计值 130kHz，实测工作点 ~115kHz 暗示真实 fr 可能在 135~145kHz。需空载扫频确认 |
| **FAULT 自动重启死循环** | `safe_sm.c` | ⚠️ FLT3 OCP 触发后 VAUX≥23V 自动重启 → PI 再推 period 到 50000 → 再触发。FAULT 应锁死 |
| **PI_PERIOD_MAX 需修正** | `pi_ctrl.h` | ⚠️ 当前 50000(108.8kHz)，低于谐振点。确认真实 fr 后应设在其之上 3~5kHz |
| VAUX 标定 | `adc_app.h` | ⚠️ `ADC_VAUX_CAL_GAIN/OFFSET` 默认未校正 |
| 故障恢复 | `fault_log.c` | 已有 `Fault_Rearm()`，无自动恢复（对 OCP/OVP 有意）；尚无触发入口（如串口指令） |
| Fault 消抖 | `hrtim.c` | ✅ 已用 `FAULTFILTER_9` + COMP2 迟滞 40MV；COMP4/6 仍 20MV + PB11 RC 待补 |
| Burst Mode（空载防过冲）| `pi_ctrl.c` | 🔒 待实现 |
| 电流闭环/双环 | `adc_app.c` | 🔒 待 I_CYCLE/IOUT 硬件接线 + 启用 |

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
│   ├── adc_app.c/h        【v6.1】统一 ADC 采样（VAUX/VOUT/I_CYCLE/IOUT 条件编译，#if 开关集中管理）
│   ├── safe_sm.c/h        辅源安全监测 + 安全重入状态机（门限/状态机/enter_fault/PVD/BOR）见 §5c
│   ├── pi_ctrl.c/h        【v7】PI 闭环控制器（定点 Q8.8，1kHz，VOUT 稳压 PFM 调压）
│   ├── vaux_adc.c/h       【已停用·保留】原 VAUX 采样，已迁移至 adc_app.c（#if 0）
│   ├── vout_adc.c/h       【已停用·保留】原 VOUT 采样，已迁移至 adc_app.c（#if 0）
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
- **v3**：新增 `fault_log` 故障中断记录（`HRTIM1_FLT_IRQHandler` → `Fault_OnIRQ`，三路 LED 指示 + `Fault_Rearm` 恢复）；引脚重排（LED1/2/3=PC1/2/3，ADC 增配 VOUT/IOUT/I_CYCLE + ADC2）。
- **v4（2026-06-06）**：串口诊断上报 `Fault_Report_Poll`（故障详情 + `[STAT]` + `[REGS]`）；新增 `vout_adc`（ADC1 规则组 VOUT + TIM3 10kHz 触发 + 标定校正）；串口 USART3→**USART1(PB6/PB7)**；当日完成多项硬件调试（PB12/13 短路、PB11 噪声误触发、串口共地、printf 全缓冲）。
- **v5/v6（2026-06-12/13）**：辅源安全监测 + 安全重入状态机（`safe_sm.c`）；VAUX 采样(原 VOUT/PA1 飞线改接 24V 轨)；21V/22V/23V 三层防护；PVD + BOR 代码自配置；IWDG 看门狗；软启动 re-arm 修复；CubeMX 待办全部落地。
- **v6.1（2026-06-14）**：统一 ADC 采样模块 `App/adc_app.c/h` 整合 VAUX/VOUT/I_CYCLE/IOUT；条件编译 `#if` 开关 + 软件 `ConfigChannel` 重配，通道切换纯改头文件；vaux_adc/vout_adc 旧文件 `#if 0` 保留。
- **v6.2（2026-06-17）**：VOUT 硬件改接 ADC2/IN12/PB2 并正式启用；VOUT 采样数据加入 `[STAT]` 串口心跳；旧文件 `#include` 收进 `#if 0` 杜绝冲突；确认 10kHz ISR 余量充足（~21µs/100µs），PI 闭环可在此基础上直接加入。
- **v7（2026-06-20 上午）**：PI 闭环控制器 `pi_ctrl.c/h` 初版（位置式 PI：`period += P_term + I_term`，定点 Q8.8，1kHz）；集成到安全状态机 `SAFE_RUN`；`[STAT]` 心跳追加 PI err/P/I 诊断。**问题**：位置式"双重积分"导致 bang-bang 饱和（VOUT 偏差几十 mV → period 直冲限幅值）。
- **v8（2026-06-20 下午）**：**PI 重构为增量式（velocity form）**。`delta_p = Kp×(error−prev_error)` + `delta_i = Ki×error`，`period += delta_u`，消除双重积分。新增 Anti-Windup、Slew Rate 限制、CMP4 下溢保护、寄存器写入顺序修复。调参：Kp=256(1.00)/Ki=512(2.00)，Ki/Kp=2:1 保证方向始终正确。**带载闭环实测**：VOUT 稳定于 23.9V（`PI_PERIOD_MAX` 扩至 ~110kHz）。
- **v8.1（2026-06-20 晚间）**：**PI/OVP 移入 TIM3 ISR + 状态机瘦身**。PI_CTRL_Step 从主循环移入 10kHz ISR（1kHz 分频），删除内部限速器；OVP 检测同步移入 ISR，覆盖全状态，留痕迹（g_ovp_cnt + 边沿打印）；SafeSM_Poll 退化为纯安全调度。命令行编译路径固化到 CLAUDE.md。
- **v9（2026-06-24）**：**PI 浮点化重写 + VOUT ADC2 DMA 硬件触发**。PI 全部改用 float（分段 Kp 滞回/死区/浮点无截断 I/Anti-Windup 修复）；VOUT 改为 TIM3 TRGO 硬件触发 ADC2 + DMA 循环搬运，ISR 直接读 buffer；`HRTIM_SetLLCPeriod()` 统一写入；`g_fault_request` OVP 快慢分离。**首次闭环实测**：55Ω 负载下 VOUT 稳定 ~24V，工作频率 ~115kHz（period≈47k）远低于设计谐振点 130kHz；PI_PERIOD_MAX=50000(108.8kHz) 下 FLT3 反复触发（容性区 ZVS 丢失）。调试发现真实 fr 可能在 135~145kHz，匝比 8:1 + 381V 母线 + 0.525V 二极管压降 → Vout(fr)≈23.29V 理论上不够 24V —— 问题在 LLC 增益而非 PI 控制。下一步：实测真实 fr + 修正 PI_PERIOD_MAX + FAULT 锁死不自动重启。
- **v10（2026-06-27，当前）**：**IOUT 输出电流采样 + PI 调参 + ADC2 完善**。IOUT(ADC2/IN5/PC4) 加入 ADC2 DMA 扫描序列（与 VOUT 同为 Rank0/Rank1，TIM3 TRGO 硬件触发，解决焊接冷焊问题后验证通过）；CubeMX `ADC2 ContinuousConvMode→DISABLE`（每个 TRGO 仅触发一次扫描，采样率精确 10kHz）；IOUT 换算参数填入（R=20mΩ/G=50/Vofs=0, `raw×3300/4095`）；全工程 `IOU→IOUT` 重命名（变量 `g_iout_*`、宏 `ADC_IOUT_*`、注释）；`[STAT]` 心跳增设 IOUT 字段（raw/filt/mA）；PI 调参：`PI_DECIMATION=10`（修复 1→10 bug，PI 真正运行在 1kHz）、EWMA α 0.03、Kp 0.5/0.3/0.15（降噪）、`PI_PERIOD_MAX=47000`(115.7kHz)。

---

## 当前状态与下一步

> **已完成**：
> - PA8/PA9、PB12/PB13 互补 PWM，180° 移相，死区 250 ns ✓
> - 300→139.5 kHz 变频软启动（中断驱动，含 re-arm 修复）✓
> - 三路比较器硬件 OCP/OVP/VAUX_UVP 保护（锁死型 + Filter_9 + 迟滞）✓
> - 故障软件记录 + **串口上报** `Fault_Report_Poll`（故障详情 + `[STAT]` + `[REGS]`）✓
> - **辅源安全监测 + 安全重入状态机**（VAUX 22V 软封波 + COMP2/Fault1 21V 硬兜底 + 23V 重入 + PVD/BOR/IWDG）✓
> - **统一 ADC 采样模块** `adc_app.c/h`（VAUX 活跃；VOUT/I_CYCLE/IOUT 条件编译就绪）✓（v6.1）
> - **VOUT 通道正式启用**（ADC2/IN12/PB2，`ADC_APP_ENABLE_VOUT=1`）✓（v6.2）
> - **PI 闭环控制器（增量式，float，ISR 驱动）**：分段 Kp 滞回 + 死区 + Anti-Windup + Slew Rate + OVP 快慢分离 ✓（v9）
> - **VOUT ADC2 DMA 硬件触发**：TIM3 TRGO → ADC2 扫描序列 → DMA Circular → `g_adc_dma_buf[0..1]`，ISR 直接读 ✓（v9/v10）
> - **IOUT 输出电流采样**：ADC2 DMA 序列 Rank1，`[STAT]` 心跳打印，已硬件验证（0V/3.3V 正确）✓（v10）
> - **PI_DECIMATION bugfix**：1→10，PI 真正运行在 1kHz（原 10kHz 导致过调振荡）✓（v10）
> - **PI 降噪调参**：Kp 0.5/0.3/0.15 + EWMA α=0.03，噪声从 ±275mV 降至 ±135mV ✓（v10）
> - **CubeMX 完善**：ADC2 `ContinuousConvMode=DISABLE`，采样率精确由 TIM3 TRGO 控制 ✓（v10）
> - **首次闭环实测通过**：55Ω 负载 VOUT 稳定 ~24V，PI 响应正常 ✓（v9）
>
> **下一步（按优先级）**：
> 1. **实测真实 fr**：空载开环扫频（示波器看原边电流/电压同相点），确认真实串联谐振频率。当前数据暗示 fr 在 135~145kHz，而非设计值 130kHz
> 2. **修正 PI_PERIOD_MAX**：确认真实 fr 后，`PI_PERIOD_MAX` 设在 fr 之上 3~5kHz 安全余量，防止 PI 将频率推入容性区
> 3. **FAULT 锁死不自动重启**：`SAFE_FAULT` 状态删掉 `g_vaux_filt >= VAUX_REARM_CODE` 的跳转，仅掉电冷启恢复（防止 OCP 死循环）
> 4. **Burst Mode（空载防过冲）**：空载/轻载时 LLC 增益极高，即使 PI 升到 300kHz 也无法降压 → 需间歇停波
> 5. **电流闭环/双环**：已具备 IOUT 采样基础，可探索电流内环 + 电压外环
> 6. **抗扰加固（可选）**：COMP4/6 迟滞（+ PB11 硬件 RC）——补强原 OCP/OVP 噪声裕量

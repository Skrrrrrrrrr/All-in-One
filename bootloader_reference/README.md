# STM32F407 Bootloader 部署教程

本文档讲解如何将 `bootloader_reference.c` 部署为独立的 Bootloader 工程，配合 App 侧的 OTA 模块实现 A/B 分区升级与自动回滚。

## 1. 背景与设计目标

本工程是 App 侧程序（无独立 Bootloader）。`bootloader_reference.c` 提供参考实现，用于：

- **A/B 双区引导**：根据 OTA 参数区（S11 扇区）中的启动标志，决定引导 App A 区还是 App B 区
- **自动回滚**：新固件连续启动失败（如看门狗复位）超过 `BL_MAX_BOOT_ATTEMPTS`(3次) 后，自动引导旧固件
- **防变砖底线**：目标镜像非法时回退另一区，双区均非法时死循环等待恢复

> 重要：`bootloader_reference.c` 是**独立参考工程**，**不参与 App 编译**。请将其复制到单独的 Bootloader 工程中使用。

## 2. Flash 分区布局（内部 1MB）

| 分区 | 起始地址 | 大小 | 扇区 | 用途 |
|------|---------|------|------|------|
| Bootloader | 0x08000000 | 64KB | S0-S3 | 引导程序（本工程） |
| App A区 | 0x08010000 | 448KB | S4-S7 | 应用镜像 A（出厂默认活跃区） |
| App B区 | 0x08080000 | 384KB | S8-S10 | 应用镜像 B（升级目标区） |
| OTA参数区 | 0x080E0000 | 128KB | S11 | 启动标志+固件信息（双槽冗余） |

分区宏定义见 `ota_flash_port.h`，Bootloader 与 App 必须使用**同一份** `ota_flash_port.h` 保证地址一致。

## 3. 引导流程

```
上电复位
  │
  ▼
bootloader_entry()
  ├─ 读参数区（Slot1 优先 → Slot0 回退 → 全无效用默认A区并回写）
  │
  ├─ 有 pending_slot（新固件待验证）?
  │     ├─ 是，boot_attempts < 3 → boot_attempts++ 写回 → 引导 pending_slot
  │     ├─ 是，boot_attempts >= 3 → 判定新固件失败：清除pending → 回滚引导 active_slot
  │     └─ 否 → 引导 active_slot
  │
  ├─ bl_app_valid(目标地址)？
  │     ├─ 非法 → 回退另一区再校验
  │     └─ 双区非法 → 死循环等待恢复
  │
  └─ bl_jump_to_app()
        关中断 → 重定位 VTOR → 设置 MSP → 跳转 Reset Handler
```

### 3.1 正常引导（无升级）
参数区 `pending_slot = 0xFF`，直接引导 `active_slot` 指向的分区。

### 3.2 升级引导（有 pending）
App 完成升级并 `ota activate` 后，参数区记录 `pending_slot = 目标区`。Bootloader 每次上电：

1. 若 `boot_attempts < 3`：`boot_attempts++` 并写回参数区，然后引导**新固件**
2. 若 `boot_attempts >= 3`：说明新固件连续 3 次启动失败，**清除 pending、复位尝试计数**，引导**旧固件**

### 3.3 确认与转正
新固件正常运行后，App 调用 `ota_confirm_ok()`：将 `active_slot` 更新为当前运行区，清除 pending。之后 `active_slot` 即指向新分区，下次升级将以新分区为"活跃区"。

## 4. 回滚机制原理

回滚依赖**三个要素**的配合：

| 要素 | 位置 | 作用 |
|------|------|------|
| `boot_attempts` 计数 | OTA参数区 | Bootloader 每次引导新固件前 +1 |
| 看门狗（IWDG） | App侧 | 新固件启动后必须在窗口期内喂狗，否则复位 |
| `BL_MAX_BOOT_ATTEMPTS` | Bootloader | 尝试次数上限（3次） |

**典型失败场景**：升级后新固件启动即 HardFault / 死循环 → 看门狗复位 → Bootloader 再次引导新固件（attempts=2）→ 再次复位（attempts=3）→ 第 4 次上电 attempts>=3 → 回滚旧固件。

## 5. 向量表合法性校验（bl_app_valid）

跳转前校验 App 向量表前两个词：

```c
msp            = *(uint32_t*)app_addr;        // 栈顶指针
reset_handler  = *(uint32_t*)(app_addr + 4);  // Reset Handler 地址
```

- 栈顶指针必须在 SRAM（0x20000000 ~ 0x20020000）
- Reset Handler 必须在内部 Flash（0x08000000 ~ 0x08100000）

这两条规则能过滤绝大多数"擦除中断/写入损坏/全 0xFF"的无效镜像，防止跳转后 HardFault。

## 6. 部署步骤

### 6.1 新建 Bootloader 工程（STM32CubeIDE）

1. 用 CubeMX 新建工程，选 **STM32F407ZGTx**（或实际型号）
2. 最低配置即可：调试口 SWD、外部时钟、`HAL_TIM`（可省）、无 RTOS
3. 将以下文件加入工程编译：
   - `bootloader_reference.c`（引导逻辑）
   - `ota_flash_port.c`（Flash 擦写封装，**仅依赖 HAL，无 RTOS 依赖**）
   - `ota_flash_port.h`（分区宏）
4. Bootloader **链接脚本无需修改**，默认 `FLASH ORIGIN = 0x08000000, LENGTH = 0x10000`（64KB）
   > 若需在 Bootloader 中增加自升级功能，可扩展到 0x08010000 之前的全部空间

### 6.2 启动代码调用 bootloader_entry

在 Bootloader 的 `main()` 中最先调用（**必须在任何时钟/外设初始化之前**，确保中断向量表还是 Bootloader 自己的）：

```c
int main(void)
{
    /* 最先执行：完成引导决策，跳转后不再返回 */
    bootloader_entry();

    /* 仅当 bl_app_valid 双区失败进入死循环前，不会走到这里 */
    for (;;) {}
}
```

> 注意：跳转前会关闭全局中断并重定位 VTOR，跳转后 App 自行完成全部初始化。

### 6.3 修改 App 工程（关键！）

1. **链接脚本**：`STM32F407ZGTX_FLASH.ld` 中修改：

```ld
FLASH (rx)      : ORIGIN = 0x08010000, LENGTH = 448K
```

2. **中断向量表偏移**：App 必须把 VTOR 重定位到自己的分区。
   - 最简单：启动文件中定义 `VECT_TAB_OFFSET`，如 `0x00010000`（A区）或 `0x00080000`（B区）
   - 或用 `SCB->VTOR = FLASH_BASE | 0x10000;` 在 `SystemInit()` 后执行
   > OTA 模块的 `get_running_slot()` 通过读 `SCB->VTOR` 判断当前运行区，因此 App 的 VTOR 重定位是必须的

### 6.4 烧录顺序（首次量产）

```
1. 烧录 Bootloader（地址 0x08000000）
2. 烧录 App A 区镜像（地址 0x08010000，烧录工具需支持地址偏移）
3. 上电 → Bootloader 检测到参数区无效 → 写默认参数（active=A）→ 引导 A 区
```

### 6.5 升级流程验证

App 侧执行：

```
ota begin <size> <crc32hex> <ver>   ← 擦除非活跃区（B区）
ota write <offset> <hex>            ← 分片写入（每片≤120字节）
ota end                              ← CRC32 校验，失败返回 ERR_CRC
ota activate                         ← 置 pending=B，写参数区
reset                                ← 重启
```

重启后 Bootloader 引导 B 区 → B 区 App 正常运行 → 调用 `ota_confirm_ok()` → A/B 完成切换。

## 7. 参数区双槽冗余

参数区（S11 扇区 128KB）分为两个 64KB 槽位：

| 槽位 | 地址 | 说明 |
|------|------|------|
| Slot0 | 0x080E0000 | 先写入 |
| Slot1 | 0x080F0000 | 后写入，读取时优先 |

**写入流程**：擦除整个 S11 → 写 Slot0 → 写 Slot1。
**读取策略**：优先读 Slot1，CRC 无效则回退 Slot0。

这样即使写入中途断电（Slot1 未写完），Slot0 仍是完整有效的旧参数，不会丢失启动标志。每个参数包含 `magic + version + crc32` 三重校验，CRC 覆盖除自身外的全部字段。

## 8. 常见问题

| 问题 | 原因 | 解决 |
|------|------|------|
| 跳转后 HardFault | App 的 VTOR 未重定位，中断向量仍指向 Bootloader 区域 | 按 6.3 节配置 VTOR |
| 升级后无法回滚 | App 未启用看门狗 | 在 App 中启用 IWDG 并在主循环喂狗 |
| 参数区失效 | S11 擦除中途断电（双槽均 0xFF） | 属预期行为，Bootloader 回退默认 A 区并重写参数 |
| `ota confirm` 后 active 未变化 | 无 Bootloader 时 VTOR=0x08000000，无法识别运行区 | 部署 Bootloader 后自动解决 |
| 新固件 CRC 校验失败 | 传输损坏或 CRC 计算方式不一致 | 确认 PC 端 CRC32 为 IEEE 802.3（poly 0xEDB88320） |

## 9. 相关文件

| 文件 | 角色 |
|------|------|
| `bootloader_reference/bootloader_reference.c` | 引导/回滚逻辑（独立工程源码） |
| `firmware/Drivers/BSP/Inc/ota_flash_port.h` | 分区布局宏 + Flash 驱动接口（两工程共用） |
| `firmware/Drivers/BSP/Src/ota_flash_port.c` | Flash 擦写 + CRC32 + 电源监测（Bootloader 可复用） |
| `firmware/Drivers/BSP/Src/ota_core.c` | App 侧 OTA 状态机（仅 App 编译） |

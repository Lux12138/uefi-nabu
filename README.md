# uefi-nabu

Xiaomi Pad 5（nabu / SM8150）的 UEFI 固件，基于 Project Silicium + Project Mu。
这是整条启动链的第一环：

```text
XBL → ABL → [fastboot boot 本仓库产出的 boot.img] → UniXNU 菜单
     → DarwinBoot → XNU 内核
```

Fork 自 `UniXNU/uefi-sheng`。`Platforms/Xiaomi/nabuPkg` 与
`Resources/Configs/nabu.toml` 来自上游 Mu-Silicium；本仓库加入移植所需的
UniXNU 模块、固件日志导出与 APSS 看门狗。

- [Firmware integration](https://unixnu.com/porting/uefi-firmware)
- [nabu device support](https://unixnu.com/devices/nabu)
- [Build instructions](Building.md)

## 固件参数（均为实测值，非标准机型数字）

| 项 | 值 | 来源 |
|---|---|---|
| FD base | `0x9FC00000` | `Resources/Configs/nabu.toml` |
| FD size | `0x300000`（3 MiB） | `Resources/Configs/nabu.toml` |
| Framebuffer | 1600 × 2560，32-bit | `nabuPkg/nabu.dsc` |
| System memory base | `0x80000000` | `nabuPkg/nabu.dsc` |
| UEFI stack | `0x9FF90000`，每核 256 KiB | `nabuPkg/nabu.dsc` |
| Advanced Logger | `0x9FF00000`，`0x80` 页 | `nabuPkg/nabu.dsc` |
| SMBIOS 身份 | Xiaomi / Pad 5 / nabu / 21051182G / K82 | `nabuPkg/nabu.dsc` |
| Boot image header | version 1 | `Resources/Configs/nabu.toml` |
| SoC type | 2（SM8150-AC / 860） | `nabuPkg/nabu.dsc` |

## 加入了什么

### `nabu.dsc` — `[Components]`

| 模块 | 作用 |
|---|---|
| `UniXNU.inf` | 启动菜单（选 payload、超时、回滚） |
| `UniXNUDxe.inf` | UniXNU DXE 驱动 |
| `UniXNURamDiskDxe.inf` | initrd payload 卷 |
| `DarwinBoot.inf` | XNU 加载器（解析 Mach-O、校验 ADT、ExitBootServices、跳转） |
| `AdvancedSerialLoggerDxe.inf` | 固件日志经串口导出 |
| `AdvancedLogDxe.inf` | 日志导出驱动 |
| `NabuWdtDxe.inf` | APSS 看门狗 + ESP 启动日志（见下） |
| `DiskIoDxe` / `PartitionDxe` / `Fat` | payload 卷的文件系统 |

`[LibraryClasses]`：`UniXNULib`、`FdtLib`、`HobLib`、
`AdvancedLoggerAccessLib`、`AdvancedLoggerLib`（分 DXE_CORE / DXE 两档）。

`[PcdsFeatureFlag]`：`PcdAdvancedLoggerFixedInRAM = TRUE`。

### `nabu.fdf` — DXE 固件卷

与候选 `nabu.dsc` 同一批 INF：UniXNU 四个模块 + 两个日志驱动。

### `BootShim/BootShim.S`

两处修正：

1. **镜像长度**：`FD_SIZE` → `FD_SIZE + (_Payload - _Head)`，覆盖 shim 加
   payload 的完整范围。
2. **写后执行**：跳转前先 `dsb sy`，按 cache line 走一遍 `dc cvac`，
   再 `ic iallu` / `dsb sy` / `isb`。不刷新就直接执行刚写入的指令，
   等于赌 bootloader 留下的 cache 状态。

### `Drivers/NabuWdtDxe`（新增）

两个职责，都默认不动作：

| 时机 | 动作 | 开关 |
|---|---|---|
| `ReadyToBoot` | 采样 `PRAM_BASE`，写到 NABUESP 卷的 `BOOTLOG0/1.TXT`（经 `BOOTLOG.TMP` + rename），然后打新标记 | `NABU_ESP_LOG`（0 时编译掉） |
| `ExitBootServices` | mode 为 1 或 2 时武装 APSS 看门狗（`0x17C10000`）；mode 0 完全不碰 MMIO | UEFI 变量 `NabuWdt` |

`NabuWdt` 变量缺失时用 DSC 的 `NABU_WDT_DEFAULT`（本板编译为 `0`）。

**入口点故意不碰块设备**：它在 `PartitionDxe` / `FatPkg` 起来之前运行，
2026-10-26 有一次在那里做了无界 `ReadBlocks`，把板子卡死且没有复位源。
所有存储访问都在 `ReadyToBoot`。

## 构建

从 UniXNU 驱动：

```bash
./unixnu build firmware --uefi ../uefi-nabu
```

驱动读 `Resources/Configs/nabu.toml`，把 `FD_BASE` / `FD_SIZE` / `FD_BLOCKS`
传给 EDK II，按 `board.toml [firmware] packages` 组装 `PACKAGES_PATH`，
最后校验输出大小。

**独立 `python build_uefi.py -d nabu` 不可用**：它不注入 UniXNU 的包路径，
`UniXNUPkg` 与 `DarwinBootPkg` 的组件会解析失败。要能启动操作系统的固件，
必须走 UniXNU 驱动。

产物是 gzip 压缩的 UEFI FD，包进 Android boot image（`header_version = 1`，
`page_size = 2048`），大小约 3.0 MiB。

## 注意事项

- **`USE_CUSTOM_DISPLAY_DRIVER = 1`**：厂商显示链（`CPRDxe` + `DisplayDxe`）
  会编入。实机固件解压后 `DisplayDxe` 命中 118 次、`CPRDxe` 命中 5 次，
  确认这条链是加载的。sheng 移植从 `= 0`（`SHENG_VENDOR_HW = 0`）起步；
  若 bring-up 时面板黑屏，先对比这一档。
- 固件卷 3 MiB，与 sheng 相同，容得下 UniXNU 的模块。
- `nabu.toml` 的 `header_version = 1` 与 sheng 的 `0` 不同。boot image
  打包器会读这个值；刷机前要对着 nabu 的 bootloader 核对产物头。
- 本仓库的源码是**板侧实际构建用的那一份**。构建路径会内嵌进固件：
  对解压后的 FD 跑 `strings`，能看到形如
  `<构建根>/Build/nabuPkg/DEBUG_CLANGPDB/AARCH64/.../*.pdb` 的路径，
  可以据此确认某个固件是由哪棵树构建的。

## Credits

[Microsoft](https://github.com/microsoft/) for [Project Mu](https://microsoft.github.io/mu/), which powers this project. <br>
[WOA-Project](https://github.com/WOA-Project/) for [mu_andromeda_platforms](https://github.com/WOA-Project/mu_andromeda_platforms/), used as the base. <br>
[Project Aloha](https://github.com/Project-Aloha/) for [mu_aloha_platforms](https://github.com/Project-Aloha/mu_aloha_platforms/), used as a reference for some SoCs. <br>
[Project Silicium](https://github.com/Project-Silicium/) for the Mu-Silicium platform integration and device binaries.

## License

All code except drivers in `GPLDrivers`, `GPLApplications` and `GPLLibrary`
directories is licensed under BSD-2-Clause. <br />
GPL drivers are licensed under GPLv2 or later.

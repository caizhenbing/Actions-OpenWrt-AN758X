# OpenWrt 导入包

这个文件夹是**唯一需要进 OpenWrt（OP）源码树的东西**：已经装配好的四个包、设备树
补丁、以及导入脚本。仓库里其它目录（`driver/`、`userspace/`、`luci/`）是这些包的
规范源，构建时不需要它们——本文件夹是自包含的。

```
openwrt-import/
├── install-into-openwrt.sh        导入脚本（推荐用法）
├── package/fttr/
│   ├── fttr-fmcs/                 内核模块：FPGA 驱动
│   │   └── src/                   fmcs.c fmcs.h fmcs_spi.c fmcs_load.c fmcs_uapi.h
│   ├── fttr-tools/                用户态：miniolt
│   │   └── src/                   miniolt.c fmcs_uapi.h
│   ├── fttr-firmware/             FPGA 比特流 FTTR_TOP.sbit（文件需自备）
│   └── luci-app-h3c-fttr/         LuCI 页面、rpcd 插件、菜单与 ACL
└── patches/0001-arm64-dts-airoha-hm2004-du-enable-fmcs.patch
```

---

## 用法一：脚本导入（推荐）

```sh
./install-into-openwrt.sh ~/ponwrt
# 带上比特流，并顺手提交
./install-into-openwrt.sh --commit --firmware /path/to/FTTR_TOP.sbit ~/ponwrt
```

脚本会：

1. 校验目标确实是 airoha 的 OpenWrt 树（`rules.mk`、`include/`、`target/linux/airoha/`）；
2. 把 `package/fttr/` 下的每个包复制到 `<openwrt>/package/fttr/`；
3. 若给了 `--firmware`，把比特流放到 `fttr-firmware/src/FTTR_TOP.sbit`，并校验大小
   （原厂为 2 087 000 字节；换比特流可以，截断的不行）；
4. 应用设备树补丁（`git apply`，失败则退回 `patch -p1`），已经打过会跳过。

常用参数：`--dry-run` 只看计划、`--no-dts` 跳过设备树、`--commit` 自动提交。
重复运行是安全的：它会提示覆盖了什么，且不会删除任何不是它放进去的东西。

## 用法二：手工导入

```sh
cp -R package/fttr <openwrt>/package/
# 比特流（2 087 000 字节，来自原厂 rootfs 的 /lib/firmware/）
cp FTTR_TOP.sbit <openwrt>/package/fttr/fttr-firmware/src/
# 设备树
git -C <openwrt> apply patches/0001-arm64-dts-airoha-hm2004-du-enable-fmcs.patch
```

## 选包与编译

```sh
make menuconfig
  Kernel modules -> Other modules -> fttr-fmcs
  Firmware        -> fttr-firmware
  Utilities       -> fttr-tools
  LuCI -> Applications -> luci-app-h3c-fttr
make -j$(nproc)
```

## 注意事项

* **`FTTR_TOP.sbit` 不在仓库里。** 它是 2 MB 的固件二进制，不属于源码；
  `fttr-firmware` 在放进去之前编不过。没有比特流时驱动仍会加载（
  `CONFIG_FTTR_FMCS_LOAD_AT_PROBE=y` 下加载失败只告警不阻断），寄存器访问也会失败。
* **不要往内核源码树里放任何东西。** 没有 `target/linux/airoha/files/` 的需要，
  唯一的 tree 改动就是那个设备树补丁。
* **`fmcs_uapi.h` 有两份**（驱动包和工具包各一份），必须字节一致，否则驱动和
  `miniolt` 会对 ABI 产生分歧而双方都能编译通过。`tools/check_package_tree.py` 会
  比对它们以及各自与规范源的一致性，改完源码记得跑一次。
* **寄存器通路仍未打通**（R1）：驱动加载、比特流下载、`/dev/fmcs_mci`、两个 net
  device 都正常，但 SPI 控制器在 6.18 上还被 SPI-NAND 驱动占着，没有通用
  `spi_sync()` 传输，所以每次寄存器读写都会失败。见仓库 `docs/NEXT_STEPS.md`。

## 编译状态：哪些验过，哪些没验过

诚实交代，避免把"装好了"误当成"编过了"：

| 部件 | 状态 |
| --- | --- |
| 包结构 / Makefile / 文件清单 | **已自检**（`check_package_tree.py`：树形、obj-m 形式、`fmcs_uapi.h` 双份一致、与规范源无漂移） |
| `fttr-tools`（miniolt） | **本地编译通过**（`check_c.py` 在宿主上编） |
| `fttr-fmcs`（内核模块） | **未在真实 6.18 内核树上编译过** — 此仓库没有内核源码树，无法验证 |
| `fttr-firmware` | 缺 `FTTR_TOP.sbit` 时**必然**编不过，属预期 |
| `luci-app-h3c-fttr` | 纯 JS/shell + Makefile；rpcd 接口已做契约自测，未经完整 ipkg 构建 |

内核模块是唯一未验证的一环，风险来自厂商是 5.4、目标是 6.18。已做的静态核对
（对着 v6.18 头文件逐个查签名）：

* `class_create(const char *)` —— 6.4 起改为单参数，源码是单参数 ✓
* `dev_alloc_skb()` —— 6.18 仍作为 legacy inline 存在 ✓
* `genl_register_family(family)` / `genlmsg_multicast(family, skb, portid, group, flags)` ✓
* `proc_create(name, mode, parent, const struct proc_ops *)` ✓
* 用的是 gpiod（descriptor）接口，不是已被清退的整数 GPIO ✓
* 未使用 `strlcpy`、`init_timer`、旧 `ndo_set_mac_address` 等 6.x 已移除/改签名的接口

仍然只有上机才能确认的：结构体字段级差异、OpenWrt 的 backport 补丁、`-Werror`
类告警。`make package/fttr-fmcs/compile V=s` 是判据，别靠推理。

## 校验

```sh
python ../tools/check_package_tree.py    # 包树自洽性 + 与规范源无漂移
python ../tools/run_checks.py            # 仓库全部检查（含上面这项）
```

# ok3399-forlinx

Forlinx OK3399（Rockchip RK3399 核心板 + Forlinx 官方底板）的 Armbian 适配 overlay。
配套 CI：GitHub Actions 拉固定版本的 `armbian/build`，把这个 overlay 塞进去，直接出可烧录镜像。

## 目录结构

```
config/boards/forlinx-ok3399.conf        板级配置（含 U-Boot 的 defconfig/dts/dtsi 生成 hook）
patch/kernel/rockchip64-current/
    0000.patching_config.yaml            告诉框架把 dt/*.dts 拷进 arch/arm64/boot/dts/rockchip 并改 Makefile
    dt/rk3399-forlinx-ok3399.dts         内核设备树（mainline 6.18 风格）
.github/workflows/build-ok3399.yml       CI 构建
```

## 关键设计决策

**BOOT_SCENARIO = `only-blobs`**：rkbin 的 DDR bin + miniloader + BL31 blob，加 mainline U-Boot proper（v2025.01）。
选它的理由是不依赖 DRAM 的 `rockchip,sdram-params`——我们手上没有 OK3399 的 DDR 时序参数，
所以 `blobless`（U-Boot 内部 TPL 做 DRAM 初始化）那条路走不了。

没选 `binman` + 外部 TPL：U-Boot v2025.01 里 26 个 rk3399 defconfig 全是 `CONFIG_TPL=y`，
`ROCKCHIP_EXTERNAL_TPL` 的 `default y` 只对 rk3308/rk3568/rk3588 生效，rk3399 上没有先例。

**U-Boot 的 dts 必须单独一份**，不能复用内核那棵：v2025.01 的 `dts/upstream` 是**未拆分的旧版**
`rk3399.dtsi`（没有 `rk3399-base.dtsi`、没有 `mipi_dsi` 标签），内核 6.18 风格的 dts 给它用编译必挂。

**U-Boot 的三个文件用 board conf 里的 `pre_config_uboot_target__*` hook 现场生成**，不用 patch 文件：
armbian 的 patch 注入只认 `patch/u-boot/<ver>/*.patch`，`defconfig/`、`dt_upstream_rockchip/`、
`dt_uboot/` 这类子目录在 `lib/tools/patching.py` 里没有任何处理逻辑。

## 本地构建

```bash
git clone --depth 1 https://github.com/armbian/build
cd build
cp /path/to/this/config/boards/forlinx-ok3399.conf config/boards/
mkdir -p patch/kernel/rockchip64-current
cp -r /path/to/this/patch/kernel/rockchip64-current/. patch/kernel/rockchip64-current/
sudo ./compile.sh build BOARD=forlinx-ok3399 BRANCH=current RELEASE=noble \
     EXPERT=yes KERNEL_CONFIGURE=no BUILD_DESKTOP=no BUILD_MINIMAL=no
```

## CI

`build-ok3399.yml` 拆成**两个 job**，只为绕开 runner 的单 job 6 小时上限：

```
job1 rootfs  (timeout 90min)   compile.sh rootfs   debootstrap + 包安装   20~30 分钟
     └─ artifact: rootfs-cache
job2 image   (timeout 360min)  compile.sh build    u-boot + 内核 + 打包   3.5~4.5 小时
```

job1 产出的 rootfs 用 artifact 在同一次 run 内交给 job2，
同时用 `actions/cache` 跨 run 复用（rootfs cache id 与板子无关，见下文）。

各步细节：
1. 腾磁盘（runner 只有 ~14GB，6.18 内核 worktree 会撑爆）
2. checkout 本仓库 + 固定 SHA 的 armbian/build
3. 把 overlay 拷进 armbian-build，清 CRLF，并把 `git.sh` 改成 shallow fetch
4. rootfs 改名成当月（跨月复用关键）、ccache 恢复
5. `compile.sh build ... KERNEL_BTF=no`
6. `xz -T0 -6` 压缩镜像（Release 附件单文件上限 2GB，裸镜像接近 3GB）
7. 镜像 + rootfs + debs 一起发到 GitHub Release（都走 Release，不占 artifact 存储配额）

⚠️ **GitHub-hosted runner 是 2 核 / 7GB RAM，单次 job 硬上限 6 小时**。
6.18 内核冷编译在这个规格上要 3~5 小时，非常贴边。要稳定出图就用自托管 runner
（把 `runs-on` 改成 `[self-hosted, linux, x64]` 即可，其余不用动）。

四个省时/省资源的关键开关，都是查过框架源码才定的，不是拍脑袋：

| 开关 | 依据 | 作用 |
|---|---|---|
| `KERNEL_BTF=no` | `lib/functions/compilation/armbian-kernel.sh:150-171`：BTF 要求 **6451 MiB** 可用内存，不足直接 `exit_with_error` | runner 只有 ~6.8GB 可用，BTF 会 OOM；且 `pahole` 在 2 核上极慢 |
| rootfs 剥离 | `artifact-rootfs.sh:46-51`：缓存版本含 `YYYYMM`，「按月强制刷新」 | 复用预置 rootfs，省 debootstrap + 包安装的 15~25 分钟 |
| `git fetch --depth 1` | Linux 完整 git 历史好几个 GB | 省 clone 时间和磁盘（runner 只有 ~14GB） |
| ccache | 内核第二次起大幅加速 | 反复调 dts 时收益巨大 |

## rootfs 剥离

armbian 的 rootfs 缓存**与板子无关**（只取决于 ARCH / RELEASE / cache_type / 包列表），
所以可以做成独立资产，跨构建复用。

缓存文件名是精确匹配的：

```
cache/rootfs/rootfs-${ARCH}-${RELEASE}-${cache_type}_${YYYYMM}-${rootfs_cache_id}-${suffix}.tar.zst
例：rootfs-arm64-noble-cli_202609-de3dd0bda694-H6eccde-Bf1b6db.tar.zst
```

`YYYYMM` 是按月强制刷新的（框架注释原话："we use YYYYMM to make a new rootfs cache version
per-month, even if nothing else changes"），所以跨月会失效——
workflow 里的做法是把下载到的 tarball **改名成当月的名字**再放进 `cache/rootfs/`，这样跨月也能命中。

命中失败不会报错，框架只是自己重建（慢一点，不会坏）。

用法：每次构建跑完会把 rootfs tarball 一并传到 Release，下次构建时把那个 URL 填进
`workflow_dispatch` 的 `rootfs_url` 即可。不填就 job 内自己生成，并用 `actions/cache` 缓存。

## 烧录

- SD 卡：`dd if=Armbian-*.img of=/dev/sdX bs=4M status=progress conv=fsync`
- eMMC：进 maskrom，`rkdeveloptool db rk3399_loader_v1.30.130.bin` → `wl 0 <img>` → `rd`
- 串口：**UART2，1500000 8N1**
- 想看内核启动日志：改 `/boot/armbianEnv.txt` 的 `verbosity=7`（默认 1 会把 printk 全屏蔽）

## 已知待办

- MIPI-DSI 面板：`innolux,p070acb-ab1` 不在主线 `panel-simple.c`，当前 `&mipi_dsi` 是 `disabled`（HDMI 优先）
- BT 三个 GPIO 的 pinctrl 依赖上电默认值，没有显式 mux
- `&dmc` 没加，内存 devfreq 不可用
- RTC / 音频 codec（RT5651）未接

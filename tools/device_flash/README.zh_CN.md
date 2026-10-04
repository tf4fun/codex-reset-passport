<p align="right"><strong>简体中文</strong> · <a href="README.md">English</a></p>

# CI 固件验证与设备等待

[verify_ci_firmware.py](../verify_ci_firmware.py) 离线验证已解压的 CI 包。
[flash_ci_firmware.py](../flash_ci_firmware.py) 复用相同校验，并可等待明确指定的唯一设备，
完成单次刷写、独立验证、标准复位和有界启动观察。工具不编译固件、不安装依赖、不下载文件，
也不修改安全设置。默认只做 dry-run，不访问串口、不创建输出文件，也不需要安装 esptool 或 pyserial。

## 支持的包与数据保留

使用 [package_ci_firmware.py](../package_ci_firmware.py) 生成的包：必须包含
`FoloToy-AI-Passport-full.bin`、`manifest.json` 和 `SHA256SUMS`。
从可信 CI 交付记录独立取得获准镜像的 SHA256。校验包括整个文件、manifest 大小与哈希、
C3 镜像完整段、校验和及附加 SHA256、应用与 ELF 身份、组件偏移、分区 MD5、
擦除填充间隙和扇区对齐的擦除范围。组件必须覆盖完整镜像，应用必须延伸至合并文件结尾。
manifest 只作为数据读取。哈希一致性不是发布者签名或硬件验收；清单里的 ELF 身份
也不等于取得了可用于解码崩溃的实际匹配 ELF 文件。

此工具限定 ESP32-C3、8MB Flash、ESP-IDF 5.5.3、DIO/80MHz 及仓库最小分区布局。
仓库通用构建流程仍支持其他合法布局；请使用单独审查的对应刷写流程，不能强行绕过此工具检查。

| 组件 | 偏移 | 保护 |
| --- | --- | --- |
| Bootloader | `0x0` | 必须在受保护的数据扇区之前结束 |
| 分区表 | `0x8000` | 一个分区表扇区，在 `0x9000` 之前结束 |
| Factory 应用 | `0x10000` | 必须适合 `0x7F0000` 大小的 factory 分区 |
| NVS | `0x9000` 至 `0xEFFF` | 工具不读取、擦除或写入 |
| PHY | `0xF000` 至 `0xFFFF` | 工具不擦除或写入 |

合并镜像会填充数据间隙，整文件写入可能重置 NVS。工具只从内存中的不可变快照提取三个
经过校验的组件。写入前只读取设备现有分区表扇区（`0x8000`，4096 字节），要求它与
CI 分区表匹配。这是兼容性检查，不是原固件备份；不会读取 Wi-Fi 凭据。
保留扇区不能证明新应用能正确使用不兼容的存储格式，仍需确认应用数据兼容性。

## 离线检查与 dry-run

激活计划使用的 Python 环境，将引号内占位内容替换为获准产物的实际值：

```bash
python3 tools/verify_ci_firmware.py /path/to/extracted-ci \
  --sha256 '<approved SHA256>' --expected-version '<app version>'

python3 tools/flash_ci_firmware.py /path/to/extracted-ci \
  --sha256 '<approved SHA256>' --expected-version '<app version>'
```

输入也可以是合并镜像文件路径。`--manifest /path/to/manifest.json` 指定清单文件，
`SHA256SUMS` 仍应在合并镜像旁。`--expected-version` 可选，提供时必须与二进制匹配；
省略时，启动验证使用已从二进制校验过的版本。

## 获得授权后执行

执行目前支持 macOS/Linux，需要已有的官方 esptool 5.4.x 环境、pyserial 和 `lsof`。
使用该环境的 Python 执行命令。事先用只读的 `python3 -m serial.tools.list_ports -v`
识别目标原生 USB 序列号；C3 的原生 USB 序列号对应其 MAC 身份。保持 USB 连接稳定。
发现设备不等于刷写授权；执行前应向设备所有者确认目标、获准镜像和数据范围。

```bash
python3 tools/flash_ci_firmware.py /path/to/extracted-ci \
  --sha256 '<approved SHA256>' --expected-version '<app version>' \
  --execute --target-serial '<native USB serial>' \
  --wait-seconds 600 --output build/device-flash/run-001
```

`--execute` 必须提供已知目标序列号和全新的输出目录。USB VID/PID 与身份必须唯一匹配；
复位后重新发现端口名。检查串口占用，但不终止其他进程。私有目标锁防止对同一设备并发执行；
已有结果目录会被拒绝，也不会自动清除遗留目标锁。

离线准备与加锁完成后，工具输出 `waiting_for_wake` 和 UTC 截止时间，届时可唤醒设备。
默认窗口为 600 秒，可设为大于零、最多 3600 秒。目标已存在时立即继续。
识别、安全状态/容量/分区预检、单次写入、独立 verify 和启动观察连续执行，无需另一轮提示或模型处理。
目标缺失或不唯一、安全标志未知或已启用、型号/容量错误或分区表改变，均在写入前停止。

`--command-seconds` 默认 120、最多 180；`--boot-seconds` 默认 25、最多 60。
写入保留 esptool 原有安全检查。有时限的子进程仅在本次调用中禁用整镜像和数据块重试，
忽略继承的 `ESPTOOL_*` 默认参数，不修改已安装文件或配置。
不整片擦除、不使用 force、不绕过加密、不自动重试写入，失败后也不自动执行恢复复位。
独立验证使用原组件字节和显式 `keep` 参数；stub 命令使用 `no-reset-stub`，
避免 `no-reset` 隐含的恢复复位。验证通过后才做一次标准复位并采集启动日志。

## 结果与停止的操作

`result.json` 记录 `preflight`、`write`、`verify`、`boot`、写入尝试次数和数据边界。
阶段进展以 JSON 输出。发出写入命令前持久记录写入意图；写入中断、失败或超时被标为
`UNCERTAIN`，停止后续 verify、复位与启动观察。决定下一次手动操作前先检查结果、
进程状态和设备，不能把执行通道断连视为再次启动的授权。
若进程死亡后留下目标锁，应先确认进程已结束并检查其结果，再手动删除该锁。

Boot PASS 要求观测到期望应用版本、`app_main()`，且窗口内没有检测到 panic。
中英文 BSP 就绪信息另行识别。没有日志、版本错误或崩溃都以非零退出，不会重新刷写。
监视串口在 `finally` 中关闭，读取异常也会关闭。只保存白名单中的脱敏启动行；
丢弃凭据/SSID/token/URL 行，脱敏 MAC/IP。子进程输出在保存前脱敏。
结果、组件二进制和运行日志应放在 Git 外；示例使用被忽略的 `build/` 目录。

成功退出 `0`；校验、超时、命令或启动失败都以非零退出。
Build 和 Host tests 属于源 CI。刷写及启动成功不能验收 LCD 外观、按键交互、
相机扫码、射频行为、深睡眠/唤醒、存储设置语义或功耗。

## 无硬件测试

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_flash_ci_firmware.py
```

测试使用合成镜像和模拟的枚举/esptool/串口操作，覆盖哈希/映射/擦除边界失败、
dry-run 隔离、无设备和多设备、有界等待、安全状态/容量/分区拒绝、完整镜像检查、
子进程重试控制、单次写入失败处理、
独立验证、成功顺序、脱敏、中文启动证据、输出复用拒绝及串口关闭。
不需要设备或 SDK，并已接入仓库现有完整 CI gate。

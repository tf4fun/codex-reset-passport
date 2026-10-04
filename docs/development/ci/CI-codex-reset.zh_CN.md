<p align="right"><a href="CI-codex-reset.md">English</a></p>

# Codex Reset 私有仓库 CI

私有派生仓库使用 `.github/workflows/codex-reset.yml`，在推送、拉取请求和手动触发时运行。原上游工作流保留供参考，但限制为官方上游仓库，不会同步或发布本派生项目。

任务固定官方 `espressif/idf:v5.5.3` 版本镜像和官方 GitHub Actions 完整提交哈希，仓库权限仅只读。无需自定义 secret、PAT、Wi-Fi 信息或私有服务凭据，不保留 checkout 身份信息。验证前仅将临时 checkout 的属主调整为容器用户。任务安装主机 GCC 构建工具与 curl；Python、CMake、Ninja 和 RISC-V 工具链由 IDF 镜像提供。镜像使用版本标签，并非不可变 OCI digest；这是可重复执行的验证流程，不承诺产物逐字节一致。

先应用带原始/目标哈希校验的应用专用 BLUFI 补丁，再执行完整 `./tools/validate.sh`：仓库与工作流检查、生成字体覆盖、全部主机测试、隔离固件构建、合并镜像与分区验证、匹配调试归档验证。构建不得改动 `dependencies.lock`。字体检查不依赖已安装助手技能、npm、fonttools 或主机字体包，使用已提交的 C 字体；重新生成字体属于另行记录的开发操作。

产物仅包括合并固件、SHA256SUMS、来源 manifest 和 FLASHING.txt，不上传 SDK、缓存、ELF/MAP 或源码 ZIP。manifest 记录匹配调试文件身份，但这些文件仅保留在构建机。产物保留 14 天。下载烧录前核对成功的确切提交、运行和 SHA-256。设备测试单独进行，0x0 合并烧录可能重置 NVS 设置。

源码 ZIP 解压后先运行 `git init`，在已激活 ESP-IDF 5.5.3 的环境中应用已记录 BLUFI 补丁后运行验证。仓库检查需要 Git 元数据。ZIP 不包含机器专用助手技能链接，保留原上游、字体和第三方许可证。

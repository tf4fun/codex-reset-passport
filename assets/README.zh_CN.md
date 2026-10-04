<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。

### Codex 重置观察应用字体子集

- `fonts/reset_font_12.c` / `fonts/reset_font_16.c` 是固件使用的 `reset_font_16`：12/16 px、4 bpp、
  不压缩的 LVGL 位图字体，覆盖可打印 ASCII 以及 `main/reset_ui.c` 和
  `main/reset_presenter.c` 中的全部固定非 ASCII 文本。
- `fonts/passport_reset_source.otf` 是对应的可复用源字体子集，已重命名。
  它来自 Noto Sans CJK SC Regular 2.004，TTC 中第 2 号字体。
  上游：[Noto CJK](https://github.com/notofonts/noto-cjk)。原系统字体路径为
  `/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc`，SHA-256 为
  `b76b0433203017ca80401b2ee0dd69350349871c4b19d504c34dbdd80541690a`。
- 两个字体资源均遵循 [SIL Open Font License 1.1](fonts/OFL.txt)。
  版权属于 2014–2021 Adobe；上游软件包也标注了 2010–2012 Google。
  子集使用新的字体家族名称 **Passport Reset UI**。
- `fonts/reset_font_text.txt` 记录真实文本清单。该字体不支持任意服务端文字、
  SSID 或人名；应用仅显示有界枚举、时间戳和本地固定文本。
- `main/CMakeLists.txt` 仅编译一次 C 字体资源；中文标签显式选择
  `reset_font_12` 或 `reset_font_16`，仅显示 ASCII 的字号使用已启用的 Montserrat 字体。

复现使用 Python `fonttools==4.61.1` 和官方 `lv_font_conv@1.5.3`。
仓库已包含二进制源子集，常规重新生成无需下载上游字体。在隔离环境安装工具后执行：

```bash
python3 -m pip install fonttools==4.61.1
npm install --prefix /tmp/passport-font-tools --no-audit --no-fund lv_font_conv@1.5.3
python3 tools/generate_reset_font.py \
  --converter /tmp/passport-font-tools/node_modules/.bin/lv_font_conv
python3 tools/generate_reset_font.py --check
```

新增字符不在源子集中时，提供有许可证的原字体：

```bash
python3 tools/generate_reset_font.py \
  --source-font /path/to/NotoSansCJK-Regular.ttc --font-number 2 \
  --converter /tmp/passport-font-tools/node_modules/.bin/lv_font_conv
```

生成器从源代码提取清单、检查源字体覆盖、重命名字体家族，并使用稳定的相对路径、
12/16 px、4 bpp 和 `--no-compress` 参数生成。请一起审查清单和字体变更。

真实 LVGL 主机预览需要 C/C++ 编译器、CMake、Python Pillow 以及已解析的 LVGL
托管组件源码。它直接编译原版 `reset_ui.c`、展示模型和有界数据解析器：

```bash
python3 tools/render_reset_preview.py
# 可选：本地捕获的只读状态 API 响应：
python3 tools/render_reset_preview.py --feed /path/to/status.json
```

PNG、标明模拟状态的拼图、字形/裁剪检查结果和源文件哈希清单输出到
`build/reset-ui-preview/screens/`。捕获数据的图片使用响应生成时间，电量显示为
不可用占位值。这是主机渲染，不能当作设备实拍。工具检查清单全部字形，验证已知
缺失字符 U+9F98 确实被拒绝，检查各标签当前实际字体，并拒绝被裁剪的标签。
设备实际显示和内部堆内存仍需单独检查，预览和编译均不能替代硬件验收。

v2.2 重新生成的 12/16 px 字体均覆盖长按进度弹窗、松键等待及配网阻止休眠
提示（290 个字形）。源字体子集 SHA-256 为
`a4bd342097ae88282d4ab3f8c9e313c098f50e9594fd4f95076cee8d6504854c`。
实际渲染工具同时输出 25 fps 长按动图及 不带数字标注的关键帧拼图；参见
[预览说明](../tools/reset_ui_preview/README.zh_CN.md#v22-长按休眠预览)。

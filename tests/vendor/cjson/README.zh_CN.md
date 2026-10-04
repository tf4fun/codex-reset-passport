<p align="right"><strong>简体中文</strong> · <a href="README.md">English</a></p>

# 主机测试使用的 cJSON 副本

这些未经修改的文件为 cJSON 1.7.19，复制自 ESP-IDF 5.5.3 的
`components/json/cJSON` 子模块，提交为
`c859b25da02955fef659d658b8f324b5cde87be3`：

- [上游源码](https://github.com/DaveGamble/cJSON/tree/c859b25da02955fef659d658b8f324b5cde87be3)
- [MIT 许可证](LICENSE)

此副本使重置公告数据源的主机测试无需 ESP-IDF 或系统 cJSON 开发包即可运行。
固件仍使用 ESP-IDF 的 `json` 组件；请勿将此目录加入固件组件源文件。
固件固定的 ESP-IDF/cJSON 版本更新时，也应同步更新此主机测试副本。

```bash
cc -std=c11 -Wall -Wextra -Werror -Imain -Itests/vendor/cjson \
  tests/test_reset_feed.c main/reset_feed.c tests/vendor/cjson/cJSON.c \
  -lm -o /tmp/test_reset_feed
/tmp/test_reset_feed
```

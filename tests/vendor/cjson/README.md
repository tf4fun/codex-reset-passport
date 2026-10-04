<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Host-test cJSON copy

These unmodified files are cJSON 1.7.19, copied from the ESP-IDF 5.5.3
`components/json/cJSON` submodule, commit
`c859b25da02955fef659d658b8f324b5cde87be3`:

- [Upstream source](https://github.com/DaveGamble/cJSON/tree/c859b25da02955fef659d658b8f324b5cde87be3)
- [MIT license](LICENSE)

This copy lets the reset-feed host tests run without ESP-IDF or a system
cJSON development package. Firmware continues to use ESP-IDF's `json`
component; do not add this directory to firmware component sources. Keep
the host copy aligned when the firmware's pinned ESP-IDF/cJSON changes.

```bash
cc -std=c11 -Wall -Wextra -Werror -Imain -Itests/vendor/cjson \
  tests/test_reset_feed.c main/reset_feed.c tests/vendor/cjson/cJSON.c \
  -lm -o /tmp/test_reset_feed
/tmp/test_reset_feed
```

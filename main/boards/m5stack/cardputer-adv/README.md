# M5Stack Cardputer Adv

M5Stack Cardputer Adv 是一款基于 ESP32-S3FN8 (Stamp-S3A) 的卡片式电脑。

## 硬件规格

| 组件 | 规格 |
|------|------|
| MCU | ESP32-S3FN8 @ 240MHz |
| Flash | 8MB |
| 显示屏 | ST7789V2 1.14" 240x135 |
| 音频编解码 | ES8311 |
| 功放 | NS4150B |
| 麦克风 | MEMS |
| 键盘 | 56键 (TCA8418) |
| IMU | BMI270 |
| 电池 | 1750mAh (带ADC电量监测) |

## 引脚定义

### 显示屏 (ST7789V2)
| 功能 | GPIO |
|------|------|
| MOSI | GPIO35 |
| SCLK | GPIO36 |
| CS | GPIO37 |
| DC | GPIO34 |
| RST | GPIO33 |
| BL | GPIO38 |

### 音频 (ES8311)
| 功能 | GPIO |
|------|------|
| I2C SDA | GPIO8 |
| I2C SCL | GPIO9 |
| I2S BCLK | GPIO41 |
| I2S LRCK | GPIO43 |
| I2S DOUT | GPIO46 |
| I2S DIN | GPIO42 |

### 电池监测
| 功能 | GPIO | 备注 |
|------|------|------|
| ADC | GPIO10 | 100KΩ / 100KΩ 分压 |

## 使用方法

1. 按下 BOOT 按钮进入配网模式
2. 连接 WiFi 后即可使用语音助手功能

## 日本語対応

- Cardputer Adv を言語指定なしでビルドした場合、表示言語は `ja-JP`（日本語）が既定になります。`--language` または menuconfig で他の言語を明示指定した場合は、その指定が優先されます。
- キーボード配網画面と音量・明るさの通知をロケールから表示するようにし、日本語表示を追加しました。
- 現行のCardputer Advビルドは `CONFIG_SPIRAM=n` のため、ESP32-S3で必要なAFE／カスタムウェイクワードを有効にできません。会話開始はBOOTボタンまたはEnterキーを使用してください。日本語ウェイクワードを使うには、PSRAM対応構成と対応モデルが必要です。
- **Wi-Fiを再設定するには:** 待機画面（idle）で、左下の `Ctrl` を押したまま `W` を**約4秒**（3秒の長押し成立後、750msの確認表示が終わるまで）押し続けます。既存のAP/captive portal（画面にSSIDと `http://192.168.4.1` を表示）へ入ります。途中でどちらかのキーを離すと取り消されます。会話中・接続中には動作しません。
- **ペルソナを切り替えるには:** 待機画面（idle）で、左下の `Ctrl` を押したまま `P` を一度押します。`;/ .`（`;` は上、`.` は下）で候補を選び、`Enter` で確定します。`Esc` は戻る／取得失敗時はキャンセル、失敗画面での `Enter` は再試行です。確定後は短時間で接続設定を取り直してから会話を再開します。会話中・接続中には選択画面を開けません。

## 烧录参数

芯片: ESP32-S3, Flash: 8MB, 模式: DIO, 频率: 80MHz

| 地址 | 文件 |
|------|------|
| 0x0 | bootloader/bootloader.bin |
| 0x8000 | partition_table/partition-table.bin |
| 0xd000 | ota_data_initial.bin |
| 0x20000 | xiaozhi.bin |
| 0x600000 | generated_assets.bin |

烧录命令 (build 目录为 `build-cardputer-adv`):

```bash
python -m esptool --chip esp32s3 -b 460800 -p PORT \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 build-cardputer-adv/bootloader/bootloader.bin \
  0x8000 build-cardputer-adv/partition_table/partition-table.bin \
  0xd000 build-cardputer-adv/ota_data_initial.bin \
  0x20000 build-cardputer-adv/xiaozhi.bin \
  0x600000 build-cardputer-adv/generated_assets.bin
```

将 `PORT` 替换为实际串口设备路径（如 `/dev/cu.usbmodem21101`）。

## 参考链接

- [M5Stack Cardputer Adv 官方文档](https://docs.m5stack.com/en/core/Cardputer-Adv)

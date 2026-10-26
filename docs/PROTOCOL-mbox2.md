# Mbox 2（USB）協定規格

本文件整理目前已知的 Mbox 2 USB 協定，是 `core/src/device.c`、`core/src/stream.c` 和 `core/src/models.c` 實作的依據。
資料來源是 Linux kernel 的 `sound/usb/quirks.c`（`snd_usb_mbox2_boot_quirk`、`mbox2_setup_48_24_magic`）和 `sound/usb/quirks-table.h`。這裡只用自己的話記錄協定事實，沒有複製程式碼。

標記說明：**[已知]** 表示 Linux 上已經能正常運作的行為；**[推測]** 表示還需要用實機或封包驗證。

## 1. 裝置識別

| 項目 | 值 |
|---|---|
| Vendor ID | `0x0dba`（Digidesign）|
| Product ID | `0x3000` |
| USB 速度 | Full speed（USB 1.1），每 1 ms 一個 isochronous 封包 |

## 2. 開機（firmware boot）[已知]

剛上電時，裝置處在 boot-loader 狀態，configuration descriptor 的 `wTotalLength` 是 **646**。

1. 每隔 0.5 秒發一次 vendor 請求，最多 10 次：

   | bmRequestType | bRequest | wValue | wIndex | wLength |
   |---|---|---|---|---|
   | `0xC0`（vendor, device, IN） | `0x85` | `0x0001` | `0x0000` | `0x12`（18 bytes）|

   回應的第 0 個 byte：`0x01` 表示還在載入，`0x02` 表示就緒。

2. 就緒後，裝置會提供新的、比較大的 descriptor。重新讀取 device descriptor，再重送一次 `SET_CONFIGURATION`（Linux 用的是 `usb_reset_configuration`）。

**macOS 上的注意事項 [推測]：** IOKit 會快取列舉（enumeration）時拿到的 descriptor。如果 `SET_CONFIGURATION` 之後還看不到第 6 號 interface，`device.c` 會呼叫 `libusb_reset_device()` 強制重新列舉，然後回傳 `MBOX_REOPEN`，讓呼叫端關掉再重新開啟裝置。重新開啟時如果 `wTotalLength` 已經不是 646，就代表 firmware 已經在跑，會跳過開機流程。這段需要實機確認。

## 3. 設定取樣率 [已知，參數有疑點]

Linux 的流程如下（UAC1 class 請求，recipient 是 endpoint，`wValue = 0x0100` 是 Sampling Frequency Control）：

| 步驟 | bmRequestType | bRequest | wIndex（端點）| 資料 |
|---|---|---|---|---|
| 1 | `0x22`（class, endpoint, OUT） | `0x01` SET_CUR | `0x0085` | 3 bytes |
| 2 | `0xA2`（class, endpoint, IN） | `0x81` GET_CUR | `0x0085` | 3 bytes |
| 3 | `0xA2` | `0x81` GET_CUR | `0x0086` | 3 bytes |
| 4 | `0xA2` | `0x81` GET_CUR | `0x0003` | 3 bytes |

取樣率用 3 個 byte 的 little-endian 表示：48000 = `80 BB 00`。

疑點：Linux 在第 1 步送出的 3 個 byte 其實是沒有初始化的緩衝區，真正的 48000 只出現在後面幾個 GET 請求的緩衝區裡。本實作在 SET_CUR 送出正確的 `80 BB 00`，GET_CUR 讀回來的值只拿來記 log。端點 `0x86` 沒有出現在任何 streaming interface 上，推測是 feedback 或同步用的端點。

只確認過 **48 kHz**。44.1 kHz 理論上可以把資料換成 `44 AC 00`，但還沒有驗證，所以 `models.c` 只列出 48000。

## 4. 音訊串流 [已知]

| 方向 | Interface | Alt setting | 端點 | 同步方式 | 格式 |
|---|---|---|---|---|---|
| 播放 | 2 | 2 | `0x03` OUT | isochronous **async** | S24_3BE，2 ch，48 kHz |
| 錄音 | 4 | 2 | `0x85` IN | isochronous **sync** | S24_3BE，2 ch，48 kHz |

- S24_3BE：每個 sample 3 個 byte、**big-endian**、有號整數，交錯排列（L R L R …）。每個 frame 6 個 byte，48 kHz 時每個封包 48 個 frame，也就是 288 個 byte。
- Interface 0、1、3、5 在 Linux 上是忽略的（應該是 audio control 和 alt 0 的空 interface）。
- 播放端點是 async，但沒有宣告 feedback 端點。本實作採用 **implicit feedback**：每個播放封包的 frame 數，直接照抄最近收到的錄音封包的 frame 數（錄音端點和裝置時脈同步）。

## 5. MIDI [已知]

| 項目 | 值 |
|---|---|
| Interface | 6 |
| OUT 端點 | `0x02` |
| IN 端點 | `0x81`，interval 1 |
| 封包格式 | 「Midiman」格式，每個封包 4 bytes |

Midiman 封包格式：

```
byte 0..2 : 原始 MIDI bytes（不足 3 個就補 0）
byte 3    : (cable << 4) | 有效長度(1..3)；0 表示填充用的封包
```

傳送端要把 MIDI 位元組流切成完整的訊息，running status 要展開，sysex 每 3 個 byte 切一段（實作在 `core/src/midi.c`）。端點的傳輸類型（bulk 或 interrupt）在執行時從 descriptor 讀取。

## 6. 尚未逆向的部分

- 44.1 kHz 的切換流程
- 有沒有可以由軟體控制的 mixer 或監聽混音（面板上的旋鈕是類比的）
- 端點 `0x86` 的用途
- 錄音 S/PDIF 時鐘鎖定的狀態查詢（Mbox 1 有對應的 vendor 請求，Mbox 2 未知）

要抓封包，請參考 [REVERSE-ENGINEERING.md](REVERSE-ENGINEERING.md)。

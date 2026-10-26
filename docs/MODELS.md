# 機型支援說明

所有跟機型有關的常數都集中在 `core/src/models.c`。串流引擎、daemon 和外掛都只讀取這張表，所以新增或修正機型不需要改其他程式碼。

## Mbox 2（USB，`0dba:3000`）— 已實作

協定見 [PROTOCOL-mbox2.md](PROTOCOL-mbox2.md)。

## Mbox 2 Mini（USB）— 實驗性

- 目前沒有任何公開的協定資料，Linux 也不支援。
- **Product ID 未知**：請執行 `mboxctl list` 查出來，再用 `--pid` 指定。
- 目前的設定檔是假設：因為和 Mbox 2 同一代，推測使用同樣的 boot loader（vendor 請求 `0x85`）、同樣的 interface 和端點配置、S24_3BE 格式。
- 因為是實驗性設定檔，boot 時不檢查 `wTotalLength`，只把數值印出來。
- 驗證步驟：
  1. 用 `mboxctl probe` 把 descriptor 拿出來，和 Mbox 2 比對 interface、alt setting、端點和 `wMaxPacketSize`。
  2. 如果 descriptor 不一樣，照 [REVERSE-ENGINEERING.md](REVERSE-ENGINEERING.md) 抓原廠驅動的封包。
  3. 修正 `models.c` 裡 `mini` 這一筆，用 `mboxctl tone` 測試。全部正常後，把 `support` 改成 `MBOX_SUPPORT_SUPPORTED`，並填入 `pid`。

## Mbox 2 Micro（USB）— 實驗性

- 隨身碟造型的耳機輸出介面，**只有輸出**（設定檔裡 `capture.endpoint = 0`）。
- Product ID 一樣未知，驗證流程和 Mini 相同。
- 因為沒有錄音端點，串流引擎沒辦法用 implicit feedback，改用 nominal rate 決定封包大小（例如 44.1 kHz 時是 44、44、…、45）。daemon 的時脈改由播放封包完成的時間推算。如果 descriptor 裡有 feedback 端點，之後可以加上 explicit feedback。
- Core Audio 外掛還是會顯示 2 進 2 出，輸入端是靜音。

## Mbox 2 Pro（FireWire）— 不支援

這不是程式碼的問題，而是平台已經沒有可行的路徑：

1. **硬體**：Apple Silicon 的 Mac 沒有 FireWire 埠，只能用 Thunderbolt 轉 FireWire 的轉接器（還要多一層 Thunderbolt 3 轉 2 的轉接器）。
2. **軟體**：新版 macOS 已經不提供可用的 FireWire 驅動框架。DriverKit 沒有 FireWire transport，舊的 IOFireWireFamily 只能用 kext。另外，**據報導 macOS 26 Tahoe 已經完全移除 FireWire 支援**，請以 Apple 官方說明為準。
3. **協定**：沒有任何公開資料。據我所知，Linux 的 FFADO 也不支援這台。

可行的替代方案：
- 留一台舊版 macOS 的 Mac，用原廠驅動。
- 改用 Linux：先確認 FFADO 的支援清單；不支援的話，得自己逆向 FireWire 協定（工作量非常大）。
- 換一台 class-compliant 的 USB 介面。

設定檔表裡保留 `pro` 這一筆，`mboxctl` 和 `mboxd` 遇到時會直接說明原因，不會嘗試開啟。

## 其他 Digidesign 機型

- **Mbox（第一代，`0dba:1000`）**：大致上是 class compliant，macOS 內建驅動通常可以直接用。
- **Mbox 3（`0dba:5000`）**：Linux 已經支援（播放 4 聲道，44.1 到 96 kHz，S24_3LE little-endian）。如果需要，可以照同樣的方式加進 `models.c`，但 Mbox 3 的 boot 和格式設定不一樣，要另外實作 `boot_kind`。

# 逆向工程指南：Mbox 2 Mini / Micro

目標：拿到足夠的資訊，把 `core/src/models.c` 裡 `mini` 和 `micro` 這兩筆從「實驗性」改成「已支援」。

## 第 1 步：Descriptor（不需要原廠驅動）

任何一台 macOS 或 Linux 電腦都可以做：

```sh
./build/mboxctl list
sudo ./build/mboxctl probe > probe-before.txt          # 剛插上、還沒開機的狀態
sudo ./build/mboxctl --model mini --pid 0xXXXX boot    # 嘗試用 Mbox 2 的流程開機
sudo ./build/mboxctl probe > probe-after.txt
```

要看的重點：

| 項目 | 代表的意義 |
|---|---|
| `wTotalLength`，以及 `Live wTotalLength` 在開機前後有沒有變 | 有沒有 boot loader 階段 |
| 每個 interface 的 class、subclass 和 alt setting | 哪些是 audio streaming（class 1/2）或 vendor（255） |
| isochronous 端點的位址、方向、同步方式（async/sync/adaptive）和 `wMaxPacketSize` | `wMaxPacketSize ÷ (每 sample 的 bytes × 聲道數)` 大約等於每毫秒的 frame 數 |
| class-specific 的 hex 內容 | 如果是 UAC1，format type descriptor 裡會有 bit 數、聲道數和取樣率 |

如果開機前後完全一樣，而且 descriptor 是標準的 UAC1，那很可能是 class compliant，只差一些 vendor 請求。

## 第 2 步：抓原廠驅動的 USB 封包

最可靠的做法是在 **Linux 主機上跑 Windows 虛擬機**，把 Mbox 透過 USB passthrough 給虛擬機，在虛擬機裡裝原廠驅動，然後在主機用 usbmon 抓封包：

```sh
sudo modprobe usbmon
lsusb | grep 0dba                       # 記下 bus 編號，例如 Bus 003
sudo wireshark -i usbmon3 -k            # 或者: sudo tshark -i usbmon3 -w mini.pcapng
```

其他做法：
- 實體 Windows 電腦：安裝 [USBPcap](https://desowin.org/usbpcap/) 和 Wireshark。
- 舊版 macOS 加原廠驅動：macOS 抓 USB 比較麻煩，不建議。

### 要錄的操作（每一項存成一個檔案）

1. **插上裝置** → 驅動載入完成 → 停止錄製（開機流程）
2. 在原廠控制面板**切換取樣率** 44.1 ↔ 48 kHz（取樣率請求）
3. **播放**一段音訊 5 秒（播放端點、封包大小）
4. **錄音** 5 秒（錄音端點）
5. 如果有面板上的軟體控制（monitor mix 等），每個設定各錄一次

### 在 Wireshark 裡要找什麼

- 過濾 `usb.transfer_type == 0x02`（control transfer）：
  - `bmRequestType` 為 `0x40` 或 `0xC0` 的 → vendor 請求（開機流程、私有設定）
  - `0x22` 或 `0xA2` 加上 `bRequest` 0x01/0x81、`wValue` 0x0100 → 取樣率設定
  - `SET_INTERFACE`（bRequest 0x0B）→ 播放和錄音分別用哪個 interface 和 alt setting
- 過濾 `usb.transfer_type == 0x00`（isochronous）：
  - 封包長度 ÷ 6 = 每毫秒的 frame 數（24-bit 立體聲時）
  - 看實際的 sample bytes 是 big-endian 還是 little-endian：播一個已知的波形（例如 +0.5 的直流）會最容易判斷
  - 如果有一個額外的 IN 端點，每個封包 3 或 4 個 byte → 這是 explicit feedback 端點

## 第 3 步：寫進設定檔

依照抓到的結果修改 `core/src/models.c`：

```c
{
    .id = "mini",
    .pid = 0x????,                       /* 第 1 步查到的 product id */
    .boot = MBOX_BOOT_MBOX2,             /* 開機流程不同的話，新增一個 mbox_boot_kind */
    .bootloader_config_len = ???,        /* 開機前的 wTotalLength */
    .format = MBOX_PCM_S24_3BE,
    .playback = {.iface = ?, .altsetting = ?, .endpoint = 0x??, .channels = 2},
    .capture  = {.iface = ?, .altsetting = ?, .endpoint = 0x??, .channels = 2},
    ...
}
```

開機或取樣率流程如果和 Mbox 2 不一樣，就在 `core/src/device.c` 新增一個 `mbox_boot_kind` 和對應的函式，不要修改 Mbox 2 原本的流程。

接著：

```sh
cmake --build build && ctest --test-dir build
sudo ./build/mboxctl --model mini tone 10
```

聲音和電平都正常之後，把 `support` 改成 `MBOX_SUPPORT_SUPPORTED`，同時更新 `docs/MODELS.md` 和 README 的狀態表。

## 回報資料時請附上

- `mboxctl probe` 開機前後的輸出
- 上面每個操作的 `.pcapng` 檔（可以只保留 VID 0x0dba 的封包）
- 機型名稱、序號標籤上的型號，以及原廠驅動的版本

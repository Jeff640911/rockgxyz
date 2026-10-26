# Mbox 2 系列 macOS 驅動（社群重寫版）

Avid（Digidesign）的 Mbox 2 系列原廠驅動是舊式 kext，新版 macOS 和 Apple Silicon 都已經不支援。這個專案用**不需要 kext、不用關閉 SIP** 的方式重新實作：

```
DAW / 系統音效
      │  Core Audio
      ▼
Mbox2.driver（AudioServerPlugIn，跑在 coreaudiod 裡）
      │  共享記憶體（XPC 交付）：時間軸 ring buffer＋裝置時脈
      ▼
mboxd（LaunchDaemon，透過 libusb 獨佔 USB 裝置）◄──► mboxmidi（LaunchAgent，CoreMIDI）
      │  isochronous USB
      ▼
Mbox 2 / Mini / Micro
```

架構細節請看 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 支援狀態

| 機型 | 介面 | 狀態 | 說明 |
|---|---|---|---|
| **Mbox 2** | USB | ✅ 已實作 | 協定已公開（見 [PROTOCOL-mbox2.md](docs/PROTOCOL-mbox2.md)），2 進 2 出、48 kHz/24-bit、MIDI |
| **Mbox 2 Mini** | USB | 🧪 實驗性 | 沒有公開協定，先套用 Mbox 2 的設定檔當假設，需要抓封包驗證 |
| **Mbox 2 Micro** | USB | 🧪 實驗性 | 只有輸出，同上 |
| **Mbox 2 Pro** | FireWire | ❌ 不支援 | 新版 macOS 沒有可用的 FireWire 音訊路徑，見 [MODELS.md](docs/MODELS.md) |

> ⚠️ **還沒有在實機上測試過。** 單元測試在 CI 上跑（Linux＋macOS），macOS 外掛只確認能編譯成功。第一次接上實機請照下面「第一次測試」的步驟做，並回報 `mboxctl probe` 的輸出。

Mini 和 Micro 要轉成「已支援」，需要有人用原廠驅動抓一次 USB 封包，步驟見 [docs/REVERSE-ENGINEERING.md](docs/REVERSE-ENGINEERING.md)。新增機型只需要在 `core/src/models.c` 加一筆設定檔。

## 編譯

需要 CMake 3.16 以上和 libusb 1.0。

```sh
# macOS
brew install cmake libusb
cmake -S . -B build
cmake --build build -j
ctest --test-dir build        # 單元測試

# Linux（只會編譯核心函式庫、mboxctl、mboxd，用來開發和抓封包）
sudo apt install cmake libusb-1.0-0-dev
```

macOS 上的產出：`build/Mbox2.driver`（Core Audio 外掛）、`mboxd`、`mboxmidi`、`mboxctl`。

## 第一次測試（建議順序）

```sh
./build/mboxctl models               # 列出所有機型設定檔
./build/mboxctl list                 # 找出接上的 Digidesign 裝置（VID 0x0dba）
sudo ./build/mboxctl probe           # 輸出所有 USB descriptor（回報問題時請附上）
sudo ./build/mboxctl boot            # 只跑開機流程
sudo ./build/mboxctl tone 10         # 輸出 1 kHz 測試音，並顯示輸入電平
sudo ./build/mboxctl midi 30         # 印出收到的 MIDI bytes
```

Mini 和 Micro 的 product id 還不知道，要手動指定：

```sh
sudo ./build/mboxctl --model mini --pid 0xXXXX tone
```

`tone` 能正常出聲、輸入電平也會跟著變，再安裝成系統驅動：

```sh
sudo packaging/install.sh build      # 安裝外掛、daemon、LaunchAgent，然後重啟 coreaudiod
sudo packaging/uninstall.sh          # 移除
```

安裝後，「音訊 MIDI 設定」裡會出現「Mbox 2」裝置，以及「Mbox 2 MIDI In/Out」兩個 MIDI 埠。daemon 的 log 在 `/var/log/mboxd.log`。

## 專案結構

```
core/      可攜的 C11 核心：機型表、PCM 轉換、MIDI 編解碼、ring buffer、時脈 DLL、USB 裝置層、串流引擎
daemon/    mboxd（USB ↔ 共享記憶體）、mboxmidi（CoreMIDI 橋接）、XPC 傳輸
hal/       Core Audio AudioServerPlugIn
tools/     mboxctl：開機、探測、測試音、MIDI 監看
tests/     單元測試（ctest）
packaging/ launchd plist、安裝/移除腳本
docs/      架構、協定、機型、逆向工程說明
```

## 已知限制

- 取樣率由 daemon 啟動時決定（Mbox 2 固定 48 kHz），沒辦法從 DAW 切換。
- 外掛固定提供 2 進 2 出；Micro 的輸入端會是靜音。
- 裝置拔除或重新插入時，Core Audio 會看到時間軸跳一次（外掛會改用不同的 seed 通知 HAL）。
- 硬體上的監聽和音量旋鈕是類比電路，不需要驅動；Mbox 2 的 USB mixer 控制還沒有被逆向出來。
- 在 Apple Silicon 上，外掛要簽章才能載入。`install.sh` 會用 ad-hoc 簽章，自己用沒問題；要發布則需要 Developer ID 簽章和公證（notarization）。

## 授權與來源

Mbox 2 的協定常數（請求碼、端點、格式）來自 Linux kernel `sound/usb/quirks.c` 和 `quirks-table.h` 中公開的事實描述。本專案的程式碼是獨立撰寫的，沒有複製 GPL 程式碼。

# 架構說明

## 為什麼不用 kext 或 DriverKit

| 方案 | 問題 |
|---|---|
| kext | 已經被淘汰。Apple Silicon 上要降低安全等級才能載入，未來的 macOS 版本隨時可能完全拒絕 |
| DriverKit＋AudioDriverKit | 技術上最理想，但需要 Apple 核發綁定 USB Vendor ID 的 `com.apple.developer.driverkit.transport.usb` entitlement。`0x0dba` 是 Avid 的 VID，第三方開發者很難拿到 |
| **AudioServerPlugIn＋user-space USB（本專案）** | 不需要特殊 entitlement，ad-hoc 簽章就能自己使用；代價是多一個 daemon，延遲稍高 |

之後如果要改成 DriverKit，`core/` 裡的機型表、PCM 轉換、MIDI 和串流邏輯都是純 C，可以直接沿用。

## 程序分工

```
┌──────────────── coreaudiod（sandbox）────────────────┐
│ Mbox2.driver (hal/MboxHAL.c)                          │
│  • 發布 1 個裝置、2 個 stream（立體聲輸入/輸出）      │
│  • GetZeroTimeStamp ← 共享記憶體裡的時脈 anchor       │
│  • ReadInput  ← 輸入 ring[sample time]                │
│  • WriteMix   → 輸出 ring[sample time]                │
└──────────────────────┬────────────────────────────────┘
                       │ XPC mach service "com.rockgxyz.mboxd"
                       │ 回傳 xpc_shmem（AudioServerPlugIn_MachServices 允許）
┌──────────────────────┴─────── mboxd（root）───────────┐
│ daemon/mboxd.c                                        │
│  • libusb：開機 → claim → 設定取樣率 → 串流           │
│  • USB 事件執行緒：capture → 輸入 ring，              │
│                     輸出 ring → playback              │
│  • 時脈 DLL → 每 512 frames 發布一次 anchor           │
│  • 裝置拔除或錯誤時自動重試                           │
└──────────────────────┬────────────────────────────────┘
                       │ 同一塊共享記憶體裡的 MIDI FIFO
┌──────────────────────┴──── mboxmidi（使用者 session）─┐
│ CoreMIDI 虛擬埠「Mbox 2 MIDI In/Out」                  │
└───────────────────────────────────────────────────────┘
```

MIDI 要另外拆一個 agent，原因是 mboxd 以 root 身分跑在系統 domain，在那裡建立的 CoreMIDI 端點，使用者的 app 看不到。

## 共享記憶體（`core/include/mbox/shm.h`）

```
[mbox_shm_header][輸入 ring：float32 × 2ch × 16384][輸出 ring：同樣大小]
```

- **時間軸 ring（timeline ring）**：frame 存在 `sample_time mod 容量` 的位置。daemon 和外掛不用協調讀寫指標，只要對同一條裝置時間軸定址就行，完全不用鎖。
- **時脈 anchor**：`(sample_time, host_time, generation)`，用 seqlock 保護。外掛在 realtime 執行緒上只做讀取。
- **generation**：裝置重新接上時加 1，外掛把它當成 GetZeroTimeStamp 的 seed，讓 HAL 知道時間軸重新開始了。
- **過期偵測**：anchor 超過 0.5 秒沒更新，外掛就當成裝置不在，改用 host clock 自由執行並輸出靜音，同時在背景重新連線（daemon 可能重啟過）。

## 時脈

1. Mbox 2 的錄音端點和裝置時脈同步，**每個錄音封包裡的 frame 數就是裝置時脈**。
2. USB callback 的時間點有大約 ±0.5 ms 的抖動。`core/src/clock.c` 用二階 DLL 把它平滑成穩定的「host time ↔ sample time」對應（測試顯示：相位誤差在數十微秒內，速率誤差在 ppm 等級）。
3. 每經過 `zts_period`（512）個 frame，就發布一個 anchor。
4. 播放封包的大小照抄錄音封包的大小（implicit feedback），所以播放速率會精準跟上裝置。只有輸出的機型（Micro）改用 nominal rate。

## 延遲預算（48 kHz，預設 3 個 transfer × 8 個封包）

| 段落 | frames | 時間 |
|---|---|---|
| 輸出：USB 佇列（transfers × packets × 1 ms） | 1152 | 24 ms |
| 輸入：一個 transfer 收齊才會交付 | 384 | 8 ms |
| 額外安全餘量 | 48 | 1 ms |

外掛把上面的值回報成 `kAudioDevicePropertySafetyOffset`，DAW 會自動做延遲補償。可以用 `mboxd --transfers 2 --packets 4` 降低延遲，代價是 USB 排程比較容易出問題。

## 執行緒和 realtime 安全

- 外掛的 IO 路徑（`DoIOOperation`、`GetZeroTimeStamp`）只有 memcpy 和 atomic 讀取，不配置記憶體、不用鎖、不做系統呼叫（除了 `mach_absolute_time`）。
- XPC 連線只在背景的 dispatch timer 裡做。
- 重新連線時，舊的映射不會解除（IO 執行緒可能還在用），只會洩漏一次，而且只有 daemon 重啟時才會發生。

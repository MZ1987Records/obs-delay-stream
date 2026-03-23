# obs-delay-stream 安定性修正レビュードキュメント

## 概要

obs-delay-stream v1.7.0 のスレッド安全性に関する9件の不具合を修正した。
修正は深刻度順（CRITICAL → HIGH → MEDIUM）に分類される。

**修正対象ファイル:**
- `src/websocket-server.hpp` — 全面書き換え (v2.0.0)
- `src/plugin-main.cpp` — 部分修正
- `src/delay-filter.hpp` — 全面書き換え
- `src/sync-flow.hpp` — 部分修正
- `src/tunnel-manager.hpp` — 部分修正
- `src/rtmp-prober.hpp` — 部分修正

**ビルド確認:** 未実施（OBSソース環境が当該マシンに存在しないため）

---

## スレッドモデル（前提知識）

| # | スレッド | 用途 |
|---|---------|------|
| 1 | OBS Audio Thread | `ds_filter_audio()` を ~48kHzで呼出。音声処理+WebSocket送信 |
| 2 | OBS UI Thread | GUI描画、ボタンコールバック、`ds_update()` |
| 3 | WebSocket ASIO Thread | websocketpp の `server::run()` イベントループ |
| 4 | Measurement Thread (per-CH) | ping/pong RTT計測（以前は `.detach()` で生成） |
| 5 | RTMP Prober Thread | RTMP TCPハンドシェイクRTT計測 |
| 6 | Tunnel Worker Thread | ngrok/cloudflared 子プロセス管理 |

---

## 修正一覧

### [CRITICAL-1] 音声スレッドでの WebSocket 送信がブロッキング

**ファイル:** `src/websocket-server.hpp`

**問題:**
音声スレッド（`ds_filter_audio()`）から `router.send_audio()` を呼ぶと、
`std::mutex` 取得 + `websocketpp::send()` でネットワークI/Oが完了するまでブロックされていた。
10CH分のクライアントに順次送信するためさらに悪化。
クライアント側のネットワーク遅延やTCP再送により、OBS音声パイプライン全体が停止する可能性があった。

**修正内容:**
`send_audio()` を非ブロッキング化。音声スレッドではパケット構築と接続ハンドルのスナップショット取得のみ行い、
実際の送信は `server_ptr_->get_io_service().post()` でASIOスレッドに委譲するようにした。

```cpp
// 修正後の send_audio() の核心部分
void send_audio(int ch, const float* pcm, size_t frames, uint32_t sr, uint32_t nch) {
    // 1. パケット構築（ロック不要、スタック上）
    auto pkt = std::make_shared<std::string>(/* header + PCM data */);

    // 2. 接続スナップショット（短時間ロック）
    std::vector<WsServer::connection_hdl> targets;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        // sid + ch が一致する接続ハンドルをコピー
    }

    // 3. ASIO スレッドへ委譲（非ブロッキング）
    auto srv = server_ptr_;
    srv->get_io_service().post([srv, targets, pkt]() {
        for (auto& hdl : targets) {
            try { srv->send(hdl, *pkt, websocketpp::frame::opcode::binary); }
            catch (...) {}
        }
    });
}
```

**ポイント:**
- パケットは `shared_ptr<string>` で管理し、ASIOスレッドでの送信完了まで生存を保証
- 接続リストのスナップショットは短時間の mutex ロックで取得（ネットワークI/Oは含まない）
- `post()` は即座に返るため、音声スレッドがブロックされない

---

### [CRITICAL-2] デタッチスレッドによる Use-After-Free

**ファイル:** `src/websocket-server.hpp`

**問題:**
計測スレッドが `.detach()` で生成されており、ライフサイクル管理がなかった。
`ds_destroy()` → `router.stop()` でサーバーを破棄しても、デタッチスレッドは動き続け、
破棄済みの `server_ptr_->send()` を呼んでクラッシュする可能性があった。

**修正内容:**
`.detach()` を廃止し、`std::vector<std::unique_ptr<std::thread>>` で全計測スレッドを管理。
`stop()` 時に `stop_all_measurements()` → 全スレッド `join()` → サーバー停止 の順序で安全に破棄。

```cpp
// 修正前
std::thread([this, sid, ch, ...](){ measure_loop(...); }).detach();

// 修正後
{
    std::lock_guard<std::mutex> lk(measure_threads_mtx_);
    measure_threads_.emplace_back(
        std::make_unique<std::thread>([this, sid, ch, ...]() {
            measure_loop(...);
        })
    );
}
```

計測ループ内の sleep も `running_` フラグをチェックする割り込み可能な方式に変更:
```cpp
// 修正前
std::this_thread::sleep_for(std::chrono::milliseconds(interval));

// 修正後
for (int t = 0; t < interval && running_.load(); ++t)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
```

`stop()` の停止順序:
```cpp
void stop() {
    running_ = false;
    stop_all_measurements();     // 1. 計測停止要求 + join
    join_measure_threads();      // 2. 計測スレッド回収
    // 3. ASIOサーバー停止
    if (server_ptr_) {
        server_ptr_->stop_listening();
        for (auto& [hdl, info] : conn_map_) {
            try { server_ptr_->close(hdl, ...); } catch(...) {}
        }
        server_ptr_->stop();
    }
    if (thread_.joinable()) thread_.join();  // 4. ASIOスレッド join
    conn_map_.clear();                       // 5. マップクリア
    ch_map_.clear();
    server_ptr_.reset();                     // 6. 最後にサーバー破棄
}
```

---

### [CRITICAL-3] 音声スレッドとUIスレッド間のデータ競合

**ファイル:** `src/plugin-main.cpp`

**問題:**
以下のフィールドがロックなしで音声スレッド（読み）とUIスレッド（書き）の両方からアクセスされていた:

| フィールド | 型 | 問題 |
|-----------|------|------|
| `enabled` | `bool` | 非アトミック |
| `ws_enabled` | `bool` | 非アトミック |
| `router_running` | `bool` | 非アトミック |
| `stream_id` | `std::string` | 非POD、並行アクセスは未定義動作 |

**修正内容:**

1. `bool` → `std::atomic<bool>` に変更:
```cpp
// 修正前
bool enabled = true;
bool ws_enabled = true;
bool router_running = false;

// 修正後
std::atomic<bool> enabled{true};
std::atomic<bool> ws_enabled{true};
std::atomic<bool> router_running{false};
```

2. `stream_id`, `host_ip` を mutex 保護 + スレッドセーフアクセサ追加:
```cpp
mutable std::mutex stream_id_mtx;

std::string get_stream_id() const {
    std::lock_guard<std::mutex> lk(stream_id_mtx);
    return stream_id;
}
void set_stream_id(const std::string& id) {
    std::lock_guard<std::mutex> lk(stream_id_mtx);
    stream_id = id;
}
std::string get_host_ip() const {
    std::lock_guard<std::mutex> lk(stream_id_mtx);
    return host_ip;
}
```

3. 音声スレッド (`ds_filter_audio()`) でのスナップショットパターン:
```cpp
// 修正後: 関数冒頭でアトミック値をスナップショット
bool en = d->enabled.load(std::memory_order_relaxed);
bool ws = d->ws_enabled.load(std::memory_order_relaxed);
bool rr = d->router_running.load(std::memory_order_relaxed);
bool has_sid = !d->get_stream_id().empty();
```

4. UIスレッド (`ds_update()`) でも `.store()` / `set_stream_id()` を使用。

5. `ds_get_properties()` 等全てのコールバックも `.load()` / `get_stream_id()` に統一。

---

### [CRITICAL-4] DelayBuffer の非スレッドセーフ操作

**ファイル:** `src/delay-filter.hpp`

**問題:**
`set_delay_ms()` は「音声スレッドから呼ぶこと」とコメントされていたが、実際にはUIスレッド（`ds_update()`）や
UIタスクキュー（`write_sub_delays()`, `write_master_delay()`）から呼ばれていた。
`process()` が `delay_samples_` を読んでいる最中に `set_delay_ms()` が書き換えると、
音声グリッチやバッファ境界外アクセスの可能性があった。

**修正内容:**
`delay_samples_` を `std::atomic<size_t>` に変更（元から atomic だったがコメントと実際の使い方が不整合だった）。
`process()` でフレームバッチ処理開始時に `delay_samples_` のスナップショットを取得し、
バッチ全体で一貫した値を使用するパターンに明確化。

```cpp
void set_delay_ms(uint32_t ms) {
    delay_ms_ = ms;
    size_t ds = (size_t)sample_rate_ * ms / 1000;
    size_t max_samples = buf_.size() / std::max(channels_, 1u);
    if (ds > max_samples) ds = max_samples;
    delay_samples_.store(ds, std::memory_order_relaxed);  // 明示的 store
}

void process(const float* input, float* output, size_t frames) {
    // ...
    size_t ds = delay_samples_.load(std::memory_order_relaxed);  // スナップショット
    for (size_t i = 0; i < frames; ++i) {
        // ds を一貫して使用
    }
}
```

---

### [HIGH-5] SyncFlow コールバック内でのデッドロックリスク

**ファイル:** `src/sync-flow.hpp`

**問題:**
`prober_.on_result` コールバック内で `mtx_` をロック保持したまま `on_update()` を呼んでいた。
`on_update()` → OBS UI タスクキュー → `ds_get_properties()` → `flow.phase()` → `mtx_` 再取得
という経路でデッドロックの可能性があった。

**修正内容:**
`on_update()` の呼び出しをロック外に移動:

```cpp
// 修正前
prober_.on_result = [this](RtmpProbeResult r) {
    std::lock_guard<std::mutex> lk(mtx_);
    // ... state updates ...
    phase_ = FlowPhase::Step3_Done;
    if (on_update) on_update();  // ★ ロック保持中に呼び出し
};

// 修正後
prober_.on_result = [this](RtmpProbeResult r) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        // ... state updates ...
        phase_ = FlowPhase::Step3_Done;
    }
    // コールバックはロック外で呼び出し（デッドロック防止）
    if (on_update) on_update();
};
```

---

### [HIGH-6] サーバー停止時の競合

**ファイル:** `src/websocket-server.hpp`

**問題:**
`stop()` で `server_ptr_.reset()` した後に `conn_map_.clear()` していた。
`on_close()` ハンドラが ASIO スレッドで `server_ptr_` 破棄後も発火し得た。

**修正内容:**
CRITICAL-2 の修正で `stop()` の順序を全面的に書き換え済み（上記参照）。
マップクリア → サーバー停止 → join → reset の正しい順序に。

---

### [MEDIUM-7] RtmpProber のリソースリーク

**ファイル:** `src/rtmp-prober.hpp`

**問題:**
`probe_once()` 内で socket と addrinfo を手動で管理しており、
複数の早期リターンパスでリソースリークの可能性があった。

**修正内容:**
RAII ガードクラスを導入し、スコープ離脱時に自動解放:

```cpp
// 新規追加: RAII ガード
struct AddrInfoGuard {
    addrinfo* p = nullptr;
    ~AddrInfoGuard() { if (p) freeaddrinfo(p); }
};
struct SocketGuard {
    SOCKET s = INVALID_SOCKET;
    ~SocketGuard() { if (s != INVALID_SOCKET) closesocket(s); }
};

double probe_once() {
    AddrInfoGuard ai;
    if (getaddrinfo(..., &ai.p) != 0 || !ai.p)
        return -1.0;  // ai のデストラクタが自動で freeaddrinfo

    SocketGuard sg;
    sg.s = socket(...);
    if (sg.s == INVALID_SOCKET) return -1.0;  // 自動解放

    // ... 以降の早期リターンでも自動解放が保証される
}
```

手動の `closesocket()` / `freeaddrinfo()` 呼び出しをすべて除去。

---

### [MEDIUM-8] TunnelManager のプロセスハンドル競合

**ファイル:** `src/tunnel-manager.hpp`

**問題:**
- `kill_child()` が `proc_handle_` を `CloseHandle` → `INVALID_HANDLE_VALUE` に設定
- 同時にワーカースレッドが `GetExitCodeProcess(proc_handle_, ...)` を呼ぶ
- ロックなしでハンドルにアクセス → 無効ハンドル使用

**修正内容:**
`std::mutex proc_mtx_` を新規追加し、プロセスハンドルへのアクセスをすべて保護:

1. `kill_child()`: 既に `proc_mtx_` 保護済み（前回セッションで修正）
2. `launch_process()`: ハンドル代入を `proc_mtx_` で保護
```cpp
{
    std::lock_guard<std::mutex> lk(proc_mtx_);
    proc_handle_   = pi.hProcess;
    thread_handle_ = pi.hThread;
}
```
3. ngrok 監視ループ: `GetExitCodeProcess` を `proc_mtx_` で保護（前回セッションで修正）
4. cloudflared 監視ループ: 同様に `proc_mtx_` で保護
```cpp
while (!stop_requested_) {
    DWORD exit_code = STILL_ACTIVE;
    {
        std::lock_guard<std::mutex> lk(proc_mtx_);
        if (proc_handle_ != INVALID_HANDLE_VALUE)
            GetExitCodeProcess(proc_handle_, &exit_code);
        else
            break;
    }
    if (exit_code != STILL_ACTIVE) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}
```

---

### [MEDIUM-9] TunnelManager::start() のロック不整合

**ファイル:** `src/tunnel-manager.hpp`

**問題:**
`start()` で `url_`, `error_` をロックなしで書き換えていたが、
`url()`, `error()` は `mtx_` 保護下で読む → 不整合。

**修正内容:**
`start()` での `url_`, `error_` 書き換えを `mtx_` 保護下に:
```cpp
{
    std::lock_guard<std::mutex> lk(mtx_);
    url_   = "";
    error_ = "";
}
```

---

### [P0] ds_destroy() のコールバック安全化

**ファイル:** `src/plugin-main.cpp`

**問題:**
`ds_destroy()` が `delete d` する際、デタッチスレッドやASIOスレッドのコールバックが
破棄済みの `d` を参照してクラッシュする可能性があった。

**修正内容:**
すべてのコールバックを先に `nullptr` 化してから各コンポーネントを停止:

```cpp
static void ds_destroy(void* data) {
    auto* d = static_cast<DelayStreamData*>(data);

    // 1. コールバックをすべて無効化（デタッチスレッドからのアクセス防止）
    d->flow.on_update       = nullptr;
    d->flow.on_ch_measured  = nullptr;
    d->flow.on_apply_sub    = nullptr;
    d->flow.on_apply_master = nullptr;
    d->rtmp.prober.on_result = nullptr;
    d->tunnel.on_url_ready  = nullptr;
    d->tunnel.on_error      = nullptr;
    d->tunnel.on_stopped    = nullptr;
    d->router.clear_callbacks();

    // 2. 各コンポーネントを停止（スレッド join）
    d->flow.reset();
    d->rtmp.prober.cancel();
    d->tunnel.stop();
    d->router.stop();

    // 3. 安全に破棄
    delete d;
}
```

`StreamRouter` にも `clear_callbacks()` メソッドを新規追加し、
全CHの `on_latency_result` コールバックを一括クリア可能にした。

---

## 未実施事項

1. **ビルド確認**: OBS Studio ソースコード環境が当該マシンに存在しないため、コンパイル確認は未実施。OBS SDKのある環境でのビルドテストが必要。
2. **`std::function` コールバックのスレッド安全性**: `on_result`, `on_update` 等の `std::function` メンバ自体は非アトミック。`ds_destroy()` でのnull化とワーカースレッドからの読み取りに微小な競合窓がある。現在は `null化 → cancel()/stop() (join)` の順序で緩和しているが、厳密には `std::atomic<std::shared_ptr<...>>` 等が必要。
3. **ThreadSanitizer (TSan)** によるデータ競合の自動検出テスト。
4. **負荷テスト**: 10CH同時接続 + ネットワーク遅延シミュレーション環境での音声途切れ確認。

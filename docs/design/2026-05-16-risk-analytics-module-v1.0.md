# Risk Analytics Module — Design Document

**Version:** 1.0
**Date:** 2026-05-16
**Status:** Approved design, implementation-ready

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-05-16 | Initial design: EquityCurve (SQLite), RiskMetrics (Sharpe/Sortino/UPI), RiskController (soft/hard limits), SignalEngine integration |

---

## 1. Mục Tiêu

Thiết kế module `src/risk/` để đo lường hiệu suất chiến lược theo **risk-adjusted returns** thay vì chỉ net profit hay CAGR. Module này phục vụ ba mục đích đồng thời:

1. **Live risk monitoring** — phát hiện khi drawdown vượt ngưỡng nguy hiểm và block mở lệnh mới.
2. **Strategy evaluation** — so sánh chiến lược theo UPI (Ulcer Performance Index), không chỉ theo lợi nhuận tuyệt đối.
3. **Reporting / observability** — ghi log metrics định kỳ để operator theo dõi sức khỏe portfolio.

**Vấn đề hiện tại:**

- Bot có `ExposureController` kiểm soát beta exposure nhưng không có bất kỳ drawdown hay risk-adjusted metric nào.
- Không có equity curve history — không thể tính Sharpe, Sortino hay Ulcer Index.
- Không có cơ chế tự động dừng mở lệnh khi account đang trong trạng thái sụt giảm nguy hiểm.

**Tài liệu tham chiếu:** *Universal Tactics of Successful Trend Trading* — Chapter 7: Measuring Risk.

---

## 2. Understanding Lock

### 2.1 Summary

- **Cái gì được xây dựng**: Module C++ `src/risk/` gồm 4 thành phần: `RiskDb`, `EquityCurve`, `RiskMetrics`, `RiskController`.
- **Tại sao**: Survival comes before profit — cần đo lường real drawdown risk, không phải standard deviation giả định returns theo normal distribution.
- **Ba metric chính**: Sharpe ratio → Sortino ratio → Ulcer Performance Index (UPI), theo thứ tự phức tạp tăng dần, UPI là metric ưu tiên cuối cùng.
- **Enforcement**: Soft limit = log warning. Hard limit = block `openPosition()` (giống pattern `ExposureController`).
- **Lưu trữ**: SQLite, đa granularity (per-trade close + periodic snapshot), reset equity curve theo năm dương lịch.

### 2.2 Assumptions

| # | Assumption |
|---|-----------|
| A1 | Risk-free rate mặc định **0%** (crypto không có benchmark risk-free rõ ràng), cấu hình trong `config.json` |
| A2 | Equity data point là `totalWalletBalance` từ `AccountSnapshot` (không phải mark-to-market từng position riêng lẻ) |
| A3 | Equity curve reset về zero tại Jan 1 mỗi năm — mỗi năm là một tập data độc lập (nhất quán với phương pháp tính UI trong sách) |
| A4 | Cần tối thiểu **30 data points** để compute metrics có nghĩa; nếu thiếu, `RiskMetricsResult::isValid()` trả về `false` và không block |
| A5 | `period_return_i = (balance_i - balance_{i-1}) / balance_{i-1}` — return giữa hai consecutive equity points bất kể granularity |
| A6 | Annualization: `estimated_periods_per_year = seconds_per_year / avg_interval_seconds_between_points` |
| A7 | File SQLite tại path cấu hình trong `config.json`, tạo tự động nếu chưa tồn tại |
| A8 | `RiskController` inject vào `SignalEngine` qua interface `IRiskPort` (nhất quán với port pattern: `IExposurePort`, `IScannerPort`...) |
| A9 | Metrics được recompute mỗi `metrics_compute_interval_minutes` (mặc định 60 phút), cache kết quả vào SQLite |
| A10 | Failure mode mặc định **fail-open** — nếu `RiskController` throw exception, `openPosition()` tiếp tục (vì chưa đủ data phase đầu) |

### 2.3 Non-Goals

- Không tính correlation matrix giữa các coin.
- Không tự động rebalance hay đóng position khi vi phạm hard limit (chỉ block MỞ lệnh mới).
- Không có dashboard hay external reporting (log output only).
- Không tính Calmar, MAR, Treynor hay các ratio ngoài ba metric đã chọn.
- Không thay đổi sizing logic (vẫn dùng ATR/risk_pct hiện tại).

### 2.4 Non-Functional Requirements

| Category | Requirement |
|---|---|
| Performance | `canOpenPosition()` < 0.1ms — chỉ đọc cached status, không tính toán hay I/O trong hot path |
| Performance | Recompute metrics chạy async / off hot path (background timer), không block scan cycle |
| Thread safety | `RiskController::status_` protected bởi `std::shared_mutex` (concurrent reads, exclusive write khi update) |
| Persistence | SQLite WAL mode, tránh lock contention giữa insert thread và read thread |
| Testability | `IRiskPort` pure interface, mockable trong `test_signal_engine.cpp` |
| Reliability | Fail-open mặc định — thiếu data không block trading |
| Scale | Equity curve có thể có hàng chục nghìn points mỗi năm (bot chạy 24/7); index trên `(year, timestamp_ms)` |

---

## 3. Architecture

```
src/risk/
├── risk_db.h / risk_db.cpp           # SQLite abstraction layer
├── equity_curve.h / equity_curve.cpp  # Record & query equity data points
├── risk_metrics.h / risk_metrics.cpp  # Compute Sharpe, Sortino, UI, UPI
├── risk_controller.h / risk_controller.cpp  # Threshold enforcement
└── irisk_port.h                       # Pure interface cho SignalEngine
```

**Data flow:**

```
AccountSnapshot::totalWalletBalance
        │
        ▼
  EquityCurve::record()   ◄─── triggered by: position close + periodic scan
        │
        ▼
   RiskDb (SQLite)        ◄─── equity_points table
        │
        ▼ (every N minutes, background)
  RiskMetrics::compute()  ──► RiskMetricsResult {sharpe, sortino, ui, upi, maxDD}
        │                           │
        ▼                           ▼
   RiskDb (SQLite)              RiskController::update()
  risk_metrics_cache                    │
                                        ▼
                              SignalEngine::openPosition()
                                calls canOpenPosition()
```

---

## 4. Data Structures

### 4.1 EquityPoint

```cpp
struct EquityPoint {
    int64_t timestampMs;
    double  walletBalance;
    int     year;           // extracted từ timestampMs
    std::string source;     // "trade_close" | "periodic"
};
```

### 4.2 RiskMetricsResult

```cpp
struct RiskMetricsResult {
    int     year;
    int64_t computedAtMs;
    int     dataPoints;

    // Return metrics
    double annualReturnPct;    // (last_balance - first_balance) / first_balance * 100
    double excessReturnPct;    // annualReturnPct - (riskFreeRate * 100)

    // Sharpe components
    double stdDevAll;          // annualized std dev of ALL period returns
    double sharpeRatio;        // excessReturnPct / stdDevAll

    // Sortino components
    double stdDevDownside;     // annualized std dev of NEGATIVE period returns only
    double sortinoRatio;       // excessReturnPct / stdDevDownside

    // Ulcer components
    double ulcerIndex;         // sqrt(mean(drawdown_pct^2)) — average % drawdown
    double maxDrawdownPct;     // worst single drawdown from high watermark (negative value)
    double upi;                // excessReturnPct / ulcerIndex (Ulcer Performance Index)

    bool isValid() const { return dataPoints >= 30; }
};
```

### 4.3 RiskStatus

```cpp
enum class RiskStatus {
    OK,           // Tất cả metrics trong ngưỡng an toàn
    SOFT_BREACH,  // Vượt soft limit — log warning, vẫn cho phép mở lệnh
    HARD_BREACH   // Vượt hard limit — block openPosition()
};
```

---

## 5. Component Design

### 5.1 RiskDb

**Responsibility:** SQLite abstraction — schema creation, insert, query.

**Schema:**

```sql
-- Lưu mọi equity data point (per-trade-close + periodic)
CREATE TABLE IF NOT EXISTS equity_points (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp_ms  INTEGER NOT NULL,
    wallet_balance REAL   NOT NULL,
    year          INTEGER NOT NULL,
    source        TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_ep_year_ts
    ON equity_points(year, timestamp_ms);

-- Cache kết quả compute metrics (avoid recompute mỗi request)
CREATE TABLE IF NOT EXISTS risk_metrics_cache (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    computed_at_ms  INTEGER NOT NULL,
    year            INTEGER NOT NULL,
    data_points     INTEGER NOT NULL,
    annual_return   REAL,
    excess_return   REAL,
    std_dev_all     REAL,
    std_dev_downside REAL,
    ulcer_index     REAL,
    max_drawdown_pct REAL,
    sharpe_ratio    REAL,
    sortino_ratio   REAL,
    upi             REAL
);
```

**Interface:**

```cpp
class RiskDb {
public:
    explicit RiskDb(const std::string& dbPath);
    ~RiskDb();

    void insertEquityPoint(const EquityPoint& p);
    std::vector<EquityPoint> getByYear(int year) const;

    void insertMetrics(const RiskMetricsResult& m);
    std::optional<RiskMetricsResult> getLatestMetrics(int year) const;

private:
    sqlite3* db_{nullptr};
    void     initSchema();
};
```

**Notes:**
- Mở DB với `PRAGMA journal_mode=WAL` để tránh write lock block read.
- Insert equity point là fire-and-forget (không cần return value).
- `getByYear()` sort theo `timestamp_ms ASC`.

---

### 5.2 EquityCurve

**Responsibility:** Record equity data points từ hai nguồn, delegate persistence xuống `RiskDb`.

```cpp
class EquityCurve {
public:
    EquityCurve(RiskDb& db);

    // Gọi sau mỗi position close
    void recordTradeClose(double walletBalance, int64_t timestampMs);

    // Gọi mỗi periodic scan cycle
    void recordPeriodic(double walletBalance, int64_t timestampMs);

    // Query cho RiskMetrics
    std::vector<EquityPoint> getByYear(int year) const;

private:
    RiskDb& db_;
    void    record(double balance, int64_t ts, const std::string& source);
    int     extractYear(int64_t timestampMs) const;
};
```

**Notes:**
- Không deduplicate — nếu periodic và trade_close xảy ra gần nhau, cả hai đều được lưu. `RiskMetrics` handle noisy data tốt vì dùng mean/std.
- `extractYear()` dùng UTC timestamp → năm dương lịch.

---

### 5.3 RiskMetrics

**Responsibility:** Pure computation — nhận vector `EquityPoint`, trả về `RiskMetricsResult`. Không có side effects.

```cpp
class RiskMetrics {
public:
    explicit RiskMetrics(double riskFreeRateAnnual = 0.0);

    RiskMetricsResult compute(const std::vector<EquityPoint>& points) const;

private:
    double riskFreeRate_;

    // Internal computation steps
    std::vector<double> periodReturns(const std::vector<EquityPoint>& pts) const;
    double annualizedStdDev(const std::vector<double>& returns,
                            double avgIntervalSeconds) const;
    double annualizedDownsideStdDev(const std::vector<double>& returns,
                                    double avgIntervalSeconds) const;
    double computeUlcerIndex(const std::vector<EquityPoint>& pts) const;
    double computeMaxDrawdown(const std::vector<EquityPoint>& pts) const;
    double estimateAvgIntervalSeconds(const std::vector<EquityPoint>& pts) const;
};
```

**Computation logic:**

```
1. period_returns[i] = (pts[i].balance - pts[i-1].balance) / pts[i-1].balance
   (n-1 returns từ n points)

2. annual_return = (pts.back().balance - pts.front().balance) / pts.front().balance

3. avg_interval_s = mean(timestamp_diff_seconds giữa consecutive points)
   periods_per_year = 365 * 24 * 3600 / avg_interval_s

4. std_dev_all = std_dev(period_returns) * sqrt(periods_per_year)
   std_dev_downside = std_dev([r for r if r < 0]) * sqrt(periods_per_year)
   (nếu không có downside return nào → sortino = +∞, set to sentinel 99.0)

5. Ulcer Index:
   high_watermark = pts[0].balance
   for each point:
       high_watermark = max(high_watermark, point.balance)
       dd_pct[i] = (point.balance - high_watermark) / high_watermark * 100
   ulcer_index = sqrt(mean(dd_pct[i]^2))

6. max_drawdown = min(dd_pct[i])   [most negative value]

7. excess_return = annual_return - risk_free_rate   [both in decimal, not pct]

8. sharpe  = excess_return / std_dev_all          (if std_dev_all > 0, else 0)
   sortino = excess_return / std_dev_downside      (if std_dev_downside > 0, else 99.0)
   upi     = excess_return / (ulcer_index / 100)   (convert UI từ % về decimal)
   (nếu ulcer_index == 0 → upi = 99.0)
```

**Note về isValid():** Nếu `dataPoints < 30`, tất cả ratio metrics đều unreliable. Caller (RiskController) kiểm tra `isValid()` trước khi đánh giá threshold.

---

### 5.4 IRiskPort

```cpp
class IRiskPort {
public:
    virtual ~IRiskPort() = default;

    // Gọi từ SignalEngine::openPosition() — hot path
    virtual bool canOpenPosition() const = 0;

    // Gọi sau mỗi position close
    virtual void onPositionClosed(double walletBalance, int64_t timestampMs) = 0;

    // Gọi trong scan cycle (periodic snapshot)
    virtual void onScanCycle(double walletBalance, int64_t timestampMs) = 0;
};
```

---

### 5.5 RiskController

**Responsibility:** Implement `IRiskPort`. Orchestrate `EquityCurve`, `RiskMetrics`, `RiskDb`. Cache computed status. Log breaches.

```cpp
class RiskController : public IRiskPort {
public:
    RiskController(RiskDb& db,
                   EquityCurve& curve,
                   const RiskMetrics& metrics,
                   const RiskConfig& config);

    // IRiskPort
    bool canOpenPosition() const override;
    void onPositionClosed(double walletBalance, int64_t timestampMs) override;
    void onScanCycle(double walletBalance, int64_t timestampMs) override;

    // Gọi từ background timer mỗi metrics_compute_interval_minutes
    void recomputeMetrics();

    RiskMetricsResult latestMetrics() const;
    RiskStatus        currentStatus() const;

private:
    RiskDb&           db_;
    EquityCurve&      curve_;
    const RiskMetrics& metrics_;
    RiskConfig        config_;

    mutable std::shared_mutex mutex_;
    RiskMetricsResult         latest_;
    RiskStatus                status_{RiskStatus::OK};
    int64_t                   lastComputeMs_{0};

    RiskStatus evaluate(const RiskMetricsResult& r) const;
    void       logMetrics(const RiskMetricsResult& r, RiskStatus s) const;
};
```

**evaluate() logic:**

```
HARD_BREACH nếu bất kỳ:
  - maxDrawdownPct < -config.hardMaxDrawdownPct        (drawdown quá sâu)
  - upi < config.hardMinUpi && isValid()               (return per unit risk quá thấp)

SOFT_BREACH nếu bất kỳ:
  - maxDrawdownPct < -config.softMaxDrawdownPct
  - upi < config.softMinUpi && isValid()

Ngược lại: OK
```

**canOpenPosition():**

```cpp
bool RiskController::canOpenPosition() const {
    std::shared_lock lock(mutex_);
    if (!latest_.isValid()) return true;  // fail-open khi thiếu data
    return status_ != RiskStatus::HARD_BREACH;
}
```

**recomputeMetrics() flow:**

```
1. Lấy current year
2. db_.getByYear(year) → vector<EquityPoint>
3. metrics_.compute(points) → RiskMetricsResult
4. db_.insertMetrics(result)
5. status_ = evaluate(result)
6. logMetrics(result, status_)
7. latest_ = result
8. lastComputeMs_ = now()
```

**Background timer:** `SignalEngine` hoặc main loop gọi `riskController_->recomputeMetrics()` mỗi `metrics_compute_interval_minutes` (so sánh `lastComputeMs_` với `now()`). Không spawn thread riêng — chạy trong scan cycle, off hot path của `openPosition()`.

---

## 6. Config Section

Thêm vào `config.json`:

```json
"risk_analytics": {
  "enabled": true,
  "db_path": "data/risk_metrics.db",
  "risk_free_rate": 0.0,
  "min_data_points": 30,
  "metrics_compute_interval_minutes": 60,

  "soft_max_drawdown_pct": 20.0,
  "hard_max_drawdown_pct": 35.0,

  "soft_min_upi": 0.5,
  "hard_min_upi": -1.0
}
```

**Struct tương ứng:**

```cpp
struct RiskConfig {
    bool        enabled{true};
    std::string dbPath{"data/risk_metrics.db"};
    double      riskFreeRate{0.0};
    int         minDataPoints{30};
    int         metricsComputeIntervalMinutes{60};

    double softMaxDrawdownPct{20.0};
    double hardMaxDrawdownPct{35.0};

    double softMinUpi{0.5};
    double hardMinUpi{-1.0};

    static RiskConfig fromJson(const nlohmann::json& j);
};
```

**Giải thích ngưỡng mặc định:**

| Config | Default | Ý nghĩa |
|---|---|---|
| `soft_max_drawdown_pct` | 20% | Account đang down 20% từ peak → log warning |
| `hard_max_drawdown_pct` | 35% | Account down 35% → block mở lệnh mới |
| `soft_min_upi` | 0.5 | UPI < 0.5 (return kém so với drawdown risk) → log warning |
| `hard_min_upi` | -1.0 | UPI < -1.0 (negative return, account đang thua nặng) → block |

---

## 7. SignalEngine Integration

### 7.1 Constructor injection

```cpp
class SignalEngine {
public:
    SignalEngine(
        // ... existing ports ...
        std::unique_ptr<IRiskPort> riskPort = nullptr  // optional, nullable
    );
private:
    std::unique_ptr<IRiskPort> riskPort_;
};
```

Nullable với default `nullptr` để không break existing construction sites hay tests.

### 7.2 openPosition() — thêm risk gate

```cpp
// Sau exposure check, trước khi gọi orders_->open()
if (riskPort_ && !riskPort_->canOpenPosition()) {
    log_.warn("[RiskController] HARD_BREACH — blocking new position for {}", symbol);
    return;
}
```

### 7.3 Sau closePosition()

```cpp
// Sau khi confirm position đã close
if (riskPort_) {
    auto snap = account_->snapshot();
    riskPort_->onPositionClosed(snap.totalWalletBalance, nowMs());
}
```

### 7.4 Trong scan cycle (periodic)

```cpp
// Đầu mỗi runScanCycle()
if (riskPort_) {
    auto snap = account_->snapshot();
    riskPort_->onScanCycle(snap.totalWalletBalance, nowMs());
}
```

### 7.5 Background recompute trong scan cycle

```cpp
// Trong runScanCycle(), kiểm tra interval
if (riskPort_) {
    auto* rc = dynamic_cast<RiskController*>(riskPort_.get());
    if (rc) rc->maybeRecompute();  // internal check lastComputeMs_
}
```

Thêm `maybeRecompute()` vào `IRiskPort` hoặc dùng `RiskController*` trực tiếp nếu không muốn leak interface.

---

## 8. Testing Strategy

| Test file | What to test |
|---|---|
| `test_risk_db.cpp` | Schema creation, insert/query equity points, insert/query metrics cache |
| `test_equity_curve.cpp` | record() gọi đúng db_, extractYear() UTC correct |
| `test_risk_metrics.cpp` | compute() với known dataset → verify sharpe/sortino/upi/maxDD giá trị chính xác; isValid() với < 30 points; zero downside returns → sortino sentinel |
| `test_risk_controller.cpp` | canOpenPosition() false khi HARD_BREACH; true khi !isValid(); evaluate() threshold logic; concurrent reads safe |
| `test_signal_engine.cpp` | Mock `IRiskPort` — verify `canOpenPosition()` được gọi trước open; verify `onPositionClosed()` được gọi sau close |

**Test dataset cho `test_risk_metrics.cpp`:**
Dùng data SP500 1992–2019 từ Table 7.1 trong sách để verify UI = 14.5% — đây là ground truth có sẵn.

---

## 9. Decision Log

| # | Quyết định | Alternatives xem xét | Lý do chọn |
|---|---|---|---|
| D1 | SQLite cho equity curve persistence | File-based JSONL (như OrderJournal), in-memory only | Structured queries theo year, index hiệu quả, tránh parse text khi recompute |
| D2 | Implement cả 3: Sharpe → Sortino → UPI | Chỉ UPI (D option), chỉ UPI + maxDD | User muốn đầy đủ; Sharpe/Sortino là building blocks, code reuse cao |
| D3 | Toàn bộ trong C++ | Python service, hybrid | Consistency với codebase; không có IPC overhead; user chọn rõ ràng |
| D4 | Multi-granularity equity curve | Per-trade only, daily only | Linh hoạt nhất; per-trade cho accuracy khi ít trades; periodic cho bots chạy dài không có trade |
| D5 | Soft = log, Hard = block openPosition() | Hard = close positions, Hard = reduce sizing | Block mở lệnh mới là reversible nhất; giống pattern ExposureController |
| D6 | Risk-free rate mặc định 0% | SOFR, US T-bill yield | Crypto không có chuẩn; 0% là conservative; dễ override trong config |
| D7 | Fail-open khi !isValid() | Fail-closed (block khi không đủ data) | Giai đoạn đầu (<30 data points) không nên block bot — thiếu data ≠ nguy hiểm |
| D8 | Recompute trong scan cycle (background-ish) | Separate thread, every-insert recompute | Tránh spawn thread; recompute 1 lần/giờ là đủ; không I/O trong hot path |
| D9 | IRiskPort nullable trong SignalEngine | Required dependency | Backward compatible với tests hiện có; dễ toggle `enabled: false` |
| D10 | Reset equity curve per calendar year | Rolling 365 days, all-time | Nhất quán với phương pháp tính UI trong sách (Table 7.1 reset per year) |

---

## 10. File Dependencies

```
src/risk/risk_db.h          ← sqlite3.h (system), types/common.h
src/risk/equity_curve.h     ← risk_db.h
src/risk/risk_metrics.h     ← equity_curve.h, <cmath>, <numeric>
src/risk/risk_controller.h  ← risk_metrics.h, risk_db.h, equity_curve.h, <shared_mutex>
src/risk/irisk_port.h       ← (no deps — pure interface)
src/engine/signal_engine.h  ← irisk_port.h (thêm vào existing includes)
```

**CMakeLists.txt:** Thêm `src/risk/*.cpp` vào `target_sources(bot_core ...)`. Link `sqlite3` (đã có nếu bot dùng SQLite; nếu chưa, thêm `find_package(SQLite3 REQUIRED)` và `target_link_libraries(... SQLite3::SQLite3)`).

---

## 11. Rollout Sequence

1. `RiskDb` — SQLite layer, schema, CRUD. Tests: `test_risk_db.cpp`.
2. `EquityCurve` — record/query. Tests: `test_equity_curve.cpp`.
3. `RiskMetrics` — pure computation. Tests: `test_risk_metrics.cpp` với SP500 ground truth.
4. `RiskController` + `IRiskPort`. Tests: `test_risk_controller.cpp`.
5. `SignalEngine` integration — inject port, wire 3 call sites. Tests: mock in `test_signal_engine.cpp`.
6. `config.json` — thêm `risk_analytics` section với defaults.
7. Smoke test: run bot, kiểm tra equity points được insert, metrics được log mỗi 60 phút.

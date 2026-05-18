# Risk Analytics Module — Design Document

**Version:** 1.0
**Date:** 2026-05-16
**Status:** Revised design, implementation-ready

---

## Changelog

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-05-16 | Initial design: EquityCurve (SQLite), RiskMetrics (Sharpe/Sortino/UPI), RiskController (soft/hard limits), SignalEngine integration |
| 1.0-r1 | 2026-05-16 | Architecture review hardening: margin-equity basis for live control, rolling enforcement window, fixed metric units, sampled equity returns, async-compatible SignalEngine integration, config-driven validity threshold |

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
- **Lưu trữ**: SQLite, raw equity events + sampled equity series. Reporting có thể group theo năm dương lịch; live enforcement dùng rolling window, không reset tại Jan 1.

### 2.2 Assumptions

| # | Assumption |
|---|-----------|
| A1 | Risk-free rate mặc định **0%** (crypto không có benchmark risk-free rõ ràng), cấu hình trong `config.json` |
| A2 | Equity basis mặc định cho live control là `AccountSnapshot.account.totalMarginBalance` vì có unrealized PnL. `totalWalletBalance` chỉ dùng khi cấu hình `equity_basis = "wallet"` cho reporting/backtest đặc biệt |
| A3 | Reporting metrics có thể query theo năm dương lịch; hard/soft risk enforcement dùng rolling lookback mặc định **365 ngày** và không reset tại Jan 1 |
| A4 | Cần tối thiểu `risk_analytics.min_data_points` sampled points để compute metrics có nghĩa; nếu thiếu, metrics `valid=false` và không block |
| A5 | Raw events có thể đến từ `periodic` và `trade_close`, nhưng `RiskMetrics` chỉ tính trên sampled equity series đã dedupe/coalesce theo bucket thời gian |
| A6 | Annualization: return dùng CAGR theo elapsed time; volatility dùng fixed sampling cadence hoặc median interval của sampled points, không dùng raw mixed-event average |
| A7 | File SQLite tại path cấu hình trong `config.json`, tạo tự động nếu chưa tồn tại |
| A8 | `RiskController` inject vào `SignalEngine` qua interface `IRiskPort` (nhất quán với port pattern: `IExposurePort`, `IScannerPort`...) |
| A9 | Metrics được recompute mỗi `metrics_compute_interval_minutes` (mặc định 60 phút), cache kết quả vào SQLite theo `window_kind` + `window_start_ms` + `window_end_ms` |
| A10 | Missing-data mode mặc định **open**; exception failure mode cấu hình riêng, mặc định **closed** cho live control để lỗi risk module không âm thầm bypass hard gate |

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
| Performance | Recompute metrics chạy qua explicit `maybeRecompute()` async-compatible path; không chạy trong `openPosition()` hot path. Nếu chạy trong scan cycle thì phải bounded và log duration |
| Thread safety | `RiskController::status_` protected bởi `std::shared_mutex` (concurrent reads, exclusive write khi update) |
| Persistence | SQLite WAL mode, tránh lock contention giữa insert thread và read thread |
| Testability | `IRiskPort` pure interface, mockable trong `test_signal_engine.cpp` |
| Reliability | Thiếu data không block trading; exception trong risk module dùng `failure_mode` cấu hình, mặc định `closed` khi `enabled=true` |
| Scale | Equity curve có thể có hàng chục nghìn points mỗi năm (bot chạy 24/7); indexes trên `(year, timestamp_ms)` và `(basis, timestamp_ms)` |

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
AccountSnapshot.account.totalMarginBalance
        │  (default equity_basis="margin")
        │
        ▼
  EquityCurve::record*()  ◄─── triggered by: position close + periodic scan
        │
        ▼
   RiskDb (SQLite)        ◄─── raw equity_points table
        │
        ▼ (sample/dedupe by bucket)
   sampled equity series
        │
        ▼ (every N minutes)
  RiskMetrics::compute()  ──► RiskMetricsResult {sharpe, sortino, ui, upi, maxDD, valid}
        │                           │
        ▼                           ▼
   RiskDb (SQLite)              RiskController::update()
  risk_metrics_cache                    │ rolling window, no Jan 1 reset
                                        ▼
                              SignalEngine::openPosition()
                                calls canOpenPosition()
```

---

## 4. Data Structures

### 4.1 EquityPoint

```cpp
struct EquityPoint {
    int64_t id;             // SQLite row id, used for deterministic tie-breaks
    int64_t timestampMs;
    double  equity;
    int     year;           // extracted từ timestampMs
    std::string source;     // "trade_close" | "periodic"
    std::string basis;      // "margin" | "wallet"
};
```

`equity` mặc định lấy từ `AccountSnapshot.account.totalMarginBalance`.
Nếu `basis == "wallet"` thì lấy `AccountSnapshot.account.totalWalletBalance`.

### 4.2 SampledEquityPoint

```cpp
struct SampledEquityPoint {
    int64_t timestampMs;    // bucket boundary or last event timestamp in bucket
    double  equity;
};
```

`RiskMetrics` nhận sampled points, không nhận raw mixed-granularity events.
Sampling rule mặc định: một point cuối cùng trong mỗi `sample_interval_minutes` bucket.
Nếu bucket có cả `periodic` và `trade_close`, chọn event mới nhất trong bucket.

### 4.3 RiskMetricsResult

```cpp
struct RiskMetricsResult {
    std::string windowKind;      // "rolling" | "calendar_year"
    int64_t     windowStartMs;
    int64_t     windowEndMs;
    int64_t     computedAtMs;
    int         dataPoints;
    bool        valid;

    // Return metrics
    double annualReturn;       // decimal CAGR, e.g. 0.12 = 12%
    double excessReturn;       // decimal annualReturn - riskFreeRateAnnual

    // Sharpe components
    double stdDevAll;          // annualized decimal std dev of ALL period returns
    double sharpeRatio;        // excessReturn / stdDevAll

    // Sortino components
    double stdDevDownside;     // annualized decimal downside std dev
    double sortinoRatio;       // excessReturn / stdDevDownside

    // Ulcer components
    double ulcerIndex;         // decimal, e.g. 0.145 = 14.5%
    double maxDrawdown;        // decimal, worst drawdown from high watermark, e.g. -0.35
    double upi;                // excessReturn / ulcerIndex

    bool isValid() const { return valid; }
};
```

All return/risk fields are stored as decimals. Format as percentages only at log/report boundaries.

### 4.4 RiskStatus

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
    equity        REAL    NOT NULL,
    year          INTEGER NOT NULL,
    source        TEXT    NOT NULL,
    basis         TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_ep_year_ts
    ON equity_points(year, timestamp_ms);
CREATE INDEX IF NOT EXISTS idx_ep_basis_ts
    ON equity_points(basis, timestamp_ms);

-- Cache kết quả compute metrics theo window
CREATE TABLE IF NOT EXISTS risk_metrics_cache (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    computed_at_ms  INTEGER NOT NULL,
    window_kind     TEXT    NOT NULL,
    window_start_ms INTEGER NOT NULL,
    window_end_ms   INTEGER NOT NULL,
    basis           TEXT    NOT NULL,
    data_points     INTEGER NOT NULL,
    valid           INTEGER NOT NULL,
    annual_return   REAL,
    excess_return   REAL,
    std_dev_all     REAL,
    std_dev_downside REAL,
    ulcer_index     REAL,
    max_drawdown    REAL,
    sharpe_ratio    REAL,
    sortino_ratio   REAL,
    upi             REAL
);
CREATE INDEX IF NOT EXISTS idx_rmc_window
    ON risk_metrics_cache(window_kind, basis, window_end_ms);
```

**Interface:**

```cpp
class RiskDb {
public:
    explicit RiskDb(const std::string& dbPath);
    ~RiskDb();

    void insertEquityPoint(const EquityPoint& p);
    std::vector<EquityPoint> getByYear(int year) const;
    std::vector<EquityPoint> getByTimeRange(
        std::string_view basis,
        int64_t startMs,
        int64_t endMs) const;

    void insertMetrics(const RiskMetricsResult& m);
    std::optional<RiskMetricsResult> getLatestMetrics(
        std::string_view windowKind,
        std::string_view basis) const;

private:
    sqlite3* db_{nullptr};
    void     initSchema();
};
```

**Notes:**

- Mở DB với `PRAGMA journal_mode=WAL` để tránh write lock block read.
- Insert equity point là fire-and-forget (không cần return value).
- `getByYear()` sort theo `timestamp_ms ASC`.
- `getByTimeRange()` sort theo `timestamp_ms ASC` và filter đúng `basis`.
- `RiskDb` phải tạo parent directory của `db_path` trước khi mở SQLite.

---

### 5.2 EquityCurve

**Responsibility:** Record equity data points từ hai nguồn, delegate persistence xuống `RiskDb`.

```cpp
class EquityCurve {
public:
    EquityCurve(RiskDb& db);

    // Gọi sau mỗi position close
    void recordTradeClose(double equity, int64_t timestampMs, std::string_view basis);

    // Gọi mỗi periodic scan cycle
    void recordPeriodic(double equity, int64_t timestampMs, std::string_view basis);

    // Query cho RiskMetrics
    std::vector<EquityPoint> getByYear(int year) const;
    std::vector<EquityPoint> getByTimeRange(std::string_view basis, int64_t startMs, int64_t endMs) const;

private:
    RiskDb& db_;
    void    record(double equity, int64_t ts, std::string_view source, std::string_view basis);
    int     extractYear(int64_t timestampMs) const;
};
```

**Notes:**

- Raw layer không deduplicate để giữ audit trail. Sampling/dedupe diễn ra khi build input cho `RiskMetrics`.
- `extractYear()` dùng UTC timestamp → năm dương lịch.
- Reject hoặc skip record nếu `equity <= 0` vì return và drawdown math không xác định an toàn.

---

### 5.3 RiskMetrics

**Responsibility:** Pure computation — nhận vector `SampledEquityPoint`, trả về `RiskMetricsResult`. Không có side effects.

```cpp
class RiskMetrics {
public:
    RiskMetrics(double riskFreeRateAnnual = 0.0,
                int minDataPoints = 30,
                std::chrono::minutes sampleInterval = std::chrono::minutes{60});

    RiskMetricsResult compute(
        const std::vector<SampledEquityPoint>& points,
        std::string_view windowKind,
        int64_t windowStartMs,
        int64_t windowEndMs) const;

private:
    double riskFreeRate_;
    int    minDataPoints_;
    std::chrono::minutes sampleInterval_;

    // Internal computation steps
    std::vector<double> periodReturns(const std::vector<SampledEquityPoint>& pts) const;
    double annualizedReturn(const std::vector<SampledEquityPoint>& pts) const;
    double annualizedStdDev(const std::vector<double>& returns) const;
    double annualizedDownsideStdDev(const std::vector<double>& returns) const;
    double computeUlcerIndex(const std::vector<SampledEquityPoint>& pts) const;
    double computeMaxDrawdown(const std::vector<SampledEquityPoint>& pts) const;
};
```

**Computation logic:**

```
0. Input points are sampled, sorted ASC, have unique bucket timestamps, and all equity values > 0.

1. period_returns[i] = (pts[i].equity - pts[i-1].equity) / pts[i-1].equity
   (n-1 returns từ n points)

2. elapsed_years = (pts.back().timestampMs - pts.front().timestampMs) / milliseconds_per_year
   annual_return = pow(pts.back().equity / pts.front().equity, 1 / elapsed_years) - 1
   Nếu elapsed_years <= 0 hoặc equity đầu/cuối <= 0 → valid=false.

3. periods_per_year = 365 * 24 * 60 / sample_interval_minutes

4. std_dev_all = std_dev(period_returns) * sqrt(periods_per_year)
   std_dev_downside = std_dev([r for r if r < 0]) * sqrt(periods_per_year)
   Nếu không có downside return nào → sortino = 99.0 sentinel.

5. Ulcer Index:
   high_watermark = pts[0].equity
   for each point:
       high_watermark = max(high_watermark, point.equity)
       dd[i] = (point.equity - high_watermark) / high_watermark
   ulcer_index = sqrt(mean(dd[i]^2))

6. max_drawdown = min(dd[i])   [most negative value, decimal]

7. excess_return = annual_return - risk_free_rate   [both in decimal, not pct]

8. sharpe  = excess_return / std_dev_all          (if std_dev_all > 0, else 0)
   sortino = excess_return / std_dev_downside      (if std_dev_downside > 0, else 99.0)
   upi     = excess_return / ulcer_index
   (nếu ulcer_index == 0 → upi = 99.0)
```

**Note về validity:** Nếu `dataPoints < minDataPoints`, elapsed time không hợp lệ, hoặc có equity <= 0, set `valid=false`. Caller (RiskController) kiểm tra `isValid()` trước khi đánh giá threshold.

### 5.3.1 Sampling raw equity events

```cpp
std::vector<SampledEquityPoint> sampleEquity(
    const std::vector<EquityPoint>& raw,
    int64_t windowStartMs,
    int64_t windowEndMs,
    std::chrono::minutes sampleInterval);
```

Sampling contract:

1. Filter raw events to `[windowStartMs, windowEndMs]` and selected `basis`.
2. Compute `bucket = (timestampMs - windowStartMs) / sampleIntervalMs`.
3. Keep the latest raw event per bucket.
4. Return buckets sorted by timestamp ASC.
5. If multiple events have identical timestamp in a bucket, prefer `trade_close` over `periodic`, then last inserted row id.

---

### 5.4 IRiskPort

```cpp
class IRiskPort {
public:
    virtual ~IRiskPort() = default;

    // Gọi từ SignalEngine::openPosition() — hot path
    virtual bool canOpenPosition() const = 0;

    // Gọi sau mỗi position close
    virtual void onPositionClosed(const account::AccountSnapshot& snapshot, int64_t timestampMs) = 0;

    // Gọi trong scan cycle (periodic snapshot)
    virtual void onScanCycle(const account::AccountSnapshot& snapshot, int64_t timestampMs) = 0;

    // Gọi ngoài openPosition() hot path; implementation tự kiểm tra compute interval
    virtual boost::asio::awaitable<void> maybeRecompute(int64_t nowMs) = 0;
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
    void onPositionClosed(const account::AccountSnapshot& snapshot, int64_t timestampMs) override;
    void onScanCycle(const account::AccountSnapshot& snapshot, int64_t timestampMs) override;
    boost::asio::awaitable<void> maybeRecompute(int64_t nowMs) override;

    // Gọi từ maybeRecompute() khi đủ interval
    void recomputeMetrics(int64_t nowMs);

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
    std::pair<int64_t, int64_t> rollingWindow(int64_t nowMs) const;
    double     selectEquity(const account::AccountSnapshot& snapshot) const;
};
```

**evaluate() logic:**

```
HARD_BREACH nếu bất kỳ:
  - maxDrawdown < -config.hardMaxDrawdown              (drawdown quá sâu)
  - upi < config.hardMinUpi && isValid()               (return per unit risk quá thấp)

SOFT_BREACH nếu bất kỳ:
  - maxDrawdown < -config.softMaxDrawdown
  - upi < config.softMinUpi && isValid()

Ngược lại: OK
```

**canOpenPosition():**

```cpp
bool RiskController::canOpenPosition() const {
    std::shared_lock lock(mutex_);
    if (status_ == RiskStatus::HARD_BREACH) return false;
    if (!latest_.isValid()) return config_.missingDataMode == RiskMissingDataMode::Open;
    return true;
}
```

**recomputeMetrics() flow:**

```
1. Tính rolling window: `[now - control_lookback_days, now]`
2. db_.getByTimeRange(toString(config.equityBasis), start, now) → raw vector<EquityPoint>
3. sampleEquity(raw, start, now, config.sampleIntervalMinutes) → vector<SampledEquityPoint>
4. metrics_.compute(sampled, "rolling", start, now) → RiskMetricsResult
5. db_.insertMetrics(result)
6. status_ = evaluate(result)
7. logMetrics(result, status_)
8. latest_ = result
9. lastComputeMs_ = now()
```

**Timer contract:** `SignalEngine` gọi `riskPort_->maybeRecompute(nowMs)` ngoài `openPosition()` hot path. `maybeRecompute()` so sánh `lastComputeMs_` với `metrics_compute_interval_minutes`. Implementation đầu tiên có thể chạy synchronous trong scan cycle vì dữ liệu nhỏ; phải log duration và không được gọi trong per-symbol loop. Nếu duration vượt ngưỡng vận hành, nâng cấp sang worker thread/strand sau.

**Exception policy:** `maybeRecompute()` catch exception, log error, và set `status_=HARD_BREACH` khi `failure_mode="closed"`. Khi `failure_mode="open"`, giữ status trước đó và cho phép tiếp tục.

---

## 6. Config Section

Thêm vào `config.json`:

```json
"risk_analytics": {
  "enabled": true,
  "db_path": "data/risk_metrics.db",
  "equity_basis": "margin",
  "risk_free_rate": 0.0,
  "min_data_points": 30,
  "sample_interval_minutes": 60,
  "control_lookback_days": 365,
  "metrics_compute_interval_minutes": 60,
  "missing_data_mode": "open",
  "failure_mode": "closed",

  "soft_max_drawdown": 0.20,
  "hard_max_drawdown": 0.35,

  "soft_min_upi": 0.5,
  "hard_min_upi": -1.0
}
```

**Struct tương ứng:**

```cpp
enum class RiskEquityBasis { Margin, Wallet };
enum class RiskMissingDataMode { Open, Closed };
enum class RiskFailureMode { Open, Closed };

struct RiskConfig {
    bool        enabled{true};
    std::string dbPath{"data/risk_metrics.db"};
    RiskEquityBasis equityBasis{RiskEquityBasis::Margin};
    double      riskFreeRate{0.0};
    int         minDataPoints{30};
    int         sampleIntervalMinutes{60};
    int         controlLookbackDays{365};
    int         metricsComputeIntervalMinutes{60};
    RiskMissingDataMode missingDataMode{RiskMissingDataMode::Open};
    RiskFailureMode failureMode{RiskFailureMode::Closed};

    double softMaxDrawdown{0.20};
    double hardMaxDrawdown{0.35};

    double softMinUpi{0.5};
    double hardMinUpi{-1.0};

    static RiskConfig fromJson(const nlohmann::json& j);
};
```

**Giải thích ngưỡng mặc định:**

| Config | Default | Ý nghĩa |
|---|---|---|
| `equity_basis` | `margin` | Dùng `totalMarginBalance`, phản ánh unrealized PnL cho live control |
| `sample_interval_minutes` | 60 | Chuẩn hóa raw events thành hourly equity points |
| `control_lookback_days` | 365 | Rolling window cho hard/soft enforcement; không reset Jan 1 |
| `missing_data_mode` | `open` | Thiếu sampled points thì không block trading |
| `failure_mode` | `closed` | Lỗi risk module khi đang enabled thì block mở lệnh mới |
| `soft_max_drawdown` | 0.20 | Account đang down 20% từ peak → log warning |
| `hard_max_drawdown` | 0.35 | Account down 35% → block mở lệnh mới |
| `soft_min_upi` | 0.5 | UPI < 0.5 (return kém so với drawdown risk) → log warning |
| `hard_min_upi` | -1.0 | UPI < -1.0 (negative return, account đang thua nặng) → block |

Validation:

- `sample_interval_minutes > 0`
- `control_lookback_days > 0`
- `metrics_compute_interval_minutes > 0`
- `min_data_points >= 2`
- `0 < soft_max_drawdown <= hard_max_drawdown < 1`
- `equity_basis` chỉ nhận `"margin"` hoặc `"wallet"`
- `missing_data_mode` và `failure_mode` chỉ nhận `"open"` hoặc `"closed"`

---

## 7. SignalEngine Integration

### 7.1 Constructor injection

```cpp
class SignalEngine {
public:
    SignalEngine(
        // ... existing ports ...
        IRiskPort* riskPort = nullptr  // optional, nullable; lifetime owned by composition root
    );
private:
    IRiskPort* riskPort_{nullptr};
};
```

Nullable với default `nullptr` để không break existing construction sites hay tests.
Repo hiện giữ các port khác bằng reference; dùng raw nullable pointer cho optional risk port để không trộn ownership model vào `SignalEngine`.

### 7.2 openPosition() — thêm risk gate

```cpp
// Sau order cap + exposure check, trước Gemini evaluation và trước khi gọi orders_->market()
if (riskPort_ && !riskPort_->canOpenPosition()) {
    log_.warn("[RiskController] HARD_BREACH — blocking new position for {}", symbol);
    co_return Result<void>{};
}
```

### 7.3 Sau closePosition()

```cpp
// Sau khi confirm position đã close
if (riskPort_) {
    account::AccountSnapshotRequest request;
    request.includePositions = true;
    const auto snapshotResult = co_await m_account.snapshot(request);
    if (snapshotResult) {
        riskPort_->onPositionClosed(*snapshotResult, nowMs());
    }
}
```

### 7.4 Trong scan cycle (periodic)

```cpp
// Đầu mỗi runScanCycle()
if (riskPort_) {
    account::AccountSnapshotRequest request;
    request.includePositions = true;
    const auto snapshotResult = co_await m_account.snapshot(request);
    if (snapshotResult) {
        riskPort_->onScanCycle(*snapshotResult, nowMs());
    }
}
```

### 7.5 Background recompute trong scan cycle

```cpp
// Trong runScanCycle(), kiểm tra interval
if (riskPort_) {
    co_await riskPort_->maybeRecompute(nowMs());
}
```

`RiskController` chọn equity basis, không để `SignalEngine` biết chi tiết config risk:

```cpp
double RiskController::selectEquity(const account::AccountSnapshot& snap) const {
    switch (config_.equityBasis) {
    case RiskEquityBasis::Margin:
        return snap.account.totalMarginBalance;
    case RiskEquityBasis::Wallet:
        return snap.account.totalWalletBalance;
    }
    return snap.account.totalMarginBalance;
}
```

Placement trong `runScanCycle()`:

1. Record periodic snapshot một lần ở đầu cycle.
2. Process queue như hiện tại.
3. Gọi `co_await riskPort_->maybeRecompute(nowMs())` sau queue và trước `logExposureMetrics()`, không gọi trong per-symbol loop.

---

## 8. Testing Strategy

| Test file | What to test |
|---|---|
| `test_risk_db.cpp` | Schema creation, parent directory creation, insert/query equity points by basis/time range, insert/query latest metrics by window |
| `test_equity_curve.cpp` | record() gọi đúng db_, extractYear() UTC correct, rejects/skips equity <= 0, stores source + basis |
| `test_risk_metrics.cpp` | compute() với known dataset → verify sharpe/sortino/upi/maxDD giá trị chính xác; decimal unit consistency; config-driven minDataPoints; zero downside returns → sortino sentinel |
| `test_risk_sampling.cpp` | raw mixed events → one sampled point per bucket; duplicate timestamp tie-break; no divide-by-zero interval; sorted output |
| `test_risk_controller.cpp` | canOpenPosition() false khi HARD_BREACH; missingDataMode open/closed; failureMode open/closed; rolling window excludes old points; margin vs wallet equity basis |
| `test_signal_engine.cpp` | Mock `IRiskPort` — verify `canOpenPosition()` được gọi sau order cap/exposure và trước Gemini/order placement; verify `onScanCycle()` dùng awaited account snapshot; verify `maybeRecompute()` gọi ngoài per-symbol loop |

**Test dataset cho `test_risk_metrics.cpp`:**
Dùng data SP500 1992–2019 từ Table 7.1 trong sách để verify UI = 14.5% sau khi convert decimal (`0.145`). Thêm synthetic dataset nhỏ để verify CAGR annualization và UPI không lệch hệ số 100.

---

## 9. Decision Log

| # | Quyết định | Alternatives xem xét | Lý do chọn |
|---|---|---|---|
| D1 | SQLite cho equity curve persistence | File-based JSONL (như OrderJournal), in-memory only | Structured queries theo year, index hiệu quả, tránh parse text khi recompute |
| D2 | Implement cả 3: Sharpe → Sortino → UPI | Chỉ UPI (D option), chỉ UPI + maxDD | User muốn đầy đủ; Sharpe/Sortino là building blocks, code reuse cao |
| D3 | Toàn bộ trong C++ | Python service, hybrid | Consistency với codebase; không có IPC overhead; user chọn rõ ràng |
| D4 | Raw multi-granularity events + sampled metrics series | Per-trade only, daily only | Giữ audit trail đầy đủ nhưng metrics dùng cadence ổn định, tránh annualization sai do event dày/thưa bất thường |
| D5 | Soft = log, Hard = block openPosition() | Hard = close positions, Hard = reduce sizing | Block mở lệnh mới là reversible nhất; giống pattern ExposureController |
| D6 | Risk-free rate mặc định 0% | SOFR, US T-bill yield | Crypto không có chuẩn; 0% là conservative; dễ override trong config |
| D7 | Missing data open, exception failure closed | Fail-closed mọi trường hợp, fail-open mọi trường hợp | Giai đoạn đầu thiếu data không nên block; nhưng lỗi runtime của risk module khi enabled không được âm thầm bypass gate |
| D8 | `maybeRecompute()` ngoài hot path, bounded trong scan cycle trước | Separate thread, every-insert recompute | Tránh spawn thread ở bản đầu; recompute 1 lần/giờ là đủ; có log duration để biết khi nào cần worker thread |
| D9 | IRiskPort nullable trong SignalEngine | Required dependency | Backward compatible với tests hiện có; dễ toggle `enabled: false` |
| D10 | Rolling 365-day window cho enforcement; calendar-year query cho reporting | Reset mọi thứ theo năm, all-time only | Live risk gate không được mất drawdown tại Jan 1; reporting vẫn có thể xuất calendar-year metrics |
| D11 | Default equity basis là `totalMarginBalance` | `totalWalletBalance` | Live drawdown phải thấy unrealized PnL của futures positions; wallet balance chỉ phù hợp một số báo cáo realized-only |

---

## 10. File Dependencies

```
src/risk/risk_db.h          ← sqlite3.h (system), types/common.h
src/risk/equity_curve.h     ← risk_db.h
src/risk/risk_metrics.h     ← equity_curve.h, <chrono>, <cmath>, <numeric>
src/risk/risk_controller.h  ← risk_metrics.h, risk_db.h, equity_curve.h, account/account_snapshot.h, <shared_mutex>
src/risk/irisk_port.h       ← account/account_snapshot.h, boost/asio/awaitable.hpp
src/engine/signal_engine.h  ← irisk_port.h (thêm vào existing includes)
```

**CMakeLists.txt:** Repo hiện chưa link SQLite. Thêm:

```cmake
find_package(SQLite3 REQUIRED)

# Add risk sources to LIB_SOURCES:
src/risk/risk_db.cpp
src/risk/equity_curve.cpp
src/risk/risk_metrics.cpp
src/risk/risk_controller.cpp

# Add risk headers to LIB_HEADERS.

target_link_libraries(bot_lib PUBLIC SQLite::SQLite3)
```

Nếu Windows preset không tìm thấy SQLite system package, dùng `FetchContent`/vcpkg làm bước riêng; không để implementation phụ thuộc vào thư viện chưa được CMake resolve.

---

## 11. Rollout Sequence

1. CMake SQLite integration — add dependency and verify Windows preset config/build resolves `SQLite::SQLite3`.
2. `RiskDb` — SQLite layer, schema, parent directory creation, CRUD. Tests: `test_risk_db.cpp`.
3. `EquityCurve` — raw record/query by basis/time range. Tests: `test_equity_curve.cpp`.
4. Sampling helper — bucket/dedupe raw events. Tests: `test_risk_sampling.cpp`.
5. `RiskMetrics` — pure decimal computation. Tests: `test_risk_metrics.cpp` với SP500 ground truth + synthetic CAGR/UPI cases.
6. `RiskController` + `IRiskPort` — rolling enforcement, basis selection, failure policy. Tests: `test_risk_controller.cpp`.
7. `SignalEngine` integration — inject nullable pointer, wire `canOpenPosition`, `onScanCycle`, `onPositionClosed`, `maybeRecompute`. Tests: mock in `test_signal_engine.cpp`.
8. `config.json` — thêm `risk_analytics` section với defaults.
9. Smoke test: run bot, kiểm tra equity points basis=`margin` được insert, sampled metrics được log mỗi 60 phút, hard breach block mở lệnh mới trong synthetic/manual test.

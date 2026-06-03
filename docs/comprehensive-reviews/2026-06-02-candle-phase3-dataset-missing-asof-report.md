# Bao cao loi CANDLE PHASE3 dataset_missing_asof

Ngay lap bao cao: 2026-06-02  
Pham vi: Qlib orchestration Phase 3 / CandleSchedulerThread  
Log can dieu tra:

```text
[2026-06-02 09:03:41.602] [WARNING] [CANDLE][PHASE3][SKIPPED] asof=1780363800000 reason=dataset_missing_asof
```

## Ket luan ngan gon

Day la loi that cua pipeline Qlib Phase 3, khong phai warning vo hai. Phase 3 da nhan duoc su kien candle close, nhung bo qua prediction vi dataset khong co row tuong ung voi `asof=1780363800000`.

`1780363800000` tuong ung:

- UTC: `2026-06-02 01:30:00`
- Asia/Bangkok: `2026-06-02 08:30:00 +07`

Voi log luc `2026-06-02 09:03 +07`, candle `08:30` da dong va dang duoc Phase 3 xu ly sau delay. Tuy nhien file dataset runtime hien tai chi moi co du lieu toi `2026-05-30 08:00:00`, nen khong the co prediction cho candle ngay `2026-06-02 08:30`.

Ngoai viec dataset stale, con co mot bug contract: code C++ dang kiem tra asof bang cach parse so nguyen o dau dong CSV, trong khi dataset Qlib bridge hien tai dung cot `datetime` dang chuoi. Vi vay, ngay ca khi dataset co row dung ngay gio, pre-check trong scheduler van co the false-negative neu row khong bat dau bang epoch milliseconds.

## Trieu chung

Log lap lai moi khoang 5 giay:

```text
[CANDLE][PHASE3][SKIPPED] asof=1780363800000 reason=dataset_missing_asof
```

Mau lap nay xuat hien vi `CandleSchedulerThread::run()` chi cap nhat `m_lastProcessedMs` khi `processCandle()` thanh cong. Neu `processCandle()` tra ve `false`, cung mot `asof` se tiep tuc duoc retry sau delay.

Code lien quan:

- `src/orchestration/candle_scheduler_thread.cpp`
  - `datasetContainsAsofMs()`: doc dataset va tim asof.
  - `processCandle()`: log `dataset_missing_asof` neu check fail.
  - `run()`: chi advance cursor khi `processed == true`.

## Dong su kien runtime

1. Market scanner nhan closed kline tu websocket/cache.
2. `src/main.cpp` goi `candleScheduler->notifyCandleClose(openTimeMs, symbol)`.
3. `CandleSchedulerThread` coalesce thanh `m_pendingCandleMs`.
4. Sau `postCandleDelaySeconds`, scheduler goi `processCandle(asof)`.
5. `processCandle()` goi `datasetContainsAsofMs(asof)`.
6. Dataset check fail.
7. Phase 3 khong chay `predict_latest.py`; log:

```text
[CANDLE][PHASE3][SKIPPED] asof=1780363800000 reason=dataset_missing_asof
```

## Bang chung tu config va dataset

Config runtime dang tro Phase 3 toi smoke dataset:

```json
"qlib_orchestration": {
  "enabled": true,
  "data_dir": "data/qlib_smoke",
  "interval": "30m",
  "model_id": "lightgbm_30m_v1"
}
```

Dataset tuong ung:

```text
data/qlib_smoke/klines_30m.csv
```

Header va dong dau:

```text
datetime,symbol,open,high,low,close,volume,factor,quote_volume,trade_count
2025-05-30 08:30:00,BTCUSDT,105186.1,105323.2,105078.3,105129.6,2937.193,1.0,309049823.0081,64881
```

Nhung dong cuoi hien tai:

```text
2026-05-30 06:00:00,ETHUSDT,2015.03,2016.51,2011.89,2016.01,17333.3,1.0,34911479.14407,24316
2026-05-30 06:30:00,ETHUSDT,2016.01,2019.79,2015.19,2016.01,36165.816,1.0,72959556.50423,43877
2026-05-30 07:00:00,ETHUSDT,2016.0,2017.2,2013.89,2016.53,21259.454,1.0,42840871.18538,26163
2026-05-30 07:30:00,ETHUSDT,2016.54,2016.83,2012.73,2015.33,16554.854,1.0,33344799.10419,22697
2026-05-30 08:00:00,ETHUSDT,2015.33,2015.33,2014.92,2014.93,477.516,1.0,962270.06447,482
```

Khong tim thay row:

```text
2026-06-02 08:30:00
```

Do do, voi trang thai dataset hien tai, `dataset_missing_asof` la hop le: candle can predict khong ton tai trong dataset.

## Nguyen nhan goc

### 1. Dataset runtime bi stale

File `data/qlib_smoke/klines_30m.csv` chi toi `2026-05-30 08:00:00`, trong khi Phase 3 dang can `2026-06-02 08:30:00 +07`.

Tac dong truc tiep:

- `predict_latest.py` khong duoc goi.
- SQLite `qlib_predictions` khong co prediction moi cho asof nay.
- Ready flag `ready_<asof>.flag` khong duoc ghi.
- Qlib adapter co the tiep tuc thay state shadow/canary/live, nhung prediction moi bi stale hoac khong co.

### 2. Thieu buoc refresh latest candle truoc prediction

Tai lieu design `docs/design/2026-05-20-qlib-orchestration-v1.1.md` mo ta Phase 3 flow can co buoc:

```text
Run LatestCandleRefresher for all configured symbols.
```

Va command du kien:

```text
tools/qlib_bridge/refresh_latest_candles.py
  --symbols BTCUSDT ETHUSDT ...
  --interval <interval>
  --asof-ms <candleOpenTimeMs>
  --dataset <datasetPath>
  --db-path <dbPath>
  --merge-mode upsert
```

Script `tools/qlib_bridge/refresh_latest_candles.py` da ton tai va co kha nang fetch exact candle theo `--asof-ms`, upsert vao dataset va upsert vao bang `qlib_candles`.

Nhung `CandleSchedulerThread::buildPhase3Cmd()` hien chi tao command chay:

```text
tools/qlib_bridge/predict_latest.py
```

Khong co command refresh dataset truoc khi predict. Neu dataset khong duoc cap nhat boi job khac, Phase 3 se bi skip lien tuc.

### 3. Dataset pre-check trong C++ lech schema voi Python bridge

`predict_latest.py` dang yeu cau schema:

```text
datetime,symbol,open,high,low,close,volume,...
```

Va no tu convert `datetime` sang `asof_open_time_ms`:

```python
scored["asof_open_time_ms"] = _to_epoch_ms(scored["datetime"])
```

Trong khi `CandleSchedulerThread::datasetContainsAsofMs()` lai doc tung dong CSV va goi `parseLeadingInt64(line, &openTime)`. Cach nay chi dung voi dataset cu co cot dau la epoch milliseconds, vi du:

```text
1700000000000,100,101,99,100,1
```

Dataset runtime hien tai bat dau bang chuoi datetime:

```text
2026-05-30 08:00:00,ETHUSDT,...
```

Voi format nay, `parseLeadingInt64()` khong parse duoc ca token `2026-05-30 08:00:00`, nen check fail. Neu parser chi lay prefix `2026`, gia tri do cung khong the bang epoch-ms `1780363800000`.

Ket qua: scheduler co the bao `dataset_missing_asof` sai neu dataset co row dung asof nhung o format datetime.

### 4. Test hien tai dang cover format cu

`tests/test_candle_scheduler_thread.cpp` seed dataset bang cot dau `open_time_ms`, vi du:

```text
open_time_ms,open,high,low,close,volume
1700000000000,100,101,99,100,1
```

Test nay hop voi implementation C++ hien tai, nhung khong hop voi dataset that ma `predict_latest.py` dang dung. Can them test cho schema `datetime,symbol,...`.

## Tac dong

Muc do: High cho Qlib Phase 3, Medium cho trading core tong the.

Tac dong truc tiep:

- Qlib prediction moi khong duoc sinh ra cho candle vua dong.
- Promotion Phase 4 khong duoc evaluate sau Phase 3 success, vi Phase 3 khong success.
- Qlib shadow outcomes va live/canary behavior co the bi tre hoac stale tuy theo adapter doc state/prediction nhu the nao.
- Log warning lap lai, lam nhieu log va che lap warning khac.

Khong phai tac dong truc tiep:

- Khong phai loi websocket market scanner.
- Khong phai loi order placement.
- Khong phai loi auth Binance.
- Khong phai loi time-exit monitor.

## Cach xac minh nhanh

Quy doi asof:

```powershell
[DateTimeOffset]::FromUnixTimeMilliseconds(1780363800000).ToUniversalTime()
[DateTimeOffset]::FromUnixTimeMilliseconds(1780363800000).ToOffset([TimeSpan]::FromHours(7))
```

Kiem tra dataset co row asof khong:

```powershell
Select-String -LiteralPath data/qlib_smoke/klines_30m.csv -Pattern '^2026-06-02 08:30:00'
```

Kiem tra dataset cuoi file:

```powershell
Get-Content -LiteralPath data/qlib_smoke/klines_30m.csv -Tail 5
```

Neu file van ket thuc o `2026-05-30 08:00:00`, Phase 3 chac chan khong the predict cho `2026-06-02 08:30:00`.

## Huong fix triet de

### Fix A: Them LatestCandleRefresher vao Phase 3

Truoc khi check dataset va truoc khi goi `predict_latest.py`, scheduler can goi `refresh_latest_candles.py` voi:

- danh sach symbols can predict,
- interval,
- `asof-ms`,
- dataset path,
- db path.

Sau khi refresh thanh cong, moi goi `datasetContainsAsofMs()` va `predict_latest.py`.

Can bo sung config cho `CandleSchedulerConfig` de biet danh sach symbols Qlib can refresh. Hien tai `notifyCandleClose()` nhan `symbol` nhung implementation bo qua symbol va scheduler khong co danh sach symbols day du.

### Fix B: Sua datasetContainsAsofMs de doc dung schema

`datasetContainsAsofMs()` nen ho tro it nhat hai format:

1. Legacy CSV co cot dau la epoch milliseconds.
2. Qlib bridge CSV co cot `datetime`.

Voi schema `datetime`, nen parse header de tim index cot `datetime`, convert timestamp sang epoch-ms theo UTC, roi so sanh voi `asofMs`.

Can tranh parser nua voi gia dinh "cot dau la so". `predict_latest.py` da dinh nghia contract moi la dataset co cot `datetime`, nen C++ pre-check phai theo contract nay.

### Fix C: Them test regression cho schema runtime

Them test:

```text
CandleSchedulerThreadTest.DatasetDatetimeColumnMatchesAsof
```

Fixture nen seed:

```text
datetime,symbol,open,high,low,close,volume,factor,quote_volume,trade_count
2026-06-02 01:30:00,BTCUSDT,100,101,99,100,1,1,100,10
```

Va assert `processCandle(1780363800000)` khong bi skip.

Them test missing:

```text
CandleSchedulerThreadTest.DatasetDatetimeColumnMissingAsofSkips
```

### Fix D: Log diagnostic ro hon

Log hien tai thieu dataset path, asof human-readable va max datetime trong dataset. Nen doi thanh dang:

```text
[CANDLE][PHASE3][SKIPPED] asof=1780363800000 asof_utc=2026-06-02T01:30:00Z dataset=data/qlib_smoke/klines_30m.csv dataset_max=2026-05-30T01:00:00Z reason=dataset_missing_asof
```

Neu co diagnostic nay, lan sau co the biet ngay la dataset stale hay parser mismatch.

## Acceptance criteria sau khi fix

- Khi closed candle `2026-06-02 08:30 +07` den, dataset co row tuong ung truoc khi predict.
- Log khong con lap `dataset_missing_asof` cho cung mot asof neu refresh thanh cong.
- `predict_latest.py` duoc goi voi `--asof-ms 1780363800000`.
- SQLite `qlib_predictions` co rows cho `asof_open_time_ms=1780363800000`.
- Ready flag `ready_1780363800000.flag` duoc tao trong `ready_dir`.
- Unit test pass cho ca dataset legacy epoch-ms va dataset runtime `datetime`.

## Phan biet voi cac warning khac

Warning `market_scanner backfill failed ... Network [1]/[995]` va `monitor time-exit snapshot failed ... Network [995]` khong phai nguyen nhan truc tiep cua loi nay. Chung la warning mang/cancel co co che continue. Loi `dataset_missing_asof` den tu Qlib dataset/pre-check cua Phase 3.

Tuy nhien, neu backfill/network fail lam cho nguon refresh latest candle khong lay duoc data, no co the lam Phase 3 tiep tuc missing asof. Vi vay can tach hai muc:

- Nguyen nhan truc tiep hien tai: dataset stale va pre-check sai schema.
- Rui ro phu: refresh qua Binance co the fail neu network/proxy timeout.

## De xuat uu tien

1. Sua `datasetContainsAsofMs()` de doc schema `datetime`.
2. Them LatestCandleRefresher vao Phase 3 truoc prediction.
3. Bo sung tests regression cho schema runtime.
4. Nang log diagnostic de co dataset path, asof UTC/local va dataset max timestamp.

Neu chi cap nhat file CSV thu cong, warning co the het tam thoi cho mot asof, nhung se tai phat o candle tiep theo. Fix triet de phai gom ca refresh tu dong va sua schema check.

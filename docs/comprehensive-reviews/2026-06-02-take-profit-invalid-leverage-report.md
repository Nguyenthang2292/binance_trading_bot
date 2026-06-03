# Bao cao loi take-profit reconcile invalid leverage

Ngay lap bao cao: 2026-06-02  
Pham vi: TakeProfitReconciler / Account position snapshot parsing  
Trang thai: Open

Log can dieu tra:

```text
[2026-06-02 11:11:52.554] [WARNING] take-profit reconcile skipped symbol=NIGHTUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.554] [WARNING] take-profit reconcile skipped symbol=OPENUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.555] [WARNING] take-profit reconcile skipped symbol=ALTUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.557] [WARNING] take-profit reconcile skipped symbol=QNTUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.558] [WARNING] take-profit reconcile skipped symbol=TAOUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.558] [WARNING] take-profit reconcile skipped symbol=DOTUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.558] [WARNING] take-profit reconcile skipped symbol=ACXUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.558] [WARNING] take-profit reconcile skipped symbol=MEMEUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.558] [WARNING] take-profit reconcile skipped symbol=COLLECTUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.558] [WARNING] take-profit reconcile skipped symbol=MAGMAUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.558] [WARNING] take-profit reconcile skipped symbol=YBUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.559] [WARNING] take-profit reconcile skipped symbol=ZILUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.559] [WARNING] take-profit reconcile skipped symbol=STORJUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.559] [WARNING] take-profit reconcile skipped symbol=ORCAUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.559] [WARNING] take-profit reconcile skipped symbol=ASTRUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.559] [WARNING] take-profit reconcile skipped symbol=RIVERUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.559] [WARNING] take-profit reconcile skipped symbol=ROBOUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.559] [WARNING] take-profit reconcile skipped symbol=SPXUSDT reason="invalid leverage <= 0"
[2026-06-02 11:11:52.559] [WARNING] take-profit reconcile skipped symbol=DUSKUSDT reason="invalid leverage <= 0"
```

## Ket luan ngan gon

Day la loi that trong duong reconcile TP, khong phai crash. Bot van chay, nhung `TakeProfitReconciler` bo qua cac position co `leverage <= 0`, nen khong dat moi hoac adopt take-profit cho nhung symbol nay.

Tac dong chinh: neu position dang mo khong co lenh TP hop le, reconciler se khong tu sua. Vi `engine.place_stop_loss=false` trong `config.json`, viec TP reconcile bi vo hieu hoa lam bot phu thuoc vao protection da dat luc mo lenh hoac thao tac thu cong.

## Vi sao warning xuat hien

`TakeProfitReconciler::reconcileOnce()` chi tinh TP distance khi leverage hop le:

```cpp
if (position.leverage <= 0) {
    Logger::instance().log(
        LogLevel::Warning,
        "take-profit reconcile skipped symbol=" + position.symbol +
            " reason=" + quoteString("invalid leverage <= 0"));
    continue;
}
```

Code lien quan:

- `src/engine/take_profit_reconciler.cpp`
  - `livePositionsFromSnapshot()`: lay live positions tu `snapshot.positions` neu co, fallback sang `snapshot.account.positions`.
  - Guard `position.leverage <= 0`: skip reconcile.
  - TP distance: `entryPrice * takeProfitPercent / (100 * leverage)`.
- `src/rest/rest_client.cpp`
  - `parsePosition()`: gan `p.leverage = static_cast<int>(intField(doc, "leverage"))`.
  - `intField()`: hien chi doc JSON integer/double, khong parse numeric string.

Binance USD-M Futures `/fapi/v2/positionRisk` tra `leverage` trong response example dang chuoi, vi du `"leverage": "10"`. Tai lieu tham chieu:

```text
https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/rest-api/Position-Information-V2
```

Neu response live cung tra leverage dang string, `intField(doc, "leverage")` se fallback ve `0`. Ket qua la tat ca live positions trong snapshot co leverage parse thanh 0, khop voi mau log hang loat cung mot timestamp.

## Bang chung trong code

`Position` co default leverage bang 0:

```cpp
struct Position {
    ...
    int leverage{0};
    ...
};
```

Parser hien tai:

```cpp
p.leverage = static_cast<int>(intField(doc, "leverage"));
```

Helper integer hien tai:

```cpp
auto i = value.value().get_int64();
if (!i.error()) {
    return i.value();
}
auto d = value.value().get_double();
if (!d.error()) {
    return static_cast<int64_t>(d.value());
}
return fallback;
```

Khong co nhanh parse string `"10"` thanh integer.

## Muc do nghiem trong

Severity: High

Ly do:

- Khong lam bot crash, nhung lam mat tac dung cua global TP reconciler.
- Xay ra tren nhieu symbol lien tiep, nen co tinh he thong thay vi loi rieng tung position.
- Anh huong truc tiep den risk-control workflow sau khi bot restart, sau khi TP bi huy, hoac sau khi co position live nhung tracker/local state chua day du.

## Cach tai hien ky vong

1. Mock response `/fapi/v2/positionRisk` co live position va `"leverage": "10"`.
2. Goi `RestClient::positions()`.
3. Kiem tra `Position::leverage`.
4. Hien tai ky vong se ra `0`; dung phai la `10`.
5. Chay `TakeProfitReconciler::reconcileOnce()` voi snapshot do.
6. Hien tai se log `invalid leverage <= 0` va `orders.limitCalls == 0`.

## Huong sua de xuat

1. Sua `intField()` de parse numeric string:

```cpp
auto s = value.value().get_string();
if (!s.error()) {
    int64_t parsed = 0;
    const auto text = std::string_view{s.value()};
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec == std::errc{} && ptr == end) {
        return parsed;
    }
}
```

2. Them regression test cho `/positionRisk` leverage dang string.
3. Them test rieng cho `TakeProfitReconciler` voi snapshot parse tu fixture string leverage, de dam bao TP duoc dat thay vi skip.
4. Sau khi fix, chay bot smoke va xac nhan log `invalid leverage <= 0` khong lap lai cho live positions co leverage hop le.

## Ghi chu phan biet

Log sau day la loai khac va khong nam trong bao cao nay:

```text
[2026-06-02 11:11:53.230] [WARNING] exposure blocked symbol=PUMPBTCUSDT reason=gross beta exposure 4725.30 > max 4063.47
```

Diem nay la exposure gate chan mo lenh moi vi vuot gross beta cap. Bao cao hien tai chi ghi nhan loi `take-profit reconcile skipped ... invalid leverage <= 0`.

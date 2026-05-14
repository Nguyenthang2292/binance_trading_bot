# 📊 Đánh giá & thiết kế chiến lược trading crypto

## 1. Ý tưởng chiến lược ban đầu

- Quét ~2000 coin toàn thị trường  
- Mỗi coin có signal → vào lệnh size nhỏ  
- TP cố định  
- Coin nào TP trước thì chốt  
- Sau X thời gian quét lại  
- Trade cả **long & short**

---

## 2. Đánh giá tổng quan

### 👍 Điểm mạnh

- Phân tán rủi ro từng lệnh
- Tận dụng breadth toàn thị trường
- Phù hợp chiến lược short-term

### ⚠️ Rủi ro lớn

#### Correlation risk

2000 coin ≠ 2000 tài sản độc lập

#### TP cố định sai

→ nên dùng ATR

#### Thiếu SL/time-exit

→ cực nguy hiểm

#### Long/Short ≠ neutral

→ cần beta-adjusted

#### Fee + slippage

→ có thể ăn hết lợi nhuận

#### Liquidity

→ phải filter

---

## 3. Pipeline chuẩn

1. Scan market  
2. Filter liquidity  
3. Rank signal  
4. Chọn top N  
5. Size theo volatility  
6. Control exposure  
7. TP/SL ATR  
8. Rebalance  

---

## 4. Xử lý lệnh âm

- Stop loss
- Time exit
- Signal invalidation
- Portfolio stop
- Ranking exit
- Partial cut

❌ Không DCA bừa

---

## 5. Hedge BTC/ETH

### Nguyên tắc

- Hedge theo beta
- Hedge từng phần
- Chỉ hedge khi market xấu
- Hedge có stop riêng
- Ưu tiên đóng alt trước

### Công thức

hedge = alt_exposure × beta × hedge_ratio

---

## 6. Rule mẫu

TP → close  
SL → close  
Time → close  
Signal invalid → close  

Portfolio:

- DD -1% → giảm risk  
- DD -2% → close yếu  
- DD -3% → stop  

Hedge:

- Chỉ khi bearish  
- Size theo beta  
- Có exit  

---

## 7. Kết luận

Không cứu lệnh âm  
→ quản trị toàn portfolio

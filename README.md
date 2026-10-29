<<<<<<< HEAD
# Hệ thống IoT cảnh báo ngập tầng hầm gửi xe

Dự án môn học Ứng dụng IoT: thiết bị giám sát mực nước tự động tại tầng hầm chung cư / bãi xe ngầm, sử dụng vi điều khiển ESP32 và cảm biến siêu âm HC-SR04, truyền dữ liệu thời gian thực và phát cảnh báo đa kênh.

---

## 1. Bài toán và hướng giải quyết

Ngập tầng hầm thường diễn ra rất nhanh khi mưa lớn vượt công suất thoát nước đô thị. Nhược điểm của các hệ thống phao cơ truyền thống là chỉ đóng/ngắt máy bơm cục bộ, dễ kẹt rác và không báo được tình trạng từ xa khi sự cố xảy ra vào ban đêm.

Hệ thống này được thiết kế với tiêu chí quan trọng nhất: **cảnh báo tại chỗ phải độc lập hoàn toàn với Internet**.
- Mọi logic đọc cảm biến, lọc nhiễu, tính tốc độ dâng và kích hoạt còi/đèn đều chạy trực tiếp trên ESP32.
- Dù đứt cáp, mất Wi-Fi hay sập mạng đám mây, còi báo động và đèn LED tại tầng hầm vẫn hoạt động bình thường.
- Hạ tầng Cloudflare và MQTT chỉ đóng vai trò truyền tin từ xa, lưu dữ liệu lịch sử và phục vụ giao diện quản lý.

---

## 2. Các thành phần trong thư mục `src/`

Thư mục `src/` chứa toàn bộ mã nguồn của cả thiết bị nhúng, dịch vụ đám mây và giao diện web:

* **`main.cpp` (Firmware ESP32):**
  - Chạy kiến trúc đa nhiệm (FreeRTOS) tách biệt trên 2 nhân vật lý của ESP32:
    - **Core 1 (Real-time Task):** Bắn xung Trigger 10µs, đọc xung Echo, lấy trung vị 3 mẫu liên tiếp rồi đưa qua bộ đệm trung bình trượt 5 phần tử (Moving Average Filter). So sánh ngưỡng để điều khiển trực tiếp còi chip, 3 đèn LED (Xanh, Vàng, Đỏ) và xuất thông tin ra màn hình LCD 1602 (I2C).
    - **Core 0 (Network Task):** Quản lý kết nối Wi-Fi (đồng thời chạy cả chế độ Station kết nối mạng tòa nhà và Access Point dự phòng), gửi telemetry qua MQTT (TLS port 8883), đẩy log và cảnh báo khẩn cấp lên Cloudflare Worker, đồng bộ ngưỡng cảnh báo từ xa và kiểm tra cập nhật OTA.
  - Tích hợp Watchdog Timer phần cứng (Task WDT) và cơ chế phân vùng kép (Dual Partition OTA) tự động khôi phục firmware cũ nếu bản cập nhật mới bị crash hoặc không check-in thành công trong 36 giây.

* **`worker.js` (Cloudflare Worker API):**
  - Đóng vai trò backend Serverless trung tâm kết nối với cơ sở dữ liệu phân tán **Cloudflare D1**:
    - `POST /alert`: Xác thực thiết bị qua header `X-Device-Key`, kích hoạt gửi tin nhắn cảnh báo đến nhóm Telegram qua Telegram Bot API khi phát hiện chuyển mức nước.
    - `POST /log`: Nhận và ghi lịch sử quan trắc định kỳ / khi có biến động lớn vào bảng D1.
    - `GET /get-settings` & `POST /setting-log`: Cung cấp và cập nhật cấu hình ngưỡng động (Nguy hiểm, Cảnh báo, Phát hiện, Chiều cao lắp đặt) mà không cần nạp lại code cho chip.
    - `POST /ota/*`: Quản lý danh sách firmware, upload file `.bin` lên R2 bucket, kích hoạt phiên bản và ghi nhận trạng thái rollback/check-in của thiết bị.

* **`index.html` (Dashboard giám sát thời gian thực):**
  - Giao diện web trực quan dành cho ban quản lý tòa nhà, kết nối trực tiếp đến HiveMQ Cloud qua MQTT WebSocket để nhận dữ liệu từng giây.
  - Hiển thị khoảng cách tức thời, tốc độ biến thiên nước ($cm/phút$), đồ thị kép (so sánh trực tiếp giá trị thô vs giá trị đã lọc nhiễu) và mô hình cắt lớp hồ chứa mô phỏng mức dâng trực quan.

* **`setting.html` (Trang quản trị & Cấu hình thiết bị):**
  - Giao diện quản lý chuyên sâu: cấu hình ngưỡng báo động từ xa, xem bảng lịch sử đo, xuất dữ liệu và quản lý các bản cập nhật firmware OTA.

---

## 3. Kiến trúc luồng dữ liệu

```
  [ Cảm biến HC-SR04 ]
           │ (Echo / Trig)
           ▼
┌─────────────────────────────────────────────────────────────┐
│                    ESP32 (Dual Core)                        │
│                                                             │
│  [ Core 1: Xử lý tại chỗ ]        [ Core 0: Giao tiếp mạng ]│
│  • Lấy mẫu trung vị 3 lần          • Wi-Fi STA + AP Fallback │
│  • Lọc trung bình trượt 5 giá trị  • MQTT Client (TLS 8883) │
│  • Tính tốc độ dâng (cm/phút)      • HTTP Client / REST API │
│  • Điều khiển Còi + 3 LED + LCD    • Quản lý OTA Rollback   │
└──────────────┬──────────────────────────────────┬───────────┘
               │ (MQTT TLS)                       │ (HTTPS REST)
               ▼                                  ▼
      [ HiveMQ Cloud Broker ]            [ Cloudflare Worker ]
               │ (WSS)                            │
               ▼                     ┌────────────┴────────────┐
       [ index.html ]                ▼                         ▼
    (Dashboard Real-time)     [ Cloudflare D1 ]       [ Telegram Bot API ]
                             (Lịch sử & Cấu hình)     (Báo động khẩn cấp)
```

---

## 4. Yêu cầu cài đặt và vận hành

### Phần cứng
- 1x Vi điều khiển ESP32 DevKit V1 (30 chân).
- 1x Cảm biến siêu âm HC-SR04 (kèm trở chia áp/bảo vệ chân Echo nếu cần).
- 1x Màn hình LCD 1602 kèm module I2C (địa chỉ mặc định `0x27`).
- 3x Đèn LED (Đỏ, Vàng, Xanh) kèm điện trở hạn dòng 330Ω.
- 1x Còi báo active buzzer 5V.

### Nạp Firmware cho ESP32
1. Mở dự án bằng **Visual Studio Code** đã cài tiện ích **PlatformIO IDE**.
2. Tạo file `include/confidential.h` (dựa trên mẫu cấu hình) và điền các thông số:
   ```cpp
   #define WIFI_SSID "Ten_Wifi"
   #define WIFI_PASS "Mat_Khau"
   #define MQTT_HOST "your-broker.hivemq.cloud"
   #define MQTT_PORT 8883
   #define MQTT_USER "your_user"
   #define MQTT_PASS "your_pass"
   #define DEVICE_KEY "ma_khoa_xac_thuc_thiet_bi"
   ```
3. Cắm cáp USB kết nối ESP32 với máy tính.
4. Bấm **PlatformIO: Build** và **PlatformIO: Upload** để nạp firmware. Mở **Serial Monitor** ở baud rate `115200` để theo dõi tiến trình khởi động.

### Triển khai Backend Cloudflare
1. Đăng ký tài khoản Cloudflare và cài đặt công cụ CLI `wrangler`.
2. Tạo cơ sở dữ liệu D1 và khởi tạo cấu trúc bảng (schema) cho bảng log và cấu hình.
3. Deploy file `src/worker.js` lên Cloudflare Workers:
   ```bash
   npx wrangler deploy
   ```
4. Thiết lập các biến môi trường (`DEVICE_KEY`, `TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID`) trong phần cài đặt của Worker.

---

## 5. Điểm lưu ý khi thử nghiệm

- **Đặc thù cảm biến siêu âm:** Cảm biến đo khoảng cách từ trần/vị trí treo xuống mặt nước. Do đó, **khoảng cách đo được tỉ lệ nghịch với mực nước** (khoảng cách càng nhỏ tức nước dâng càng cao).
- **Bộ lọc tín hiệu:** Khi thay đổi mực nước đột ngột, bộ lọc trung bình trượt 5 phần tử cần khoảng 3 đến 5 chu kỳ lấy mẫu để giá trị sau lọc bắt kịp hoàn toàn, giúp loại bỏ các tia phản xạ nhiễu hoặc gợn sóng bề mặt.
- **Bảo mật:** Không commit file `include/confidential.h` lên các kho mã nguồn công khai.
=======
nothing
>>>>>>> 34c5f76a94dcf2f4d68951bf1e4d91a56854b60a

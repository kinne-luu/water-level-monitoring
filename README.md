# Water Level Monitoring (Hệ thống giám sát ngập lụt tầng hầm)

Mô hình hệ thống IoT cảnh báo ngập lụt tự động cho tầng hầm gửi xe, kết hợp xử lý biên trên ESP32 và truyền dữ liệu thời gian thực qua nền tảng Cloudflare Serverless & MQTT HiveMQ.

---

## Tính năng chính

- **Đo khoảng cách không tiếp xúc:** Sử dụng cảm biến siêu âm HC-SR04, chu kỳ lấy mẫu 1 giây. Dữ liệu thô được lấy trung vị 3 lần đo liên tiếp và lọc nhiễu bằng bộ đệm trung bình trượt 5 mẫu.
- **Xử lý độc lập tại chỗ:** Còi chip, cụm đèn 3 màu (Xanh / Vàng / Đỏ) và màn hình LCD 1602 (I2C) được điều khiển trực tiếp từ ESP32, đảm bảo cảnh báo tức thời ngay cả khi mất kết nối Internet.
- **Tính toán tốc độ biến thiên:** Tính tốc độ nước dâng theo $cm/phút$ theo chu kỳ 5 giây để đánh giá nguy cơ và ước lượng thời gian chạm ngưỡng báo động.
- **Giám sát thời gian thực:** Truyền dữ liệu telemetry lên broker HiveMQ Cloud qua giao thức MQTT bảo mật TLS (port 8883), cập nhật trực tiếp lên dashboard giao diện web.
- **Cảnh báo khẩn cấp từ xa:** Tự động gửi thông báo chuyển mức nguy hiểm vào nhóm chat Telegram thông qua Cloudflare Worker và Telegram Bot API.
- **Cấu hình & Đồng bộ ngưỡng động:** Cho phép quản trị viên thay đổi các ngưỡng cảnh báo từ xa trên web mà không cần nạp lại firmware.
- **Cập nhật Firmware qua mạng (OTA):** Cơ chế phân vùng kép (Dual Partition) cho phép kiểm tra, tải bản cập nhật `.bin` từ xa và tự động quay về bản cũ (Rollback) nếu firmware mới bị lỗi/treo hệ thống.

---

## Kiến trúc hệ thống

Dự án được phân bổ thành 3 lớp độc lập:

1. **Lớp cảm biến và xử lý tại chỗ:** 
   - Vi điều khiển ESP32 chạy FreeRTOS chia 2 nhân vật lý:
     - `Core 1`: Xử lý thời gian thực (đọc cảm biến, lọc nhiễu, so sánh ngưỡng, kích hoạt còi/đèn/LCD).
     - `Core 0`: Xử lý tác vụ mạng (duy trì kết nối Wi-Fi, publish MQTT, gửi HTTP request, OTA).
2. **Lớp mạng & trung gian:**
   - Wi-Fi kép: Chế độ Station (STA) kết nối mạng tòa nhà + Access Point (AP) cục bộ dùng để kỹ thuật viên cấu hình khẩn cấp.
   - Broker MQTT HiveMQ Cloud trung chuyển dữ liệu telemetry tới trình duyệt.
3. **Lớp đám mây & Ứng dụng:**
   - Cloudflare Workers xử lý logic backend không máy chủ.
   - Cloudflare D1 (Serverless SQL) lưu trữ lịch sử quan trắc và nhật ký cấu hình.
   - Telegram Bot API tiếp nhận và phân phối tin nhắn cảnh báo khẩn cấp.

---

## Cấu trúc thư mục mã nguồn

```text
├── include/
│   └── confidential.h      # Khai báo cấu hình nhạy cảm (Wi-Fi, MQTT, Key) - không commit
├── src/
│   ├── main.cpp            # Firmware chính cho ESP32 (xử lý cảm biến, 2 luồng, MQTT, OTA)
│   ├── worker.js           # Backend API chạy trên Cloudflare Workers (kết nối D1 & Telegram)
│   ├── index.html          # Web dashboard theo dõi mực nước thời gian thực (MQTT WebSocket)
│   ├── setting.html        # Trang quản trị cấu hình ngưỡng, xem log lịch sử và quản lý OTA
│   └── settings.json       # File cấu hình phụ trợ
├── platformio.ini          # Cấu hình môi trường biên dịch PlatformIO
└── README.md
```

---

## Sơ đồ kết nối phần cứng

| Linh kiện | Chân linh kiện | Chân GPIO trên ESP32 | Ghi chú |
| :--- | :--- | :--- | :--- |
| **HC-SR04** | TRIG | GPIO 5 | Ngõ ra kích hoạt xung phát siêu âm (10µs) |
| | ECHO | GPIO 18 | Ngõ vào đo thời gian sóng phản xạ |
| **LCD 1602 (I2C)** | SDA | GPIO 21 (SDA) | Đường truyền dữ liệu I2C |
| | SCL | GPIO 22 (SCL) | Xung nhịp đồng bộ I2C (địa chỉ mặc định `0x27`) |
| **LED Xanh** | Anode (+) | GPIO 25 | Mức an toàn (nối tiếp trở hạn dòng 330Ω) |
| **LED Vàng** | Anode (+) | GPIO 26 | Mức chú ý / phát hiện nước |
| **LED Đỏ** | Anode (+) | GPIO 27 | Mức nguy hiểm |
| **Còi báo (Buzzer)**| Cực dương (+) | GPIO 19 | Còi chip 5V cảnh báo mức nguy hiểm |

---

## Hướng dẫn cài đặt & Triển khai

### 1. Chuẩn bị môi trường nạp firmware
- Cài đặt [Visual Studio Code](https://code.visualstudio.com/) và tiện ích mở rộng **PlatformIO IDE**.
- Tạo file `include/confidential.h` với nội dung mẫu:
  ```cpp
  #ifndef CONFIDENTIAL_H
  #define CONFIDENTIAL_H

  #define WIFI_SSID "Ten_Wifi_Cua_Ban"
  #define WIFI_PASS "Mat_Khau_Wifi"

  #define MQTT_HOST "your-cluster.hivemq.cloud"
  #define MQTT_PORT 8883
  #define MQTT_USER "your_mqtt_username"
  #define MQTT_PASS "your_mqtt_password"

  #define DEVICE_KEY "khoa_bi_mat_dong_bo_voi_worker"

  #endif
  ```
- Kết nối ESP32 với máy tính qua cổng USB, mở tab PlatformIO và chọn **Upload** để nạp chương trình.

### 2. Triển khai backend Cloudflare Worker
- Yêu cầu môi trường đã cài sẵn [Node.js](https://nodejs.org/) và công cụ `wrangler`.
- Đăng nhập tài khoản Cloudflare:
  ```bash
  npx wrangler login
  ```
- Khởi tạo cơ sở dữ liệu Cloudflare D1 và liên kết vào file `wrangler.toml`:
  ```bash
  npx wrangler d1 create water-level-db
  ```
- Triển khai Worker:
  ```bash
  npx wrangler deploy src/worker.js
  ```
- Thiết lập các Secret trên Cloudflare Dashboard hoặc qua CLI:
  - `DEVICE_KEY`: Khóa xác thực khớp với file `confidential.h`.
  - `TELEGRAM_BOT_TOKEN`: Token bot lấy từ BotFather.
  - `TELEGRAM_CHAT_ID`: ID nhóm chat nhận thông báo.

### 3. Vận hành giao diện Web
- File `src/index.html` và `src/setting.html` có thể host trực tiếp trên **Cloudflare Pages**, GitHub Pages hoặc chạy local bằng tiện ích Live Server trong VS Code để theo dõi và tinh chỉnh hệ thống.

---

## Lưu ý kỹ thuật

1. **Nguyên lý đo khoảng cách:** Cảm biến siêu âm được treo trên cao chiếu xuống mặt sàn. Vì vậy giá trị khoảng cách đo được **nghịch biến** với chiều cao mực nước (khoảng cách càng giảm tức nước đang dâng càng cao).
2. **Độ trễ bộ lọc:** Bộ lọc trung bình trượt 5 phần tử giúp loại bỏ các tia phản xạ nhiễu bề mặt gợn sóng, nhưng sẽ tạo độ trễ khoảng 3 - 5 giây để bắt kịp giá trị khi mực nước thay đổi dạng bước nhảy (step change).
3. **An toàn thông tin:** Tuyệt đối không commit các khóa bí mật trong `confidential.h` lên GitHub.

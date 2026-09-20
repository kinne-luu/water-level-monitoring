# Xây dựng mô hình hệ thống IoT giám sát ngập lụt trong tầng hầm gửi xe

**Báo cáo kỹ thuật — Ứng dụng IoT**

## Giới thiệu đề tài

Kho mã nguồn này là phần hiện thực hóa (firmware + giao diện giám sát) của đề tài báo cáo kỹ thuật ứng dụng IoT: xây dựng một hệ thống giám sát mực nước tại tầng hầm gửi xe, nhằm phát hiện sớm nguy cơ ngập úng và tự động cảnh báo tới ban quản lý cùng cư dân trước khi thiệt hại xảy ra.

Hệ thống được thiết kế theo kiến trúc IoT ba lớp:

1. **Lớp cảm biến và xử lý tại chỗ** — vi điều khiển ESP32 đọc dữ liệu từ cảm biến siêu âm HC-SR04, lọc nhiễu, tính tốc độ dâng nước và tự ra quyết định cảnh báo (còi, đèn LED, LCD) ngay tại thiết bị, không phụ thuộc kết nối mạng.
2. **Lớp mạng** — thiết bị chạy song song vai trò trạm khách Wi-Fi (kết nối lên đám mây) và điểm phát sóng cục bộ (phục vụ cấu hình kỹ thuật), truyền dữ liệu thời gian thực qua MQTT.
3. **Lớp nền tảng đám mây không máy chủ** — Cloudflare Workers cùng cơ sở dữ liệu D1 đảm nhận lưu trữ lịch sử đo, phân phối cấu hình ngưỡng, cập nhật firmware từ xa (OTA) và chuyển tiếp cảnh báo qua Telegram; HiveMQ Cloud đóng vai trò broker MQTT cho luồng dữ liệu thời gian thực.

Phần mã nguồn trong repo này tương ứng với các nội dung đã trình bày ở Chương 3 (Phân tích và thiết kế hệ thống) của báo cáo, gồm firmware nạp cho ESP32 và giao diện web giám sát/cấu hình đi kèm.

## Công nghệ sử dụng

| Thành phần | Công nghệ |
|---|---|
| Vi điều khiển trung tâm | ESP32 (hai nhân, xử lý song song cảm biến và mạng) |
| Cảm biến đo mực nước | HC-SR04 (siêu âm, không tiếp xúc) |
| Môi trường phát triển firmware | PlatformIO |
| Giao thức truyền dữ liệu thời gian thực | MQTT qua TLS (broker HiveMQ Cloud) |
| Nền tảng đám mây xử lý trung tâm | Cloudflare Workers + Cloudflare D1 |
| Kênh gửi cảnh báo | Telegram Bot API |
| Giao diện giám sát/cấu hình | HTML/CSS/JavaScript thuần |

## Cấu trúc mã nguồn

```
water-level-monitoring/
├── include/
│   └── secrets.h        # Thông tin cấu hình mạng (không commit lên Git)
├── lib/                  # Thư viện phụ thuộc dùng riêng cho dự án
├── src/
│   ├── main.cpp          # Firmware điều khiển chính (C++)
│   ├── index.html        # Giao diện giám sát thời gian thực
│   ├── setting.html       # Giao diện cấu hình thiết bị
│   └── settings.json     # Cấu hình cổng máy chủ cục bộ (mặc định: 5501)
├── test/                  # Kịch bản kiểm thử mã nguồn
├── platformio.ini        # Cấu hình bo mạch, framework và môi trường biên dịch
└── .gitignore             # Loại trừ .pio/, .vscode/, *.hex khỏi Git
```

| Thư mục / Tệp | Vai trò |
|---|---|
| `include/` | Chứa các tệp header, trong đó `secrets.h` khai báo thông tin nhạy cảm (tên mạng Wi-Fi, mật khẩu, khóa xác thực...) |
| `lib/` | Chứa thư viện phụ thuộc riêng của dự án (nếu có), tách biệt với thư viện quản lý bởi PlatformIO |
| `src/` | Firmware (`main.cpp`) và giao diện web đi kèm (`index.html`, `setting.html`, `settings.json`) |
| `test/` | Các kịch bản kiểm thử mã nguồn |
| `platformio.ini` | Định nghĩa board, framework, tốc độ upload và thư viện cần cài |
| `.gitignore` | Loại bỏ thư mục build tạm (`.pio`, `.vscode`) và tệp biên dịch (`.hex`) khỏi Git |

## Yêu cầu môi trường

- IDE hỗ trợ **PlatformIO** (khuyến nghị VS Code + extension PlatformIO IDE).
- Cáp nạp và driver USB tương ứng với board ESP32 đang sử dụng.
- Tiện ích **Live Server** (hoặc công cụ tương đương) để phục vụ các tệp HTML tĩnh tại cổng `5501`.

## Hướng dẫn triển khai

1. Mở thư mục dự án bằng IDE có tích hợp PlatformIO.
2. Điền các thông số cấu hình mạng cần thiết (SSID, mật khẩu Wi-Fi, thông tin broker MQTT...) vào `include/secrets.h`. **Không commit tệp này lên Git.**
3. Để PlatformIO đọc `platformio.ini`, tự động tải board package và thư viện cần thiết.
4. Biên dịch và nạp `src/main.cpp` vào thiết bị ESP32 qua PlatformIO (`Build` → `Upload`).
5. Khởi chạy Live Server tại cổng `5501` để phục vụ `src/index.html` (giám sát) và `src/setting.html` (cấu hình), theo dõi và tùy chỉnh hệ thống ngay trên trình duyệt.

## Lưu ý bảo mật

`include/secrets.h` chứa thông tin nhạy cảm và nên được thêm vào `.gitignore` để tránh vô tình đưa lên kho mã nguồn công khai.

## Tài liệu liên quan

Phần cơ sở lý thuyết, khảo sát công nghệ, phân tích yêu cầu và thiết kế chi tiết được trình bày đầy đủ trong báo cáo kỹ thuật đi kèm đề tài.

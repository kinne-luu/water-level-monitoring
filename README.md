# Dự án Giám sát Mực nước (Water Level Monitoring)

## Giới thiệu chung

Dự án cung cấp mã nguồn firmware cho thiết bị IoT dùng để đo và giám sát mực nước theo thời gian thực, chạy trên vi điều khiển ESP32. Hệ thống được xây dựng và quản lý bằng môi trường phát triển **PlatformIO**, đồng thời tích hợp sẵn giao diện web nội bộ giúp người dùng theo dõi trạng thái đo đạc và tùy chỉnh thông số vận hành trực tiếp trên trình duyệt.

## Cấu trúc mã nguồn

```
water-level-monitoring/
├── include/
│   └── secrets.h        # Thông tin cấu hình mạng (không commit lên Git)
├── lib/                  # Thư viện phụ thuộc dùng riêng cho dự án
├── src/
│   ├── main.cpp          # Logic điều khiển chính (C++)
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
| `src/` | Thư mục cốt lõi — chứa firmware (`main.cpp`) và giao diện web đi kèm (`index.html`, `setting.html`, `settings.json`) |
| `test/` | Chứa các kịch bản kiểm thử mã nguồn |
| `platformio.ini` | Tệp cấu hình trung tâm của PlatformIO: định nghĩa board, framework, tốc độ upload và các thư viện cần cài |
| `.gitignore` | Loại bỏ thư mục build tạm (`.pio`, `.vscode`) và tệp biên dịch (`.hex`) khỏi hệ thống quản lý mã nguồn |

## Yêu cầu môi trường

- IDE hỗ trợ **PlatformIO** (khuyến nghị VS Code + extension PlatformIO IDE).
- Cáp nạp và driver USB tương ứng với board ESP32 đang sử dụng.
- Tiện ích **Live Server** (hoặc công cụ tương đương) để phục vụ các tệp HTML tĩnh tại cổng `5501`.

## Hướng dẫn triển khai

1. Mở thư mục dự án `water-level-monitoring` bằng IDE có tích hợp PlatformIO.
2. Sao chép nội dung mẫu (nếu có) thành `include/secrets.h` và điền các thông số cấu hình mạng cần thiết (SSID, mật khẩu Wi-Fi...). **Không commit tệp này lên Git.**
3. Để PlatformIO đọc `platformio.ini`, tự động tải về board package và thư viện cần thiết.
4. Biên dịch và nạp `src/main.cpp` vào thiết bị ESP32 qua PlatformIO (`Build` → `Upload`).
5. Khởi chạy Live Server tại cổng `5501` để phục vụ `src/index.html` (giám sát) và `src/setting.html` (cấu hình), theo dõi và tùy chỉnh hệ thống ngay trên trình duyệt.

## Lưu ý bảo mật

`include/secrets.h` chứa thông tin nhạy cảm và nên được thêm vào `.gitignore` để tránh vô tình đưa lên kho mã nguồn công khai.

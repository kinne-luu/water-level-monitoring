# Hệ thống IoT giám sát ngập lụt tầng hầm gửi xe

Đây là mã nguồn của đề tài báo cáo kỹ thuật môn Ứng dụng IoT. Hệ thống đo mực nước trong tầng hầm bằng cảm biến siêu âm, cảnh báo ngay tại chỗ bằng đèn, còi và màn hình LCD, đồng thời gửi dữ liệu lên đám mây để ban quản lý theo dõi và nhận tin nhắn khi có nguy hiểm.

Điều quan trọng nhất trong thiết kế là thiết bị phải tự cảnh báo được ngay cả khi mất mạng. Mưa lớn thường làm đứt Wi-Fi đúng lúc nước dâng nhanh nhất, nên mọi phép tính liên quan đến an toàn đều chạy trên ESP32, còn đám mây chỉ lo việc lưu lịch sử và báo tin từ xa.

## Hệ thống làm được gì

1. Đo khoảng cách từ cảm biến đến mặt nước mỗi giây một lần, lấy trung vị của ba lần đo rồi đưa qua bộ lọc trung bình trượt năm giá trị để loại nhiễu.
2. Chia mực nước thành bốn mức: an toàn, phát hiện nước, trung bình và nguy hiểm. Mỗi mức tương ứng một trạng thái đèn LED, còi và nội dung trên LCD.
3. Tính tốc độ biến thiên của mực nước theo cm mỗi phút để người trực biết nước đang dâng hay đang rút.
4. Gửi dữ liệu thời gian thực qua MQTT lên giao diện web, lưu lịch sử đo vào cơ sở dữ liệu và gửi cảnh báo qua Telegram.
5. Cho chỉnh ngưỡng cảnh báo từ xa trên giao diện web mà không cần nạp lại firmware.
6. Cập nhật firmware qua mạng (OTA), tự quay về bản cũ nếu bản mới gặp lỗi.
7. Báo lỗi riêng khi cảm biến không phản hồi, để không nhầm sự cố phần cứng với tình huống ngập thật.

## Kiến trúc

Hệ thống chia làm ba lớp.

| Lớp | Thành phần | Nhiệm vụ |
|---|---|---|
| Cảm biến và xử lý tại chỗ | ESP32, HC-SR04, LCD, LED, còi | Đo, lọc nhiễu, phân loại mức và cảnh báo tại chỗ |
| Mạng | Wi-Fi (trạm khách và điểm phát sóng), MQTT qua TLS | Truyền dữ liệu lên đám mây, cho phép cấu hình trực tiếp khi mất mạng nội bộ |
| Đám mây | HiveMQ Cloud, Cloudflare Workers, Cloudflare D1, Telegram Bot API | Phân phối dữ liệu thời gian thực, lưu lịch sử, cấp ngưỡng và firmware, chuyển cảnh báo |

Bên trong ESP32, hai nhân chạy hai việc riêng. Một nhân lo đo, lọc và điều khiển còi đèn. Nhân còn lại lo mọi việc cần mạng. Nhờ vậy khi đường truyền chậm hoặc đứt, việc cảnh báo tại chỗ vẫn chạy đều.

## Công nghệ sử dụng

| Thành phần | Công nghệ |
|---|---|
| Vi điều khiển | ESP32 |
| Cảm biến | HC-SR04 (siêu âm, không tiếp xúc với nước) |
| Môi trường phát triển firmware | PlatformIO |
| Truyền dữ liệu thời gian thực | MQTT qua TLS, broker HiveMQ Cloud |
| Xử lý và lưu trữ trên đám mây | Cloudflare Workers, Cloudflare D1 |
| Kênh cảnh báo | Telegram Bot API |
| Giao diện web | HTML, CSS và JavaScript thuần |

## Cấu trúc thư mục

```
water-level-monitoring/
├── include/
│   └── secrets.h
├── lib/
├── src/
│   ├── main.cpp
│   ├── index.html
│   ├── setting.html
│   └── settings.json
├── platformio.ini
└── .gitignore
```

| Thư mục hoặc tệp | Nội dung |
|---|---|
| `include/secrets.h` | Thông tin nhạy cảm như tên và mật khẩu Wi-Fi, thông tin broker MQTT. Tệp này không được đưa lên Git |
| `lib/` | Thư viện riêng của dự án, tách khỏi thư viện do PlatformIO quản lý |
| `src/main.cpp` | Firmware chính cho ESP32 |
| `src/index.html` | Trang giám sát mực nước theo thời gian thực |
| `src/setting.html` | Trang cấu hình thiết bị và ngưỡng cảnh báo |
| `src/settings.json` | Cấu hình cổng của máy chủ cục bộ, mặc định là 5501 |
| `platformio.ini` | Khai báo board, framework, tốc độ nạp và thư viện cần dùng |
| `.gitignore` | Loại `.pio/`, `.vscode/` và các tệp `.hex` khỏi Git |

## Chuẩn bị

Bạn cần có VS Code cùng extension PlatformIO IDE, cáp USB và driver phù hợp với board ESP32 đang dùng, và tiện ích Live Server (hoặc công cụ tương đương) để chạy các trang HTML ở cổng 5501.

## Cài đặt và chạy

1. Mở thư mục dự án bằng VS Code có cài PlatformIO. PlatformIO sẽ đọc `platformio.ini` và tự tải board package cùng các thư viện cần thiết.
2. Tạo hoặc mở `include/secrets.h`, điền tên Wi-Fi, mật khẩu và thông tin broker MQTT.
3. Cắm ESP32 vào máy, chọn Build rồi Upload để nạp `src/main.cpp`.
4. Bật Live Server ở cổng 5501, sau đó mở `src/index.html` để theo dõi mực nước và `src/setting.html` để chỉnh cấu hình.

## Bảo mật

`include/secrets.h` chứa mật khẩu và khóa xác thực, vì vậy hãy chắc chắn nó đã nằm trong `.gitignore` trước khi đẩy mã lên kho công khai.

## Tài liệu đi kèm

Cơ sở lý thuyết, so sánh công nghệ, phân tích yêu cầu và thiết kế chi tiết của hệ thống nằm trong báo cáo kỹ thuật của đề tài. Mã nguồn trong kho này tương ứng với Chương 3 của báo cáo.
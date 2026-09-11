# Dự án Giám sát Mực nước (Water Level Monitoring) [source: 1]

## Giới thiệu chung
Dự án này cung cấp mã nguồn cho hệ thống thiết bị IoT dùng để giám sát mực nước [source: 1]. Hệ thống được xây dựng và quản lý cấu hình thông qua môi trường phát triển PlatformIO [source: 1]. Mã nguồn tích hợp sẵn giao diện web nội bộ giúp người dùng trực tiếp theo dõi trạng thái và tùy chỉnh thông số [source: 1].

## Cấu trúc mã nguồn
Toàn bộ hệ thống được phân chia thành các thư mục và tệp tin tiêu chuẩn [source: 1]:
* **include/**: Thư mục lưu trữ các tệp tiêu đề (header files), trong đó có tệp `secrets.h` dùng để khai báo các thông tin bảo mật cấu hình mạng [source: 1].
* **lib/**: Khu vực chứa các tài liệu hoặc thư viện phụ thuộc phục vụ cho quá trình hoạt động [source: 1].
* **src/**: Thư mục cốt lõi chứa logic hoạt động chính [source: 1]. Bao gồm tệp lập trình C++ `main.cpp` và các tệp giao diện web như `index.html`, `setting.html` [source: 1]. Đồng thời chứa tệp `settings.json` định nghĩa cổng máy chủ cục bộ là 5501 [source: 1].
* **test/**: Nơi chứa các tệp kịch bản kiểm thử mã nguồn [source: 1].
* **platformio.ini**: Tệp tin trọng tâm định nghĩa cấu hình nền tảng, bo mạch và môi trường biên dịch của dự án PlatformIO [source: 1].
* **.gitignore**: Tệp tin khai báo loại bỏ các thư mục tạm như `.pio`, `.vscode` và các tệp `.hex` khỏi hệ thống quản lý mã nguồn [source: 1].

## Yêu cầu môi trường
* Môi trường phát triển tích hợp (IDE) có hỗ trợ phần mềm PlatformIO [source: 1].
* Tiện ích Live Server hoặc công cụ tương đương chạy tại cổng 5501 để phục vụ giao diện HTML [source: 1].

## Hướng dẫn triển khai
1. Mở thư mục dự án `water-level-monitoring-main` trên phần mềm chỉnh sửa mã có tích hợp PlatformIO [source: 1].
2. Bổ sung các thông số cấu hình mạng cần thiết vào tệp `include/secrets.h` [source: 1].
3. Sử dụng công cụ PlatformIO đọc tệp `platformio.ini` để thiết lập môi trường và thư viện [source: 1].
4. Tiến hành biên dịch tệp `src/main.cpp` và nạp mã vào thiết bị vi điều khiển [source: 1].
5. Khởi chạy máy chủ nội bộ cho các tệp `src/index.html` và `src/setting.html` để theo dõi và cài đặt dữ liệu giám sát mực nước [source: 1].

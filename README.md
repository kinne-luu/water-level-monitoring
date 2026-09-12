# Dự án Giám sát Mực nước (Water Level Monitoring) 

## Giới thiệu chung
Dự án này cung cấp mã nguồn cho hệ thống thiết bị IoT dùng để giám sát mực nước . Hệ thống được xây dựng và quản lý cấu hình thông qua môi trường phát triển PlatformIO . Mã nguồn tích hợp sẵn giao diện web nội bộ giúp người dùng trực tiếp theo dõi trạng thái và tùy chỉnh thông số .

## Cấu trúc mã nguồn
Toàn bộ hệ thống được phân chia thành các thư mục và tệp tin tiêu chuẩn :
* **include/**: Thư mục lưu trữ các tệp tiêu đề (header files), trong đó có tệp `secrets.h` dùng để khai báo các thông tin bảo mật cấu hình mạng .
* **lib/**: Khu vực chứa các tài liệu hoặc thư viện phụ thuộc phục vụ cho quá trình hoạt động .
* **src/**: Thư mục cốt lõi chứa logic hoạt động chính . Bao gồm tệp lập trình C++ `main.cpp` và các tệp giao diện web như `index.html`, `setting.html` . Đồng thời chứa tệp `settings.json` định nghĩa cổng máy chủ cục bộ là 5501 .
* **test/**: Nơi chứa các tệp kịch bản kiểm thử mã nguồn .
* **platformio.ini**: Tệp tin trọng tâm định nghĩa cấu hình nền tảng, bo mạch và môi trường biên dịch của dự án PlatformIO .
* **.gitignore**: Tệp tin khai báo loại bỏ các thư mục tạm như `.pio`, `.vscode` và các tệp `.hex` khỏi hệ thống quản lý mã nguồn .

## Yêu cầu môi trường
* Môi trường phát triển tích hợp (IDE) có hỗ trợ phần mềm PlatformIO .
* Tiện ích Live Server hoặc công cụ tương đương chạy tại cổng 5501 để phục vụ giao diện HTML .

## Hướng dẫn triển khai
1. Mở thư mục dự án `water-level-monitoring-main` trên phần mềm chỉnh sửa mã có tích hợp PlatformIO .
2. Bổ sung các thông số cấu hình mạng cần thiết vào tệp `include/secrets.h` .
3. Sử dụng công cụ PlatformIO đọc tệp `platformio.ini` để thiết lập môi trường và thư viện .
4. Tiến hành biên dịch tệp `src/main.cpp` và nạp mã vào thiết bị vi điều khiển .
5. Khởi chạy máy chủ nội bộ cho các tệp `src/index.html` và `src/setting.html` để theo dõi và cài đặt dữ liệu giám sát mực nước .

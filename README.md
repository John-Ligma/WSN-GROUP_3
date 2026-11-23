# EE4552 – Multi-Hop Cluster-Tree WSN Firmware (SV2)

Firmware cho hệ thống mạng cảm biến không dây **Cluster-Tree 2-Hop** giám sát Nhiệt độ & Độ ẩm trong tòa nhà (ĐHBKHN – EE4552, Nhóm 3).

## 1. Phần cứng (BOM v2)
| Node ID | Vai trò | Bo mạch | Cảm biến & Chân nối |
|---|---|---|---|
| `0x00` | Gateway (Sink) | ESP32-WROOM-32 / 32U | USB-UART CP2102/CH340 (115200 bps) |
| `0x01` | Node 1 – Remote Leaf (2-hop) | ESP32-C3 Super Mini | SHT30 I2C (`GPIO4` SDA, `GPIO5` SCL, `0x44`) |
| `0x02` | Node 2 – Sensor + Relay (Always-On RX) | ESP32-C3 Super Mini | SHT30 I2C (`GPIO4` SDA, `GPIO5` SCL, `0x44`) |
| `0x03` | Node 3 – Local Leaf (1-hop đối chứng) | ESP32-C3 Super Mini | SHT30 I2C (`GPIO4` SDA, `GPIO5` SCL, `0x44`) |

## 2. Cài đặt môi trường làm việc
1. Mở thư mục `PROJECT_WSN` bằng **VS Code**.
2. Cài Extension **PlatformIO IDE** trong VS Code Extensions (`Ctrl+Shift+X` -> tìm `PlatformIO IDE`).
3. Cài driver USB-UART **CP210x** hoặc **CH340** cho bo Gateway ESP32-WROOM-32 (3 bo ESP32-C3 Super Mini dùng Native USB CDC nên Windows 10/11 tự nhận).
4. (Khuyến nghị) Cài **Tera Term** hoặc dùng luôn Serial Monitor của PlatformIO để ghi log `.csv` vào thư mục `docs_logs/`.

## 3. Quy trình nạp code ngày mai khi có mạch thật
> **Lưu ý an toàn**: Luôn gạt công tắc nguồn pin `SW1` sang **OFF** trước khi cắm cáp USB nạp code cho 3 Node ESP32-C3.

### Bước 1: Đọc địa chỉ MAC của 4 bo mạch
- Cắm lần lượt từng bo ESP32-C3, nạp env `mac_reader_c3` và mở Serial Monitor (`115200`):
  ```bash
  pio run -e mac_reader_c3 -t upload -t monitor
  ```
- Cắm bo Gateway ESP32-WROOM-32, nạp env `mac_reader_gw`:
  ```bash
  pio run -e mac_reader_gw -t upload -t monitor
  ```
- Dán nhãn vật lý `GW`, `N1`, `N2`, `N3` lên 4 bo và cập nhật 4 mảng `MAC_GATEWAY`, `MAC_NODE1`, `MAC_NODE2`, `MAC_NODE3` trong `include/wsn_protocol.h`.

### Bước 2: Nạp firmware cho từng vai trò
```bash
pio run -e gateway -t upload
pio run -e node3_leaf -t upload
pio run -e node2_relay -t upload
pio run -e node1_leaf -t upload
```
*(Nếu chưa hàn SHT30 mà muốn test sóng RF trước, bỏ comment `#define SIMULATE_SENSOR` trong `include/config.h`.)*

## 4. Chuẩn bản tin & Serial CSV (20 Bytes – Hướng B)
- Gói tin `WsnPacket` cố định **20 bytes** (thêm `uint16_t last_rtt_ms` piggyback RTT đo trên chính đồng hồ nội nút gửi của gói `N-1`).
- Gateway xuất Serial `115200 bps` cho Dashboard SV3:
  ```text
  DATA,<src_id>,<via_id>,<seq_num>,<hop_count>,<temp_c>,<humi_rh>,<rssi_dbm>,<rtt_ms>
  ALERT,NODE_OFFLINE,<node_id>
  ```

## 5. Phân tích log thực nghiệm (T2, T3, T4, T5, T6)
```bash
python scripts/log_analyzer.py docs_logs/ten_file_log.csv
```

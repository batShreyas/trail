import socket
import struct
import cv2
import numpy as np

SERVER_IP = '192.168.1.10'  # Change to your FPGA/lwIP server IP
SERVER_PORT = 6001

def run_png_video_client():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((SERVER_IP, SERVER_PORT))
    print("Connected to lwIP server.")

    cap = cv2.VideoCapture(0)  # Use webcam. Replace with file path for video

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                print("No more frames or camera error.")
                break

            # Encode frame to PNG (lossless)
            ret, buffer = cv2.imencode('.png', frame)
            if not ret:
                print("Failed to encode frame.")
                continue

            data = buffer.tobytes()
            size = len(data)

            # 1. Send 4-byte big-endian size
            sock.sendall(struct.pack('>I', size))

            # 2. Send PNG data
            sock.sendall(data)

            # 3. Receive echoed data
            received_data = b''
            while len(received_data) < size:
                chunk = sock.recv(size - len(received_data))
                if not chunk:
                    raise ConnectionError("lwIP server closed the connection")
                received_data += chunk

            # 4. Decode echoed PNG and display
            echoed_frame = cv2.imdecode(np.frombuffer(received_data, dtype=np.uint8), cv2.IMREAD_COLOR)
            if echoed_frame is not None:
                cv2.imshow("Echoed PNG Frame", echoed_frame)
            else:
                print("Failed to decode echoed PNG.")

            if cv2.waitKey(1) == 27:  # Press ESC to exit
                break

    except Exception as e:
        print(f"Error: {e}")
    finally:
        cap.release()
        sock.close()
        cv2.destroyAllWindows()
        print("Disconnected.")

if __name__ == "__main__":
    run_png_video_client()

import socket
import struct
import cv2
import numpy as np

SERVER_IP = '192.168.1.10'  # Replace with your lwIP server IP
SERVER_PORT = 6001

def run_mjpeg_client():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((SERVER_IP, SERVER_PORT))
    print("Connected to lwIP server.")

    cap = cv2.VideoCapture(0)  # Use webcam; replace 0 with file path if needed

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                print("Failed to capture frame.")
                break

            # Encode to JPEG (lower quality = smaller size, faster)
            encode_param = [int(cv2.IMWRITE_JPEG_QUALITY), 60]  # You can adjust quality
            ret, buffer = cv2.imencode('.jpg', frame, encode_param)
            if not ret:
                print("JPEG encoding failed.")
                continue

            jpeg_data = buffer.tobytes()
            frame_size = len(jpeg_data)

            # 1. Send 4-byte header
            sock.sendall(struct.pack('>I', frame_size))

            # 2. Send JPEG image
            sock.sendall(jpeg_data)

            # 3. Receive echoed JPEG
            echoed_data = b''
            while len(echoed_data) < frame_size:
                chunk = sock.recv(frame_size - len(echoed_data))
                if not chunk:
                    raise ConnectionError("Server closed connection")
                echoed_data += chunk

            # 4. Decode and display echoed JPEG
            echoed_frame = cv2.imdecode(np.frombuffer(echoed_data, dtype=np.uint8), cv2.IMREAD_COLOR)
            if echoed_frame is not None:
                cv2.imshow("Echoed MJPEG Frame", echoed_frame)
            else:
                print("Failed to decode echoed JPEG.")

            if cv2.waitKey(1) == 27:  # ESC key to exit
                break

    except Exception as e:
        print(f"Client error: {e}")
    finally:
        cap.release()
        sock.close()
        cv2.destroyAllWindows()
        print("Client disconnected.")

if __name__ == "__main__":
    run_mjpeg_client()

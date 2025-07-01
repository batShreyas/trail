import socket
import struct
import cv2

SERVER_IP = '192.168.1.10'
SERVER_PORT = 6001

def run_video_client():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((SERVER_IP, SERVER_PORT))
    print("Connected to server.")

    cap = cv2.VideoCapture(0)  # or use a video file path

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                print("Failed to capture frame.")
                break

            # Encode frame as JPEG
            ret, buffer = cv2.imencode('.jpg', frame)
            if not ret:
                print("Failed to encode frame.")
                continue

            data = buffer.tobytes()
            frame_size = len(data)

            # Send 4-byte frame size
            sock.sendall(struct.pack('>I', frame_size))
            # Send actual frame
            sock.sendall(data)

            # Optionally receive echo
            received_data = b''
            while len(received_data) < frame_size:
                chunk = sock.recv(frame_size - len(received_data))
                if not chunk:
                    raise ConnectionError("Server disconnected.")
                received_data += chunk

            # Decode and display echoed frame
            echoed_frame = cv2.imdecode(
                np.frombuffer(received_data, dtype=np.uint8), cv2.IMREAD_COLOR
            )
            cv2.imshow("Echoed Frame", echoed_frame)
            if cv2.waitKey(1) == 27:  # ESC to exit
                break

    except Exception as e:
        print("Error:", e)
    finally:
        cap.release()
        sock.close()
        cv2.destroyAllWindows()

if __name__ == "__main__":
    run_video_client()

import socket
import time
import threading
import queue

SERVER_IP = '192.168.1.10'
SERVER_PORT = 7
CHUNK_SIZE = 1446
IMAGE_FILE = 'inputImage.png'
OUTPUT_FILE = 'echoedImage.png'

def receiver_thread(sock, output_file, file_size, stop_event):
    bytes_received = 0
    while bytes_received < file_size and not stop_event.is_set():
        data = sock.recv(min(CHUNK_SIZE, file_size - bytes_received))
        if not data:
            break
        output_file.write(data)
        bytes_received += len(data)
        print(f"Received: {bytes_received}/{file_size} bytes", end='\r')

def run_client():
    with open(IMAGE_FILE, 'rb') as f:
        image_data = f.read()
    file_size = len(image_data)
    
    with open(OUTPUT_FILE, 'wb') as out_file:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((SERVER_IP, SERVER_PORT))
        
        # Send header
        header = file_size.to_bytes(4, 'big')
        sock.sendall(header)
        
        # Start receiver thread
        stop_event = threading.Event()
        receiver = threading.Thread(target=receiver_thread, 
                                   args=(sock, out_file, file_size, stop_event))
        receiver.start()
        
        # Send data in full-duplex mode
        bytes_sent = 0
        start_time = time.time()
        
        while bytes_sent < file_size:
            chunk = image_data[bytes_sent:bytes_sent+CHUNK_SIZE]
            sent = sock.send(chunk)
            bytes_sent += sent
            print(f"Sent: {bytes_sent}/{file_size} bytes", end='\r')
            # Small sleep to demonstrate concurrency
            time.sleep(0.001)  
        
        # Wait for receiver
        stop_event.set()
        receiver.join()
        
        print(f"\nTransfer completed in {time.time()-start_time:.2f}s")

if __name__ == "__main__":
    run_client()
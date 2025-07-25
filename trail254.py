import socket
import os
import time
import sys

# Configuration
SERVER_IP = '192.168.1.10'  # Replace with your FPGA's IP address
SERVER_PORT = 6001
CHUNK_SIZE = 1446           # Size of each data chunk
IMAGE_FILE = 'inputImage.png' # Path to your original input PNG file
OUTPUT_IMAGE_FILE = 'echoedImage.png' # Name for the file to save the echoed data

def send_and_receive_image():
    # Verify input file exists
    if not os.path.isfile(IMAGE_FILE):
        print(f"Error: Input image file {IMAGE_FILE} not found")
        return False

    # Read the input image file
    try:
        with open(IMAGE_FILE, 'rb') as f:
            image_data = f.read()
    except IOError as e:
        print(f"Error reading input image file: {e}")
        return False

    file_size = len(image_data)
    print(f"\nStarting image echo test")
    print(f"Input image: {IMAGE_FILE}")
    print(f"Output image: {OUTPUT_IMAGE_FILE}")
    print(f"Image size: {file_size} bytes")
    print(f"Chunk size: {CHUNK_SIZE} bytes")
    print(f"Server: {SERVER_IP}:{SERVER_PORT}")

    # Create TCP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30)
    
    # Increase buffer sizes for better throughput
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
    
    try:
        # Connect to server
        print("\nConnecting to server...")
        start_connect = time.time()
        sock.connect((SERVER_IP, SERVER_PORT))
        print(f"Connected in {time.time() - start_connect:.3f} seconds")
        
        # Send file size header (4 bytes, big-endian)
        header = bytes([
            (file_size >> 24) & 0xFF,
            (file_size >> 16) & 0xFF,
            (file_size >> 8) & 0xFF,
            file_size & 0xFF
        ])
        
        print("Sending file size header...")
        sock.sendall(header)
        
        # Send and receive image in chunks
        print("Sending image data and receiving echo...")
        start_time = time.time()
        total_sent = 0
        total_received = 0
        received_data = bytearray()
        
        # Send data in chunks and receive echo simultaneously
        while total_sent < file_size or total_received < file_size:
            # Send data if we haven't sent everything
            if total_sent < file_size:
                chunk = image_data[total_sent:total_sent + CHUNK_SIZE]
                sent = sock.send(chunk)
                if sent == 0:
                    raise RuntimeError("Socket connection broken")
                total_sent += sent
            
            # Receive echoed data
            while total_received < total_sent:
                try:
                    # Only receive as much as we've sent but haven't received yet
                    to_receive = min(CHUNK_SIZE, total_sent - total_received)
                    chunk = sock.recv(to_receive)
                    if not chunk:
                        raise RuntimeError("Connection closed prematurely")
                    
                    received_data.extend(chunk)
                    total_received += len(chunk)
                except socket.timeout:
                    # No data available yet, continue sending
                    break
            
            # Display progress
            progress = min(total_sent, total_received) / file_size * 100
            print(f"Progress: sent {total_sent}/{file_size} ({total_sent/file_size:.1%}), "
                  f"received {total_received}/{file_size} ({total_received/file_size:.1%})", end='\r')
        
        transfer_time = time.time() - start_time
        print(f"\nTransfer complete in {transfer_time:.2f} seconds")
        print(f"Transfer rate: {file_size/transfer_time/1024:.2f} KB/s")
        
        # Gracefully close the connection
        print("Closing connection...")
        sock.shutdown(socket.SHUT_WR)
        
        # Receive any remaining data
        while total_received < file_size:
            chunk = sock.recv(CHUNK_SIZE)
            if not chunk:
                break
            received_data.extend(chunk)
            total_received += len(chunk)
            print(f"Received final {len(chunk)} bytes", end='\r')
        
        # Save the echoed image
        try:
            with open(OUTPUT_IMAGE_FILE, 'wb') as f:
                f.write(received_data)
            print(f"\nEchoed image saved as {OUTPUT_IMAGE_FILE}")
        except IOError as e:
            print(f"Error saving output image: {e}")
            return False
        
        # Verify received data
        if len(received_data) != file_size:
            print(f"Error: Received {len(received_data)} bytes, expected {file_size}")
            return False
        
        if received_data != image_data:
            print("Error: Received data doesn't match sent image")
            return False
        
        print("\nTest successful! Image echoed back correctly")
        return True
        
    except Exception as e:
        print(f"\nError during transfer: {e}")
        import traceback
        traceback.print_exc()
        return False
    finally:
        sock.close()

if __name__ == "__main__":
    print("KCU105 Image Echo Client")
    print("-----------------------")
    
    success = send_and_receive_image()
    
    if success:
        print("\nOperation completed successfully")
        sys.exit(0)
    else:
        print("\nOperation failed")
        sys.exit(1)
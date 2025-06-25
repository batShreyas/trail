import socket
import os
import time
import struct

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
    
    try:
        # Connect to server
        print("\nConnecting to server...")
        sock.connect((SERVER_IP, SERVER_PORT))
        
        # Send file size header (4 bytes)
        print("Sending file size header...")
        sock.sendall(struct.pack('!I', file_size))
        
        # Send image data in specified chunks
        print("Sending image data...")
        start_time = time.time()
        total_sent = 0
        
        while total_sent < file_size:
            chunk = image_data[total_sent:total_sent + CHUNK_SIZE]
            sent = sock.send(chunk)
            if sent == 0:
                raise RuntimeError("Socket connection broken")
            total_sent += sent
            print(f"Progress: {total_sent}/{file_size} bytes ({total_sent/file_size:.1%})", end='\r')
        
        transfer_time = time.time() - start_time
        print(f"\nTransfer complete in {transfer_time:.2f} seconds")
        print(f"Send rate: {file_size/transfer_time/1024:.2f} KB/s")
        
        # Shutdown sending side
        sock.shutdown(socket.SHUT_WR)
        
        # Receive echoed image
        print("\nWaiting for echoed image...")
        received_data = bytearray()
        start_time = time.time()
        total_received = 0
        
        while total_received < file_size:
            chunk = sock.recv(CHUNK_SIZE)
            if not chunk:
                break
            received_data.extend(chunk)
            total_received += len(chunk)
            print(f"Progress: {total_received}/{file_size} bytes ({total_received/file_size:.1%})", end='\r')
        
        echo_time = time.time() - start_time
        print(f"\nEcho complete in {echo_time:.2f} seconds")
        print(f"Receive rate: {file_size/echo_time/1024:.2f} KB/s")
        
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
        return False
    finally:
        sock.close()

if __name__ == "__main__":
    print("KCU105 Image Echo Client")
    print("-----------------------")
    
    success = send_and_receive_image()
    
    if success:
        print("\nOperation completed successfully")
        exit(0)
    else:
        print("\nOperation failed")
        exit(1)
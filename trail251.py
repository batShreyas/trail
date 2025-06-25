import socket
import struct
import os
import time

# --- Configuration ---
SERVER_IP = '192.168.1.10'  # Replace with your FPGA's IP address
SERVER_PORT = 6001
CHUNK_SIZE = 1446           # Size of each data chunk
IMAGE_FILE = 'input_image.png' # Path to your original input PNG file
OUTPUT_IMAGE_FILE = 'received_echo_image.png' # Name for the file to save the echoed data

# --- Create a dummy PNG file (optional, for quick testing) ---
def create_dummy_png(filename, size_kb):
    """Creates a dummy PNG file of a specified approximate size (in KB)."""
    if os.path.exists(filename) and os.path.getsize(filename) >= size_kb * 1024:
        print(f"Using existing {filename} (size: {os.path.getsize(filename)} bytes)")
        return

    print(f"Creating dummy PNG file: {filename} of ~{size_kb}KB...")
    base_png_data = bytes([
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41,
        0x54, 0x78, 0xDA, 0xED, 0xC1, 0x01, 0x01, 0x00,
        0x00, 0x00, 0x22, 0xC0, 0x20, 0x29, 0x4F, 0x59,
        0xD8, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
        0x44, 0xAE, 0x42, 0x60, 0x82
    ])
    
    dummy_data = b'\x00' * (size_kb * 1024 - len(base_png_data))
    
    with open(filename, 'wb') as f:
        f.write(base_png_data)
        f.write(dummy_data)

    print(f"Dummy PNG file created: {filename} with size {os.path.getsize(filename)} bytes")


def run_client():
    # Call to create dummy PNG. REMOVE THIS LINE for real images.
    create_dummy_png(IMAGE_FILE, 100) # Creates a ~100KB dummy PNG if not exists/too small

    try:
        with open(IMAGE_FILE, 'rb') as f_in:
            image_data = f_in.read()
    except FileNotFoundError:
        print(f"Error: Input image file '{IMAGE_FILE}' not found. Please create one or check path.")
        return

    total_image_size = len(image_data)
    print(f"Input image '{IMAGE_FILE}' loaded. Total size: {total_image_size} bytes.")

    output_file_handle = None
    try:
        output_file_handle = open(OUTPUT_IMAGE_FILE, 'wb')
        print(f"Output file '{OUTPUT_IMAGE_FILE}' opened for writing echoed data.")
        
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        
        recv_buffer_size = 1024 * 1024 # 1MB
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, recv_buffer_size)
        actual_recv_buffer_size = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
        print(f"Client socket requested SO_RCVBUF: {recv_buffer_size} bytes, actual: {actual_recv_buffer_size} bytes")

        print(f"Connecting to {SERVER_IP}:{SERVER_PORT}...")
        sock.connect((SERVER_IP, SERVER_PORT))
        print("Connected to server.")

        # Send 4-byte header (total image size)
        header = struct.pack('>I', total_image_size)
        sock.sendall(header)
        print(f"Sent 4-byte header: {total_image_size} bytes (raw: {header.hex()})")
        time.sleep(0.1)

        bytes_sent = 0
        bytes_received_echo = 0
        total_chunks = (total_image_size + CHUNK_SIZE - 1) // CHUNK_SIZE
        
        print("Starting data transfer and echo reception...")

        while bytes_sent < total_image_size:
            chunk_start = bytes_sent
            chunk_end = min(bytes_sent + CHUNK_SIZE, total_image_size)
            chunk_data = image_data[chunk_start:chunk_end]
            
            # Send the chunk
            sock.sendall(chunk_data)
            bytes_sent += len(chunk_data)
            print(f"Sent chunk {bytes_sent // CHUNK_SIZE}/{total_chunks}: {len(chunk_data)} bytes. Total sent: {bytes_sent}/{total_image_size}")

            # Receive the echo for this chunk
            received_chunk = b''
            bytes_in_current_echo = 0
            while bytes_in_current_echo < len(chunk_data):
                data = sock.recv(len(chunk_data) - bytes_in_current_echo)
                if not data:
                    print(f"Server closed connection prematurely. Received {bytes_in_current_echo}/{len(chunk_data)} bytes for current chunk.")
                    raise ConnectionResetError("Server closed connection")
                received_chunk += data
                bytes_in_current_echo += len(data)
                
            # Write the received echoed chunk to the output file
            output_file_handle.write(received_chunk)
            bytes_received_echo += len(received_chunk)

            if received_chunk != chunk_data:
                print(f"WARNING: Echo mismatch for chunk starting at {chunk_start}. Expected {len(chunk_data)} bytes, Got {len(received_chunk)} bytes.")
            else:
                print(f"Received echo for chunk {bytes_received_echo // CHUNK_SIZE}/{total_chunks}: {len(received_chunk)} bytes. Total echoed received: {bytes_received_echo}/{total_image_size}")

            # Optional: Small delay
            # time.sleep(0.001)

        print("\n--- Transfer Summary ---")
        print(f"Total bytes sent: {bytes_sent}")
        print(f"Total bytes received (echo): {bytes_received_echo}")

        if bytes_sent == total_image_size and bytes_received_echo == total_image_size:
            print("All data sent and echoed successfully.")
            print(f"Please check '{OUTPUT_IMAGE_FILE}' to verify the received image.")
        else:
            print("Transfer incomplete or echo mismatch occurred.")

    except socket.error as e:
        print(f"Socket error: {e}")
    except ConnectionResetError as e:
        print(f"Connection error: {e}")
    except Exception as e:
        print(f"An unexpected error occurred: {e}")
    finally:
        print("Closing socket.")
        if 'sock' in locals() and sock:
            sock.close()
        if output_file_handle:
            output_file_handle.close()
            print(f"Output file '{OUTPUT_IMAGE_FILE}' closed.")

if __name__ == "__main__":
    run_client()
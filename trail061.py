import socket
import struct
import os
import time

# --- Configuration ---
SERVER_IP = '192.168.1.10'  # IMPORTANT: Replace with your FPGA's actual IP address
SERVER_PORT = 6001
CHUNK_SIZE = 1446           # Size of each data chunk (matching TCP_MSS for efficiency)

# --- Video file paths ---
# IMPORTANT: Replace 'path/to/your/input_video.mp4' with the actual path to your video file.
# Example: 'my_videos/test_video.mp4' or 'C:/Users/YourUser/Videos/sample.mp4'
VIDEO_FILE = 'path/to/your/input_video.mp4'
OUTPUT_VIDEO_FILE = 'received_echo_video.mp4' # Name for the file to save the echoed video data


def run_client():
    try:
        with open(VIDEO_FILE, 'rb') as f_in:
            video_data = f_in.read()
    except FileNotFoundError:
        print(f"Error: Input video file '{VIDEO_FILE}' not found.")
        print("Please ensure the file exists and the 'VIDEO_FILE' variable points to its correct path.")
        return

    total_video_size = len(video_data)
    print(f"Input video '{VIDEO_FILE}' loaded. Total size: {total_video_size} bytes.")

    output_file_handle = None
    sock = None
    start_time = time.time() # Start timer

    try:
        output_file_handle = open(OUTPUT_VIDEO_FILE, 'wb')
        print(f"Output file '{OUTPUT_VIDEO_FILE}' opened for writing echoed data.")
        
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        
        # Set a larger receive buffer on the client side (e.g., 4MB)
        recv_buffer_size = 1024 * 1024 * 4
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, recv_buffer_size)
        actual_recv_buffer_size = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
        print(f"Client socket requested SO_RCVBUF: {recv_buffer_size} bytes, actual: {actual_recv_buffer_size} bytes")

        print(f"Connecting to {SERVER_IP}:{SERVER_PORT}...")
        sock.connect((SERVER_IP, SERVER_PORT))
        print("Connected to server.")

        # 1. Send 4-byte header (total video size)
        header = struct.pack('>I', total_video_size) # Big-endian unsigned int
        sock.sendall(header)
        print(f"Sent 4-byte header: {total_video_size} bytes (raw: {header.hex()})")
        time.sleep(0.05) # Small delay to give server time to process header

        bytes_sent = 0
        bytes_received_echo = 0
        total_chunks = (total_video_size + CHUNK_SIZE - 1) // CHUNK_SIZE
        
        print("Starting full-duplex data transfer and echo reception...")

        while bytes_sent < total_video_size:
            chunk_start = bytes_sent
            chunk_end = min(bytes_sent + CHUNK_SIZE, total_video_size)
            chunk_data_to_send = video_data[chunk_start:chunk_end]
            
            # Send the chunk
            sock.sendall(chunk_data_to_send)
            bytes_sent += len(chunk_data_to_send)
            print(f"Client: Sent chunk {bytes_sent // CHUNK_SIZE}/{total_chunks}: {len(chunk_data_to_send)} bytes. Total sent: {bytes_sent}/{total_video_size}")

            # Immediately wait to receive the echo for this chunk
            received_chunk_echo = b''
            bytes_for_this_echo = len(chunk_data_to_send) # Expecting an echo of same size
            
            while len(received_chunk_echo) < bytes_for_this_echo:
                # Read remaining bytes for this echo from socket's receive buffer
                data = sock.recv(bytes_for_this_echo - len(received_chunk_echo))
                if not data:
                    print(f"Client: Server closed connection prematurely! Expected {bytes_for_this_echo} bytes for current echo, got {len(received_chunk_echo)} so far.")
                    raise ConnectionResetError("Server closed connection during echo reception")
                received_chunk_echo += data
                
            # Write the received echoed chunk to the output file
            output_file_handle.write(received_chunk_echo)
            bytes_received_echo += len(received_chunk_echo)

            if received_chunk_echo != chunk_data_to_send:
                print(f"Client: WARNING! Echo mismatch for chunk starting at {chunk_start}. Expected {len(chunk_data_to_send)} bytes, Got {len(received_chunk_echo)} bytes.")
            else:
                print(f"Client: Received echo for chunk {bytes_received_echo // CHUNK_SIZE}/{total_chunks}: {len(received_chunk_echo)} bytes. Total echoed received: {bytes_received_echo}/{total_video_size}")

        end_time = time.time() # End timer
        transfer_duration = end_time - start_time

        print("\n--- Transfer Summary ---")
        print(f"Total bytes sent: {bytes_sent}")
        print(f"Total bytes received (echo): {bytes_received_echo}")
        print(f"Transfer duration: {transfer_duration:.2f} seconds")
        if transfer_duration > 0:
            throughput_bps = (bytes_sent * 8) / transfer_duration
            throughput_mbps = throughput_bps / (1024 * 1024)
            print(f"Calculated throughput: {throughput_mbps:.2f} Mbps")

        if bytes_sent == total_video_size and bytes_received_echo == total_video_size:
            print("Client: All data sent and echoed successfully.")
            print(f"Client: Received video saved to '{OUTPUT_VIDEO_FILE}'. Try playing it with a video player.")
            
            if os.path.exists(OUTPUT_VIDEO_FILE):
                output_size = os.path.getsize(OUTPUT_VIDEO_FILE)
                if output_size == total_video_size:
                    print(f"Client: Output file size ({output_size} bytes) matches original video size. Basic verification SUCCESS.")
                else:
                    print(f"Client: Output file size ({output_size} bytes) MISMATCHES original size ({total_video_size} bytes). Basic verification FAILED.")
            else:
                print(f"Client: Output file '{OUTPUT_VIDEO_FILE}' was not created.")
        else:
            print("Client: Transfer incomplete or echo mismatch occurred.")

    except socket.error as e:
        print(f"Client: Socket error: {e}")
    except ConnectionResetError as e:
        print(f"Client: Connection error: {e}")
    except Exception as e:
        print(f"Client: An unexpected error occurred: {e}")
    finally:
        print("Client: Closing socket.")
        if sock:
            sock.close()
        if output_file_handle:
            output_file_handle.close()
            print(f"Client: Output file '{OUTPUT_VIDEO_FILE}' closed.")

if __name__ == "__main__":
    run_client()
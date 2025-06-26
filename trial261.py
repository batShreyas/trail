import socket
import struct
import os
import time
import threading
import queue

# --- Configuration ---
SERVER_IP = '192.168.1.10'  # Replace with your FPGA's IP address
SERVER_PORT = 7
CHUNK_SIZE = 1446           # Size of each data chunk
IMAGE_FILE = 'input_image.png' # Path to your original input PNG file
OUTPUT_IMAGE_FILE = 'received_echo_image.png' # Name for the file to save the echoed data

# --- Receiver thread function ---
def receiver_thread(sock, total_image_size, output_queue, stop_event):
    """Continuously receives data and puts it in the output queue."""
    bytes_received = 0
    try:
        while bytes_received < total_image_size and not stop_event.is_set():
            # Calculate how much we expect to receive
            remaining = total_image_size - bytes_received
            to_receive = min(CHUNK_SIZE, remaining)
            
            # Receive data
            data = sock.recv(to_receive)
            if not data:
                print("Receiver: Connection closed by server")
                break
                
            bytes_received += len(data)
            output_queue.put((bytes_received, data))
            
            # Print progress periodically
            if bytes_received % (10 * CHUNK_SIZE) == 0:
                print(f"Receiver: Received {bytes_received}/{total_image_size} bytes ({bytes_received/total_image_size:.1%})")
                
    except Exception as e:
        print(f"Receiver thread error: {e}")
    finally:
        # Signal that we're done
        output_queue.put((bytes_received, None))
        print(f"Receiver thread exiting. Total received: {bytes_received} bytes")

def run_client():
    # Call to create dummy PNG. REMOVE THIS LINE for real images.
    #create_dummy_png(IMAGE_FILE, 100) # Creates a ~100KB dummy PNG if not exists/too small

    try:
        with open(IMAGE_FILE, 'rb') as f_in:
            image_data = f_in.read()
    except FileNotFoundError:
        print(f"Error: Input image file '{IMAGE_FILE}' not found. Please create one or check path.")
        return

    total_image_size = len(image_data)
    print(f"Input image '{IMAGE_FILE}' loaded. Total size: {total_image_size} bytes.")

    output_file_handle = None
    sock = None
    try:
        output_file_handle = open(OUTPUT_IMAGE_FILE, 'wb')
        print(f"Output file '{OUTPUT_IMAGE_FILE}' opened for writing echoed data.")
        
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        
        # Increase socket buffer sizes for better full-duplex performance
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024 * 1024)  # 1MB send buffer
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)  # 1MB receive buffer
        
        # Get actual buffer sizes
        actual_send_buf = sock.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF)
        actual_recv_buf = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
        print(f"Client socket buffers - Send: {actual_send_buf} bytes, Recv: {actual_recv_buf} bytes")

        print(f"Connecting to {SERVER_IP}:{SERVER_PORT}...")
        sock.connect((SERVER_IP, SERVER_PORT))
        print("Connected to server.")

        # Create a queue for received data and a stop event
        output_queue = queue.Queue()
        stop_event = threading.Event()
        
        # Start receiver thread
        receiver = threading.Thread(target=receiver_thread, 
                                   args=(sock, total_image_size, output_queue, stop_event))
        receiver.daemon = True
        receiver.start()
        
        # Send 4-byte header (total image size)
        header = struct.pack('>I', total_image_size)
        sock.sendall(header)
        print(f"Sent 4-byte header: {total_image_size} bytes (raw: {header.hex()})")
        
        # Start timing the transfer
        start_time = time.time()
        bytes_sent = 0
        bytes_written = 0
        bytes_processed = 0
        total_chunks = (total_image_size + CHUNK_SIZE - 1) // CHUNK_SIZE
        
        print("Starting full-duplex data transfer...")
        print("Sending and receiving simultaneously...")

        # Send all chunks while processing received data
        chunk_index = 0
        while bytes_sent < total_image_size or bytes_written < total_image_size:
            # Send next chunk if we haven't sent everything
            if bytes_sent < total_image_size:
                chunk_start = bytes_sent
                chunk_end = min(bytes_sent + CHUNK_SIZE, total_image_size)
                chunk_data = image_data[chunk_start:chunk_end]
                
                # Send the chunk
                sock.sendall(chunk_data)
                bytes_sent += len(chunk_data)
                chunk_index += 1
                
                # Print progress periodically
                if chunk_index % 10 == 0:
                    print(f"Sent chunk {chunk_index}/{total_chunks}: {len(chunk_data)} bytes. Total sent: {bytes_sent}/{total_image_size}")
            
            # Process received data from queue
            while not output_queue.empty():
                try:
                    received_bytes, data = output_queue.get_nowait()
                    if data is None:  # End signal
                        break
                        
                    # Write to file
                    output_file_handle.write(data)
                    bytes_written += len(data)
                    bytes_processed = received_bytes  # Update processed count
                    
                    # Print progress periodically
                    if bytes_processed % (10 * CHUNK_SIZE) == 0:
                        print(f"Received: {bytes_processed}/{total_image_size} bytes ({bytes_processed/total_image_size:.1%})")
                        
                except queue.Empty:
                    break
            
            # Small sleep to prevent busy-waiting
            time.sleep(0.001)
        
        # Signal receiver thread to stop
        stop_event.set()
        
        # Process any remaining data in the queue
        while bytes_written < total_image_size:
            try:
                received_bytes, data = output_queue.get(timeout=1.0)
                if data is None:  # End signal
                    break
                    
                output_file_handle.write(data)
                bytes_written += len(data)
                bytes_processed = received_bytes
                
            except queue.Empty:
                print("Timeout waiting for final data")
                break
        
        # Calculate transfer time
        transfer_time = time.time() - start_time
        
        print("\n--- Transfer Complete ---")
        print(f"Total time: {transfer_time:.2f} seconds")
        print(f"Transfer rate: {total_image_size/transfer_time/1024:.2f} KB/s")
        print(f"Total bytes sent: {bytes_sent}")
        print(f"Total bytes received: {bytes_processed}")
        print(f"Total bytes written to file: {bytes_written}")

        # Verify we received all data
        if bytes_written == total_image_size:
            print("All data received and written successfully.")
        else:
            print(f"Warning: Only received {bytes_written} of {total_image_size} bytes")

        # Verify file content if size matches
        if bytes_written == total_image_size:
            output_file_handle.close()
            
            # Read the received file
            with open(OUTPUT_IMAGE_FILE, 'rb') as f_out:
                received_data = f_out.read()
                
            # Compare with original
            if received_data == image_data:
                print("File verification: SUCCESS - Received data matches original!")
            else:
                print("File verification: FAILURE - Received data does not match original!")
        else:
            print("Skipping file verification due to incomplete transfer")

    except socket.error as e:
        print(f"Socket error: {e}")
    except ConnectionResetError as e:
        print(f"Connection error: {e}")
    except Exception as e:
        print(f"An unexpected error occurred: {e}")
    finally:
        print("Cleaning up...")
        if stop_event:
            stop_event.set()
            
        if sock:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except:
                pass
            sock.close()
            
        if output_file_handle:
            output_file_handle.close()
            print(f"Output file '{OUTPUT_IMAGE_FILE}' closed.")

if __name__ == "__main__":
    run_client()
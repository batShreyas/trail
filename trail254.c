/******************************************************************************
* Robust Streaming Image Echo Server with DDR4 Storage
* For Xilinx KCU105 Board with 2GB DDR4 RAM
******************************************************************************/

#include <stdio.h>
#include <string.h>
#include "lwip/err.h"
#include "lwip/tcp.h"
#if defined (__arm__) || defined (__aarch64__)
#include "xil_printf.h"
#endif
#include "xil_printf.h"
#include "xil_cache.h"
#include "xil_exception.h"

#define SERVER_PORT 6001
#define MAX_IMAGE_SIZE (512 * 1024 * 1024) // 512MB
#define DDR4_IMAGE_BUFFER_START_ADDR 0x90000000
#define HEADER_SIZE 4
#define CHUNK_SIZE 1446

typedef struct {
    u32_t buffer_addr;     // DDR4 memory address
    u32_t received_bytes;  // Total bytes received
    u32_t file_size;       // Expected file size
    u32_t echoed_bytes;    // Bytes already echoed back
    struct tcp_pcb *pcb;   // Connection PCB
    u8_t header_received;  // Flag for size header
    u8_t closing;          // Connection closing flag
} image_connection_t;

void init_ddr_memory() {
    Xil_DCacheFlush();
    Xil_ICacheInvalidate();
    xil_printf("DDR4 Memory initialized at 0x%08x\n\r", DDR4_IMAGE_BUFFER_START_ADDR);
}

err_t sent_callback(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    image_connection_t *conn = (image_connection_t *)arg;
    
    if (!conn) {
        return ERR_ARG;
    }
    
    // Update echoed bytes count
    conn->echoed_bytes += len;
    xil_printf("Sent %d bytes (total echoed: %d)\n\r", len, conn->echoed_bytes);
    
    // Check if we should close the connection
    if (conn->closing && conn->echoed_bytes >= conn->received_bytes) {
        xil_printf("All data echoed, closing connection\n\r");
        tcp_arg(tpcb, NULL);
        tcp_sent(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_close(tpcb);
        mem_free(conn);
    }
    
    return ERR_OK;
}

err_t recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    image_connection_t *conn = (image_connection_t *)arg;
    
    if (!conn || err != ERR_OK) {
        if (p) pbuf_free(p);
        return ERR_ARG;
    }

    // Handle connection closure
    if (!p) {
        if (conn->echoed_bytes < conn->received_bytes) {
            // Still have data to echo back
            conn->closing = 1;
            xil_printf("Client closed connection, waiting to echo remaining data\n\r");
        } else {
            // All data echoed back, close immediately
            xil_printf("Connection closed. Total bytes stored: %d\n\r", conn->received_bytes);
            tcp_arg(tpcb, NULL);
            tcp_sent(tpcb, NULL);
            tcp_recv(tpcb, NULL);
            tcp_close(tpcb);
            mem_free(conn);
        }
        return ERR_OK;
    }

    // Handle file size header if not yet received
    if (!conn->header_received) {
        if (p->tot_len < HEADER_SIZE) {
            xil_printf("Waiting for more header data...\n\r");
            return ERR_OK;
        }

        // Extract file size from header
        u8_t *header = p->payload;
        conn->file_size = (header[0] << 24) | (header[1] << 16) | 
                         (header[2] << 8) | header[3];
        
        conn->buffer_addr = DDR4_IMAGE_BUFFER_START_ADDR;
        conn->header_received = 1;
        xil_printf("Expected file size: %d bytes\n\r", conn->file_size);
        
        // Remove header from pbuf
        if (pbuf_header(p, -HEADER_SIZE) != 0) {
            xil_printf("Header removal failed\n\r");
            tcp_close(tpcb);
            pbuf_free(p);
            mem_free(conn);
            return ERR_VAL;
        }
    }

    // Check DDR4 space
    if (conn->received_bytes + p->tot_len > MAX_IMAGE_SIZE) {
        xil_printf("Image exceeds maximum size (%d MB)\n\r", MAX_IMAGE_SIZE/(1024*1024));
        pbuf_free(p);
        tcp_close(tpcb);
        mem_free(conn);
        return ERR_MEM;
    }

    // Process received data
    u32_t bytes_processed = 0;
    struct pbuf *q = p;
    while (q) {
        u32_t chunk_len = q->len;
        
        // Store in DDR4
        memcpy((void*)(conn->buffer_addr + conn->received_bytes), q->payload, chunk_len);
        Xil_DCacheFlushRange(conn->buffer_addr + conn->received_bytes, chunk_len);
        
        // Echo back immediately
        err_t err = tcp_write(tpcb, q->payload, chunk_len, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) {
            xil_printf("tcp_write failed: %d\n\r", err);
            pbuf_free(p);
            return err;
        }
        
        conn->received_bytes += chunk_len;
        bytes_processed += chunk_len;
        q = q->next;
    }

    xil_printf("Processed %d bytes (total: %d)\n\r", bytes_processed, conn->received_bytes);

    // Update TCP receive window
    tcp_recved(tpcb, p->tot_len);
    
    // Free the pbuf
    pbuf_free(p);

    // Send the echoed data immediately
    tcp_output(tpcb);
    
    return ERR_OK;
}

err_t accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    image_connection_t *conn;

    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }

    conn = (image_connection_t *)mem_malloc(sizeof(image_connection_t));
    if (!conn) {
        xil_printf("Failed to allocate connection struct\n\r");
        return ERR_MEM;
    }

    memset(conn, 0, sizeof(image_connection_t));
    conn->pcb = newpcb;
    conn->buffer_addr = 0;
    conn->received_bytes = 0;
    conn->echoed_bytes = 0;
    conn->header_received = 0;
    conn->closing = 0;

    tcp_arg(newpcb, conn);
    tcp_recv(newpcb, recv_callback);
    tcp_sent(newpcb, sent_callback);
    
    // Disable Nagle's algorithm for low latency
    tcp_nagle_disable(newpcb);

    xil_printf("New connection established\n\r");
    return ERR_OK;
}

int start_application()
{
    struct tcp_pcb *pcb;
    err_t err;

    init_ddr_memory();

    pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        xil_printf("Error creating PCB. Out of Memory\n\r");
        return -1;
    }

    err = tcp_bind(pcb, IP_ANY_TYPE, SERVER_PORT);
    if (err != ERR_OK) {
        xil_printf("Unable to bind to port %d: err = %d\n\r", SERVER_PORT, err);
        return -2;
    }

    pcb = tcp_listen(pcb);
    if (!pcb) {
        xil_printf("Out of memory while tcp_listen\n\r");
        return -3;
    }

    tcp_accept(pcb, accept_callback);

    xil_printf("TCP streaming echo server started @ port %d\n\r", SERVER_PORT);
    xil_printf("Using DDR4 at 0x%08x (max %d MB)\n\r", 
              DDR4_IMAGE_BUFFER_START_ADDR, MAX_IMAGE_SIZE/(1024*1024));
    return 0;
}
/******************************************************************************
* Streaming Image Echo Server with DDR4 Storage
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
#define MAX_IMAGE_SIZE (512 * 1024 * 1024) // 512MB (safe margin within 2GB DDR4)
#define DDR4_IMAGE_BUFFER_START_ADDR 0x90000000 // KCU105 DDR4 buffer address
#define HEADER_SIZE 4  // File size header is 4 bytes

typedef struct {
    u32_t buffer_addr;     // DDR4 memory address
    u32_t received_bytes;  // Total bytes received
    u32_t file_size;       // Expected file size
    struct tcp_pcb *pcb;   // Connection PCB
    u8_t header_received;  // Flag for size header
} image_connection_t;

void init_ddr_memory() {
    Xil_DCacheFlush();
    Xil_ICacheInvalidate();
    xil_printf("DDR4 Memory initialized at 0x%08x\n\r", DDR4_IMAGE_BUFFER_START_ADDR);
}

err_t recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    image_connection_t *conn = (image_connection_t *)arg;
    err_t ret_err = ERR_OK;
    
    if (!conn) {
        tcp_close(tpcb);
        return ERR_ARG;
    }

    if (!p) {
        // Connection closed by client
        xil_printf("Connection closed. Total bytes stored: %d\n\r", conn->received_bytes);
        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_close(tpcb);
        mem_free(conn);
        return ERR_OK;
    }

    // Handle file size header if not yet received
    if (!conn->header_received) {
        if (p->tot_len < HEADER_SIZE) {
            xil_printf("Waiting for more header data...\n\r");
            return ERR_OK;
        }

        // Extract file size from header (big-endian)
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

    // Check if we have enough DDR4 space
    if (conn->received_bytes + p->tot_len > MAX_IMAGE_SIZE) {
        xil_printf("Image exceeds maximum size (%d MB)\n\r", MAX_IMAGE_SIZE/(1024*1024));
        pbuf_free(p);
        tcp_close(tpcb);
        mem_free(conn);
        return ERR_MEM;
    }

    // Process received data
    struct pbuf *q = p;
    while (q) {
        // Store in DDR4
        memcpy((void*)(conn->buffer_addr + conn->received_bytes), q->payload, q->len);
        Xil_DCacheFlushRange(conn->buffer_addr + conn->received_bytes, q->len);
        
        // Echo back immediately
        err_t err = tcp_write(tpcb, q->payload, q->len, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) {
            xil_printf("tcp_write failed: %d\n\r", err);
            ret_err = err;
            break;
        }
        
        conn->received_bytes += q->len;
        xil_printf("Processed %d bytes (total: %d)\n\r", q->len, conn->received_bytes);
        q = q->next;
    }

    // Send the echoed data
    if (ret_err == ERR_OK) {
        err = tcp_output(tpcb);
        if (err != ERR_OK) {
            xil_printf("tcp_output failed: %d\n\r", err);
            ret_err = err;
        }
    }

    // Update TCP receive window
    tcp_recved(tpcb, p->tot_len);
    
    // Free the pbuf
    pbuf_free(p);

    return ret_err;
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
    conn->header_received = 0;

    tcp_arg(newpcb, conn);
    tcp_recv(newpcb, recv_callback);

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
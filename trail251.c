#include <stdio.h>
#include <string.h>

#include "lwip/err.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"

#if defined (__arm__) || defined (__aarch64__)
#include "xil_printf.h"
#include "xil_types.h"
#include "xil_cache.h"
#else
#define xil_printf printf
#endif

// Configuration for image buffer and network
#define MAX_IMAGE_BUFFER_SIZE (1024 * 1024 * 10) // 10 MB max image
#define DDR4_IMAGE_BUFFER_START_ADDR 0x10000000

// Global variables for single connection state
static struct tcp_pcb *active_pcb_global = NULL;
static char *image_storage_buffer_global = (char *)DDR4_IMAGE_BUFFER_START_ADDR;
static u32_t current_buffer_offset_global = 0;
static u32_t expected_total_image_size_global = 0;
static int is_header_processed_global = 0;
static u8_t header_byte_collection_buffer_global[4];
static u8_t header_bytes_in_buffer_global = 0;
static u32_t total_received_data_len_global = 0;
static u32_t total_echoed_data_len_global = 0;
static int echoing_in_progress_global = 0; // 0=idle, 1=echo pending ACK

// Function prototypes
static err_t server_recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static err_t server_sent_callback(void *arg, struct tcp_pcb *tpcb, u16_t len);
static err_t server_accept_callback(void *arg, struct tcp_pcb *new_pcb, err_t err);
static void server_error_callback(void *arg, err_t err);
static void server_close_connection(struct tcp_pcb *pcb);
static err_t server_poll_callback(void *arg, struct tcp_pcb *tpcb);

static void try_echo_chunk(struct tcp_pcb *pcb, const char *data_ptr, u16_t len) {
    err_t err;

    if (len > 0 && tcp_sndbuf(pcb) >= len) {
        err = tcp_write(pcb, data_ptr, len, TCP_WRITE_FLAG_COPY);
        if (err == ERR_OK) {
            total_echoed_data_len_global += len;
            echoing_in_progress_global = 1;
            xil_printf("SERVER: Echo %u bytes. Total echoed: %lu/%lu\n\r",
                       len, (unsigned long)total_echoed_data_len_global, (unsigned long)total_received_data_len_global);
        } else if (err == ERR_MEM) {
            xil_printf("SERVER: Echo failed, ERR_MEM. Retry on sent_callback.\n\r");
            echoing_in_progress_global = 0;
        } else {
            xil_printf("SERVER: Echo error: %d\n\r", err);
            server_close_connection(pcb);
            return;
        }
    } else {
        xil_printf("SERVER: Cannot echo %u bytes (buffer full or no data). snd_buf: %u\n\r", len, tcp_sndbuf(pcb));
        echoing_in_progress_global = 0;
    }

    err = tcp_output(pcb);
    if (err != ERR_OK) {
        xil_printf("SERVER: tcp_output error: %d\n\r", err);
        server_close_connection(pcb);
    }
}

static err_t server_recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    LWIP_UNUSED_ARG(arg);

    if (err != ERR_OK) {
        xil_printf("SERVER: Receive error: %d. Closing.\n\r", err);
        if (p) pbuf_free(p);
        server_close_connection(tpcb);
        return ERR_OK;
    }

    if (!p) {
        xil_printf("SERVER: Client closed connection. Total received: %lu, Total echoed: %lu. Closing.\n\r",
                   (unsigned long)total_received_data_len_global, (unsigned long)total_echoed_data_len_global);
        server_close_connection(tpcb);
        return ERR_OK;
    }

    tcp_recved(tpcb, p->tot_len);

    u16_t current_incoming_packet_length = p->tot_len;
    char *current_packet_data_pointer = (char *)p->payload;

    if (!is_header_processed_global) {
        u16_t bytes_needed_for_header = 4 - header_bytes_in_buffer_global;
        u16_t bytes_available_in_pbuf_for_header = LWIP_MIN(current_incoming_packet_length, bytes_needed_for_header);

        memcpy(header_byte_collection_buffer_global + header_bytes_in_buffer_global,
               current_packet_data_pointer, bytes_available_in_pbuf_for_header);
        header_bytes_in_buffer_global += bytes_available_in_pbuf_for_header;

        current_packet_data_pointer += bytes_available_in_pbuf_for_header;
        current_incoming_packet_length -= bytes_available_in_pbuf_for_header;

        if (header_bytes_in_buffer_global == 4) {
            is_header_processed_global = 1;
            expected_total_image_size_global = (u32_t)header_byte_collection_buffer_global[0] << 24 |
                                               (u32_t)header_byte_collection_buffer_global[1] << 16 |
                                               (u32_t)header_byte_collection_buffer_global[2] << 8  |
                                               (u32_t)header_byte_collection_buffer_global[3];

            xil_printf("SERVER: Header processed. Expected image size: %lu bytes.\n\r",
                       (unsigned long)expected_total_image_size_global);

            if (expected_total_image_size_global == 0 || expected_total_image_size_global > MAX_IMAGE_BUFFER_SIZE) {
                xil_printf("SERVER: ERROR: Invalid image size (%lu). Max allowed: %lu. Closing.\n\r",
                           (unsigned long)expected_total_image_size_global, (unsigned long)MAX_IMAGE_BUFFER_SIZE);
                pbuf_free(p);
                server_close_connection(tpcb);
                return ERR_ABRT;
            }
        }
    }

    if (is_header_processed_global && current_incoming_packet_length > 0) {
        u16_t bytes_to_copy_to_ddr = LWIP_MIN(current_incoming_packet_length,
                                              (u16_t)(expected_total_image_size_global - total_received_data_len_global));
        bytes_to_copy_to_ddr = LWIP_MIN(bytes_to_copy_to_ddr,
                                              (u16_t)(MAX_IMAGE_BUFFER_SIZE - current_buffer_offset_global));

        if (bytes_to_copy_to_ddr > 0) {
            memcpy(image_storage_buffer_global + current_buffer_offset_global,
                   current_packet_data_pointer, bytes_to_copy_to_ddr);

            #if defined (__arm__) || defined (__aarch64__)
            Xil_DCacheFlushRange((UINTPTR)(image_storage_buffer_global + current_buffer_offset_global), bytes_to_copy_to_ddr);
            #endif

            current_buffer_offset_global += bytes_to_copy_to_ddr;
            total_received_data_len_global += bytes_to_copy_to_ddr;

            xil_printf("SERVER: Recv %u image bytes. Total: %lu/%lu.\n\r",
                       bytes_to_copy_to_ddr, (unsigned long)total_received_data_len_global, (unsigned long)expected_total_image_size_global);

            try_echo_chunk(tpcb, current_packet_data_pointer, bytes_to_copy_to_ddr);
        } else {
            if (total_received_data_len_global >= expected_total_image_size_global) {
                xil_printf("SERVER: Image complete. Discarding extra data.\n\r");
            } else {
                xil_printf("SERVER: DDR4 buffer full or image size mismatch. Closing.\n\r");
                pbuf_free(p);
                server_close_connection(tpcb);
                return ERR_ABRT;
            }
        }
    }

    pbuf_free(p);
    return ERR_OK;
}

static err_t server_sent_callback(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    LWIP_UNUSED_ARG(arg);
    xil_printf("SERVER: Sent/ACK'd: %u bytes. Total echoed: %lu.\n\r", len, (unsigned long)total_echoed_data_len_global);
    echoing_in_progress_global = 0;

    if (total_echoed_data_len_global == expected_total_image_size_global &&
        total_received_data_len_global == expected_total_image_size_global &&
        is_header_processed_global) {
        xil_printf("SERVER: All %lu bytes of image received and echoed. Closing.\n\r",
                   (unsigned long)total_echoed_data_len_global);
        server_close_connection(tpcb);
    }
    return ERR_OK;
}

static void server_error_callback(void *arg, err_t err) {
    LWIP_UNUSED_ARG(arg);
    xil_printf("SERVER: Connection error %d. Resetting state.\n\r", err);
    // lwIP handles PCB freeing in error path. Just reset global state.
    active_pcb_global = NULL;
    is_header_processed_global = 0;
    header_bytes_in_buffer_global = 0;
    expected_total_image_size_global = 0;
    current_buffer_offset_global = 0;
    total_received_data_len_global = 0;
    total_echoed_data_len_global = 0;
    echoing_in_progress_global = 0;
}

static err_t server_poll_callback(void *arg, struct tcp_pcb *tpcb) {
    LWIP_UNUSED_ARG(arg);
    return ERR_OK;
}

static void server_close_connection(struct tcp_pcb *pcb) {
    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        tcp_close(pcb);
    }
    
    // Reset all global state for next connection
    active_pcb_global = NULL;
    is_header_processed_global = 0;
    header_bytes_in_buffer_global = 0;
    expected_total_image_size_global = 0;
    current_buffer_offset_global = 0;
    total_received_data_len_global = 0;
    total_echoed_data_len_global = 0;
    echoing_in_progress_global = 0;

    xil_printf("SERVER: Connection closed and state reset.\n\r");
}

static err_t server_accept_callback(void *arg, struct tcp_pcb *new_pcb, err_t err) {
    LWIP_UNUSED_ARG(arg);

    if (err != ERR_OK) {
        xil_printf("SERVER: Accept callback error: %d\n\r", err);
        return err;
    }

    if (active_pcb_global != NULL) {
        xil_printf("SERVER: Connection rejected: server busy. PCB: %lu.\n\r", (UINTPTR)active_pcb_global);
        tcp_abort(new_pcb);
        return ERR_ABRT;
    }

    active_pcb_global = new_pcb;
    // No struct to pass, so arg will remain NULL for now, or just pass a dummy value like 1
    tcp_arg(new_pcb, NULL); // We are not using `arg` in callbacks, but lwIP requires it to be set.

    tcp_recv(new_pcb, server_recv_callback);
    tcp_sent(new_pcb, server_sent_callback);
    tcp_err(new_pcb, server_error_callback);
    tcp_poll(new_pcb, server_poll_callback, 4);

    tcp_set_recv_wnd(new_pcb, TCP_WND);

    xil_printf("SERVER: Accepted new connection (PCB: %lu). Waiting for header...\n\r", (UINTPTR)new_pcb);

    return ERR_OK;
}

void echo_server_init(void) {
    struct tcp_pcb *pcb;
    err_t err;
    unsigned port = 6001;

    pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        xil_printf("SERVER: Error creating PCB. Out of Memory.\n\r");
        return;
    }

    err = tcp_bind(pcb, IP_ANY_TYPE, port);
    if (err != ERR_OK) {
        xil_printf("SERVER: Bind error %d\n\r", err);
        tcp_abort(pcb);
        return;
    }

    struct tcp_pcb *listen_pcb = tcp_listen(pcb);
    if (!listen_pcb) {
        xil_printf("SERVER: tcp_listen failed.\n\r");
        return;
    }

    tcp_accept(listen_pcb, server_accept_callback);

    xil_printf("SERVER: TCP image echo server started @ port %d.\n\r", port);
    xil_printf("SERVER: DDR4 Image Buffer Address: 0x%08lX.\n\r", (UINTPTR)DDR4_IMAGE_BUFFER_START_ADDR);
}
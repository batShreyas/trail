#include <stdio.h>
#include <string.h>

#include "lwip/err.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/opt.h"

#if defined (__arm__) || defined (__aarch64__)
#include "xil_printf.h"
#include "xil_types.h"
#include "xil_cache.h"
#include "xil_io.h"     // For XTime_GetTime
#include "xtime_l.h"    // For XTime
#include "xparameters.h" // For XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ or similar
#else
#define xil_printf printf
#endif

// Configuration for video buffer and network
#define MAX_VIDEO_BUFFER_SIZE (1024 * 1024 * 100) // 100 MB max video
#define DDR4_VIDEO_BUFFER_START_ADDR 0x10000000 // Ensure this address is valid and accessible

// Global variables for single active connection's state
static struct tcp_pcb *active_pcb_global = NULL;
static char *video_storage_buffer_global = (char *)DDR4_VIDEO_BUFFER_START_ADDR;
static u32_t current_buffer_offset_global = 0;

static u32_t expected_total_video_size_global = 0;
static int is_header_processed_global = 0;
static u8_t header_byte_collection_buffer_global[4];
static u8_t header_bytes_in_buffer_global = 0;

static u32_t total_received_data_len_global = 0;
static u32_t total_echoed_data_len_global = 0;

// Variables for rate calculation
static XTime last_report_time_ticks = 0;
static u32_t last_reported_received_bytes = 0;
static u32_t last_reported_echoed_bytes = 0;
#define REPORT_INTERVAL_MS 1000 // Report rates every 1000 milliseconds (1 second)

// Function prototypes
err_t recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
err_t accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err);
void video_echo_server_init(void);

// Helper to reset all global state variables for a new connection
static void reset_global_state(void) {
    active_pcb_global = NULL;
    current_buffer_offset_global = 0;
    expected_total_video_size_global = 0;
    is_header_processed_global = 0;
    header_bytes_in_buffer_global = 0;
    total_received_data_len_global = 0;
    total_echoed_data_len_global = 0;
    memset(header_byte_collection_buffer_global, 0, 4);

    // Reset rate calculation variables as well for a fresh start
    last_report_time_ticks = 0;
    last_reported_received_bytes = 0;
    last_reported_echoed_bytes = 0;
}

void print_app_header() {
#if (LWIP_IPV6==0)
    xil_printf("\n\r\n\r-----lwIP TCP video echo server ------\n\r");
#else
    xil_printf("\n\r\n\r-----lwIPv6 TCP video echo server ------\n\r");
#endif
    xil_printf("TCP packets sent to port 6001 will be echoed back\n\r");
}

err_t recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    LWIP_UNUSED_ARG(arg);

    if (!p || err != ERR_OK) {
        if (!p) {
            xil_printf("SERVER: Connection closed by client.\n\r");
        } else {
            xil_printf("SERVER: Receive error: %d.\n\r", err);
            pbuf_free(p);
        }
        tcp_close(tpcb);
        tcp_recv(tpcb, NULL);
        reset_global_state();
        return ERR_OK;
    }

    tcp_recved(tpcb, p->tot_len);

    u16_t current_pbuf_data_len = p->tot_len;
    char *pbuf_current_ptr = (char *)p->payload;

    // 1. Header Processing
    if (!is_header_processed_global) {
        u16_t bytes_needed = 4 - header_bytes_in_buffer_global;
        u16_t bytes_from_this_pbuf = LWIP_MIN(current_pbuf_data_len, bytes_needed);

        memcpy(header_byte_collection_buffer_global + header_bytes_in_buffer_global,
               pbuf_current_ptr, bytes_from_this_pbuf);
        header_bytes_in_buffer_global += bytes_from_this_pbuf;

        pbuf_current_ptr += bytes_from_this_pbuf;
        current_pbuf_data_len -= bytes_from_this_pbuf;

        if (header_bytes_in_buffer_global == 4) {
            is_header_processed_global = 1;
            expected_total_video_size_global = (u32_t)header_byte_collection_buffer_global[0] << 24 |
                                               (u32_t)header_byte_collection_buffer_global[1] << 16 |
                                               (u32_t)header_byte_collection_buffer_global[2] << 8  |
                                               (u32_t)header_byte_collection_buffer_global[3];

            xil_printf("SERVER: Header processed. Expected video size: %lu bytes.\n\r", (unsigned long)expected_total_video_size_global);

            if (expected_total_video_size_global == 0 || expected_total_video_size_global > MAX_VIDEO_BUFFER_SIZE) {
                xil_printf("SERVER: ERROR: Invalid video size (%lu). Max allowed: %lu. Closing.\n\r",
                           (unsigned long)expected_total_video_size_global, (unsigned long)MAX_VIDEO_BUFFER_SIZE);
                pbuf_free(p);
                tcp_close(tpcb);
                tcp_recv(tpcb, NULL);
                reset_global_state();
                return ERR_ABRT;
            }
            // Initialize timer for rate calculation after header is received
            XTime_GetTime(&last_report_time_ticks);
            last_reported_received_bytes = 0;
            last_reported_echoed_bytes = 0;
        } else {
            pbuf_free(p);
            return ERR_OK;
        }
    }

    // 2. Video Data Processing and Echoing
    if (is_header_processed_global && current_pbuf_data_len > 0) {
        u16_t copy_len = LWIP_MIN(current_pbuf_data_len,
                                  (u16_t)(expected_total_video_size_global - total_received_data_len_global));
        copy_len = LWIP_MIN(copy_len,
                                  (u16_t)(MAX_VIDEO_BUFFER_SIZE - current_buffer_offset_global));

        if (copy_len > 0) {
            memcpy(video_storage_buffer_global + current_buffer_offset_global, pbuf_current_ptr, copy_len);

            #if defined (__arm__) || defined (__aarch64__)
            Xil_DCacheFlushRange((UINTPTR)(video_storage_buffer_global + current_buffer_offset_global), copy_len);
            #endif

            current_buffer_offset_global += copy_len;
            total_received_data_len_global += copy_len;

            err_t write_err = tcp_write(tpcb, pbuf_current_ptr, copy_len, TCP_WRITE_FLAG_COPY);
            if (write_err == ERR_OK) {
                total_echoed_data_len_global += copy_len;
                tcp_output(tpcb);
            } else if (write_err == ERR_MEM) {
                xil_printf("SERVER: tcp_write (echo) failed, ERR_MEM. Send buffer full. Echo might be incomplete.\n\r");
            } else {
                xil_printf("SERVER: tcp_write (echo) error: %d\n\r", write_err);
                pbuf_free(p);
                tcp_close(tpcb);
                tcp_recv(tpcb, NULL);
                reset_global_state();
                return write_err;
            }
        } else {
            if (total_received_data_len_global >= expected_total_video_size_global) {
                xil_printf("SERVER: Video complete. Discarding extra data.\n\r");
            } else {
                xil_printf("SERVER: DDR4 buffer full or video size mismatch. Closing.\n\r");
                pbuf_free(p);
                tcp_close(tpcb);
                tcp_recv(tpcb, NULL);
                reset_global_state();
                return ERR_ABRT;
            }
        }
    }

    // Rate Calculation and Reporting
    #if defined (__arm__) || defined (__aarch64__)
    XTime current_time_ticks;
    XTime_GetTime(&current_time_ticks);
    
    // Convert ticks to milliseconds for comparison
    double delta_time_ms = (double)(current_time_ticks - last_report_time_ticks) * 1000.0 / XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ;

    if (delta_time_ms >= REPORT_INTERVAL_MS) {
        double delta_time_s = delta_time_ms / 1000.0;
        
        u32_t delta_received_bytes = total_received_data_len_global - last_reported_received_bytes;
        u32_t delta_echoed_bytes = total_echoed_data_len_global - last_reported_echoed_bytes;

        double recv_rate_kbps = (delta_received_bytes * 8.0) / (delta_time_s * 1000.0); // Bytes to Kbits per second
        double send_rate_kbps = (delta_echoed_bytes * 8.0) / (delta_time_s * 1000.0); // Bytes to Kbits per second

        xil_printf("SERVER: Recv Rate: %.2f Kbps, Send Rate: %.2f Kbps (Total Recv: %lu, Total Echoed: %lu)\n\r",
                   recv_rate_kbps, send_rate_kbps,
                   (unsigned long)total_received_data_len_global, (unsigned long)total_echoed_data_len_global);

        // Update for next interval
        last_report_time_ticks = current_time_ticks;
        last_reported_received_bytes = total_received_data_len_global;
        last_reported_echoed_bytes = total_echoed_data_len_global;
    }
    #endif

    // Check if total video is received and echoed
    if (total_received_data_len_global == expected_total_video_size_global &&
        total_echoed_data_len_global == expected_total_video_size_global &&
        is_header_processed_global) {
        xil_printf("SERVER: All %lu bytes of video received and echoed. Closing connection.\n\r",
                   (unsigned long)total_echoed_data_len_global);
        tcp_close(tpcb);
        tcp_recv(tpcb, NULL);
        reset_global_state();
    }
    
    pbuf_free(p);
    return ERR_OK;
}

err_t accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err) {
    LWIP_UNUSED_ARG(arg);

    if (err != ERR_OK) {
        xil_printf("SERVER: Accept callback error: %d\n\r", err);
        return err;
    }

    if (active_pcb_global != NULL) {
        xil_printf("SERVER: Connection rejected: server busy. Active PCB: %lu.\n\r", (UINTPTR)active_pcb_global);
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    active_pcb_global = newpcb;
    reset_global_state(); // This resets rate counters for the new connection

    tcp_recv(newpcb, recv_callback);
    tcp_arg(newpcb, NULL); 
    tcp_set_recv_wnd(newpcb, TCP_WND);

    xil_printf("SERVER: Accepted new connection (PCB: %lu). Waiting for 4-byte header...\n\r", (UINTPTR)newpcb);

    return ERR_OK;
}

void echo_server_init(void) {
    struct tcp_pcb *pcb;
    err_t err;
    unsigned port = 6001;

    pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        xil_printf("SERVER: Error creating PCB. Out of Memory\n\r");
        return;
    }

    err = tcp_bind(pcb, IP_ANY_TYPE, port);
    if (err != ERR_OK) {
        xil_printf("SERVER: Unable to bind to port %d: err = %d\n\r", port, err);
        tcp_abort(pcb);
        return;
    }

    struct tcp_pcb *listen_pcb = tcp_listen(pcb);
    if (!listen_pcb) {
        xil_printf("SERVER: Out of memory while tcp_listen\n\r");
        return;
    }

    tcp_accept(listen_pcb, accept_callback);

    xil_printf("SERVER: TCP video echo server started @ port %d\n\r", port);
    xil_printf("SERVER: DDR4 Video Buffer Address: 0x%08lX, Max Buffer Size: %lu bytes\n\r",
               (UINTPTR)DDR4_VIDEO_BUFFER_START_ADDR, (unsigned long)MAX_VIDEO_BUFFER_SIZE);
    xil_printf("lwipopts.h: TCP_MSS = %d\n\r", TCP_MSS);
    xil_printf("lwipopts.h: TCP_SND_BUF = %ld\n\r", (long)TCP_SND_BUF);
    xil_printf("lwipopts.h: TCP_WND = %ld\n\r", (long)TCP_WND);
    xil_printf("lwipopts.h: PBUF_POOL_SIZE = %d\n\r", PBUF_POOL_SIZE);
    xil_printf("lwipopts.h: PBUF_POOL_BUFSIZE = %d\n\r", PBUF_POOL_BUFSIZE);
    xil_printf("lwipopts.h: MEM_SIZE = %ld\n\r", (long)MEM_SIZE);
    xil_printf("lwipopts.h: LWIP_WND_SCALE = %d\n\r", LWIP_WND_SCALE);
}

int transfer_data() {
    return 0;
}
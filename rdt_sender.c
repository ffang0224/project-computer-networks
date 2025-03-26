#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <math.h>  // for floor function

#include"packet.h"
#include"common.h"

#define STDIN_FD    0
#define INITIAL_RTO 3000 // 3 seconds in milliseconds
#define MAX_RTO 240000   // 240 seconds in milliseconds

// Congestion control states
#define SLOW_START 0
#define CONGESTION_AVOIDANCE 1
#define FAST_RETRANSMIT 2

// Function prototypes
void resend_packets(int sig);
void start_timer();
void stop_timer();
void init_timer(int delay, void (*sig_handler)(int));
void update_rtt(int measured_rtt_ms);
void log_cwnd();
long get_current_time_ms();
int is_window_full();
int get_window_index(int seqno);
void init_window_buffer(int size);
void free_window_buffer(int index);
void resize_window_buffer();
void store_packet(tcp_packet *pkt, int index, int size);

int next_seqno=0;
int send_base=0;
float cwnd = 1.0;          // Congestion window size (in packets)
int ssthresh = 64;         // Slow start threshold (in packets)
int cc_state = SLOW_START; // Congestion control state
int dup_acks = 0;          // Count of duplicate ACKs
int last_ack = 0;          // Last ACK received
int packets_sent = 0;      // Count of packets sent in current window

// RTT estimation variables
int rtt_measured = 0;            // Whether we've measured an RTT sample
float estimated_rtt = 0;         // Estimated RTT
float dev_rtt = 0;               // Deviation RTT
int rto = INITIAL_RTO;           // Current RTO value in milliseconds
int consecutive_timeouts = 0;    // Count of consecutive timeouts for exponential backoff
struct timeval send_time;        // Time when packet was sent

// Window management
tcp_packet **window_buffer;      // Buffer to store sent packets for retransmission
int *packet_sent_time;           // Time when each packet was sent
int *packet_size;                // Size of each packet
int window_size;                 // Current maximum window size

// CWND tracking file
FILE *cwnd_file = NULL;
struct timeval start_time;       // Program start time

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;       

// Get current time in milliseconds since program start
long get_current_time_ms() {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - start_time.tv_sec) * 1000 + 
           (now.tv_usec - start_time.tv_usec) / 1000;
}

// Log CWND changes to file
void log_cwnd() {
    if (cwnd_file) {
        fprintf(cwnd_file, "%ld,%f\n", get_current_time_ms(), cwnd);
        fflush(cwnd_file);
    }
}

// Function to check if window is full
int is_window_full() {
    return packets_sent >= (int)floor(cwnd);
}

// Function to get window index for a sequence number
int get_window_index(int seqno) {
    return (seqno / DATA_SIZE) % (int)ceil(cwnd);
}

// Initialize the window buffer
void init_window_buffer(int size) {
    window_size = size;
    window_buffer = (tcp_packet **)malloc(size * sizeof(tcp_packet *));
    packet_sent_time = (int *)malloc(size * sizeof(int));
    packet_size = (int *)malloc(size * sizeof(int));
    
    for (int i = 0; i < size; i++) {
        window_buffer[i] = NULL;
        packet_sent_time[i] = 0;
        packet_size[i] = 0;
    }
}

// Function to free window buffer at a specific index
void free_window_buffer(int index) {
    if (window_buffer[index] != NULL) {
        free(window_buffer[index]);
        window_buffer[index] = NULL;
        packet_sent_time[index] = 0;
        packet_size[index] = 0;
    }
}

// Resize window buffer if needed
void resize_window_buffer() {
    int new_size = (int)ceil(cwnd) * 2;  // Double the size for safety
    if (new_size > window_size) {
        tcp_packet **new_buffer = (tcp_packet **)malloc(new_size * sizeof(tcp_packet *));
        int *new_sent_time = (int *)malloc(new_size * sizeof(int));
        int *new_packet_size = (int *)malloc(new_size * sizeof(int));
        
        // Copy existing data
        for (int i = 0; i < window_size; i++) {
            new_buffer[i] = window_buffer[i];
            new_sent_time[i] = packet_sent_time[i];
            new_packet_size[i] = packet_size[i];
        }
        
        // Initialize new slots
        for (int i = window_size; i < new_size; i++) {
            new_buffer[i] = NULL;
            new_sent_time[i] = 0;
            new_packet_size[i] = 0;
        }
        
        // Free old arrays and update pointers
        free(window_buffer);
        free(packet_sent_time);
        free(packet_size);
        
        window_buffer = new_buffer;
        packet_sent_time = new_sent_time;
        packet_size = new_packet_size;
        window_size = new_size;
    }
}

// Store a packet in the window buffer
void store_packet(tcp_packet *pkt, int index, int size) {
    if (index >= window_size) {
        resize_window_buffer();
    }
    
    free_window_buffer(index);
    
    window_buffer[index] = (tcp_packet *)malloc(TCP_HDR_SIZE + size);
    memcpy(window_buffer[index], pkt, TCP_HDR_SIZE + size);
    packet_size[index] = size;
    packet_sent_time[index] = get_current_time_ms();
}

void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, sig_handler);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}

void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        // Timeout occurred
        VLOG(INFO, "Timeout happened");
        
        // Implement exponential backoff
        consecutive_timeouts++;
        rto = rto * (1 << consecutive_timeouts); // Double RTO for each consecutive timeout
        if (rto > MAX_RTO) {
            rto = MAX_RTO;
        }
        
        // Update timer with new RTO
        init_timer(rto, resend_packets);
        
        // Congestion control actions on timeout
        ssthresh = (int)fmax(cwnd / 2, 2);
        cwnd = 1.0;
        cc_state = SLOW_START;
        packets_sent = 0;
        log_cwnd();
        
        VLOG(DEBUG, "Timeout: CWND = %.2f, ssthresh = %d", cwnd, ssthresh);
        
        // Retransmit the lost packet (first unacknowledged packet)
        int index = get_window_index(send_base);
        if (window_buffer[index] != NULL) {
            sndpkt = window_buffer[index];
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + packet_size[index], 0, 
                    (const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }
            
            VLOG(DEBUG, "Resending packet %d to %s with %d bytes (timeout)", 
                send_base, inet_ntoa(serveraddr.sin_addr), packet_size[index]);
        }
    }
}

// Update RTT estimation (RFC 6298)
void update_rtt(int measured_rtt_ms) {
    if (!rtt_measured) {
        // First measurement
        estimated_rtt = measured_rtt_ms;
        dev_rtt = measured_rtt_ms / 2.0;
        rtt_measured = 1;
    } else {
        // Update estimates (alpha = 0.125, beta = 0.25)
        dev_rtt = (1 - 0.25) * dev_rtt + 0.25 * fabs(measured_rtt_ms - estimated_rtt);
        estimated_rtt = (1 - 0.125) * estimated_rtt + 0.125 * measured_rtt_ms;
    }
    
    // Set RTO (K = 4)
    rto = (int)(estimated_rtt + 4 * dev_rtt);
    
    // Ensure minimum of 1 second (RFC 6298)
    if (rto < 1000) {
        rto = 1000;
    }
    
    VLOG(DEBUG, "RTT measured: %d ms, Estimated RTT: %.2f ms, Dev RTT: %.2f ms, RTO: %d ms", 
         measured_rtt_ms, estimated_rtt, dev_rtt, rto);
    
    // Reset consecutive timeouts since we got an ACK
    consecutive_timeouts = 0;
    
    // Update timer with new RTO
    init_timer(rto, resend_packets);
}

int main (int argc, char **argv)
{
    int portno, len;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    // Open CWND tracking file
    cwnd_file = fopen("CWND.csv", "w");
    if (cwnd_file == NULL) {
        error("Cannot open CWND.csv for writing");
    }
    fprintf(cwnd_file, "time,cwnd\n"); // CSV header
    
    // Record start time
    gettimeofday(&start_time, NULL);
    
    // Log initial CWND
    log_cwnd();

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* initialize server server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    // Initialize window buffer
    init_window_buffer(128);  // Start with a reasonable size
    
    // Initialize timer with initial RTO
    init_timer(INITIAL_RTO, resend_packets);
    
    next_seqno = 0;
    send_base = 0;
    
    while (1)
    {
        // Send data as allowed by congestion window
        while (!is_window_full()) {
            len = fread(buffer, 1, DATA_SIZE, fp);
            if (len <= 0) {
                // End of file reached
                if (packets_sent == 0) {
                    // All packets have been acknowledged, can exit
                    VLOG(INFO, "End Of File has been reached");
                    sndpkt = make_packet(0);
                    sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                           (const struct sockaddr *)&serveraddr, serverlen);
                    free(sndpkt);
                    fclose(cwnd_file); // Close CWND tracking file
                    
                    // Free window buffer
                    for (int i = 0; i < window_size; i++) {
                        free_window_buffer(i);
                    }
                    free(window_buffer);
                    free(packet_sent_time);
                    free(packet_size);
                    
                    return 0;
                }
                break; // Wait for ACKs before sending EOF
            }
            
            // Create packet
            sndpkt = make_packet(len);
            memcpy(sndpkt->data, buffer, len);
            sndpkt->hdr.seqno = next_seqno;
            
            // Store packet in window buffer
            int window_idx = get_window_index(next_seqno);
            store_packet(sndpkt, window_idx, len);
            
            // Send the packet
            VLOG(DEBUG, "Sending packet %d with %d bytes, CWND = %.2f", 
                 next_seqno, len, cwnd);
            
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + len, 0, 
                      (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
            
            // Start timer if this is the first packet in the window
            if (packets_sent == 0) {
                start_timer();
            }
            
            // Update sequence number and packet count
            next_seqno += len;
            packets_sent++;
            
            free(sndpkt); // Free the packet after sending
        }
        
        // Wait for ACKs
        if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                   (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0) {
            error("recvfrom");
        }
        
        recvpkt = (tcp_packet *)buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        
        VLOG(DEBUG, "Received ACK %d", recvpkt->hdr.ackno);
        
        // Check if this is a new ACK
        if (recvpkt->hdr.ackno > send_base) {
            // New ACK received
            
            // Calculate RTT if this ACK acknowledges the packet we're timing
            int window_idx = get_window_index(send_base);
            if (window_buffer[window_idx] != NULL && recvpkt->hdr.ackno > send_base) {
                int send_time_ms = packet_sent_time[window_idx];
                if (send_time_ms > 0) {
                    int current_time_ms = get_current_time_ms();
                    int measured_rtt = current_time_ms - send_time_ms;
                    update_rtt(measured_rtt);
                }
            }
            
            // Free acknowledged packets
            while (send_base < recvpkt->hdr.ackno) {
                int idx = get_window_index(send_base);
                free_window_buffer(idx);
                
                // Calculate the size of this packet to increment send_base correctly
                int pkt_size = DATA_SIZE; // Default if we don't know the size
                if (packet_size[idx] > 0) {
                    pkt_size = packet_size[idx];
                }
                send_base += pkt_size;
                packets_sent--;
            }
            
            // Update congestion window based on current state
            if (cc_state == SLOW_START) {
                // In slow start, increment CWND by 1 for each ACK
                cwnd += 1.0;
                VLOG(DEBUG, "Slow start: Increasing CWND to %.2f", cwnd);
                
                // Check if we should transition to congestion avoidance
                if (cwnd >= ssthresh) {
                    cc_state = CONGESTION_AVOIDANCE;
                    VLOG(DEBUG, "Transitioning to Congestion Avoidance");
                }
            } else if (cc_state == CONGESTION_AVOIDANCE) {
                // In congestion avoidance, increase CWND by 1/CWND for each ACK
                cwnd += 1.0 / cwnd;
                VLOG(DEBUG, "Congestion avoidance: Increasing CWND to %.2f", cwnd);
            }
            
            // Log CWND change
            log_cwnd();
            
            // Reset duplicate ACK count
            dup_acks = 0;
            last_ack = recvpkt->hdr.ackno;
            
            // Restart timer if there are still unacknowledged packets
            if (packets_sent > 0) {
                stop_timer();
                start_timer();
            } else {
                stop_timer(); // All packets acknowledged
            }
        } else if (recvpkt->hdr.ackno == last_ack) {
            // Duplicate ACK
            dup_acks++;
            VLOG(DEBUG, "Duplicate ACK %d received (%d)", recvpkt->hdr.ackno, dup_acks);
            
            // Fast retransmit after 3 duplicate ACKs
            if (dup_acks == 3) {
                VLOG(INFO, "Fast retransmit triggered");
                
                // Congestion control actions
                ssthresh = (int)fmax(cwnd / 2, 2);
                cwnd = 1.0;
                cc_state = SLOW_START;
                log_cwnd();
                
                VLOG(DEBUG, "Fast retransmit: CWND = %.2f, ssthresh = %d", cwnd, ssthresh);
                
                // Retransmit the lost packet
                int index = get_window_index(send_base);
                if (window_buffer[index] != NULL) {
                    sndpkt = window_buffer[index];
                    if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + packet_size[index], 0, 
                            (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                        error("sendto");
                    }
                    
                    VLOG(DEBUG, "Resending packet %d with %d bytes (fast retransmit)", 
                         send_base, packet_size[index]);
                }
                
                // Reset duplicate ACK count
                dup_acks = 0;
                
                // Restart timer
                stop_timer();
                start_timer();
            }
        }
    }

    return 0;
}




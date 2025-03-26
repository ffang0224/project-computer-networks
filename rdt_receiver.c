#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>

#include "common.h"
#include "packet.h"

/*
 * You are required to change the implementation to support
 * window size greater than one.
 * In the current implementation the window size is one, hence we have
 * only one send and receive packet
 */
#define WINDOW_SIZE 10
#define MAX_SEQ_NO 256000  // Large enough sequence number space

typedef struct {
    int received;        // Whether this packet has been received
    tcp_packet *packet;  // The actual packet
} packet_buffer;

packet_buffer recv_buffer[WINDOW_SIZE];  // Buffer for out-of-order packets
tcp_packet *sndpkt;
tcp_packet *recvpkt;  // Added declaration for recvpkt
int next_expected_seqno = 0;  // Next expected sequence number
int receiver_window_size = WINDOW_SIZE;

// File for throughput data
FILE *throughput_fp = NULL;

// Initialize the packet buffer
void init_packet_buffer() {
    for (int i = 0; i < WINDOW_SIZE; i++) {
        recv_buffer[i].received = 0;
        recv_buffer[i].packet = NULL;
    }
}

// Function to get window index for a sequence number
int get_window_index(int seqno) {
    return ((seqno - next_expected_seqno) / DATA_SIZE) % WINDOW_SIZE;
}

// Free a packet in the buffer
void free_packet_buffer(int index) {
    if (recv_buffer[index].packet != NULL) {
        free(recv_buffer[index].packet);
        recv_buffer[index].packet = NULL;
    }
    recv_buffer[index].received = 0;
}

// Write contiguous packets to file
void write_contiguous_packets(FILE *fp) {
    int window_index = 0;
    
    // Write all contiguous packets from the buffer
    while (window_index < WINDOW_SIZE && recv_buffer[window_index].received) {
        tcp_packet *pkt = recv_buffer[window_index].packet;
        
        // Write packet data to file
        fseek(fp, pkt->hdr.seqno, SEEK_SET);
        int bytes_written = fwrite(pkt->data, 1, pkt->hdr.data_size, fp);
        VLOG(DEBUG, "Wrote %d bytes at position %d to file", bytes_written, pkt->hdr.seqno);
        fflush(fp);
        
        // Update next expected sequence number
        next_expected_seqno = pkt->hdr.seqno + pkt->hdr.data_size;
        
        // Free this packet
        free_packet_buffer(window_index);
        
        // Shift the window
        for (int i = 0; i < WINDOW_SIZE-1; i++) {
            recv_buffer[i] = recv_buffer[i+1];
        }
        
        // Clear the last slot
        recv_buffer[WINDOW_SIZE-1].received = 0;
        recv_buffer[WINDOW_SIZE-1].packet = NULL;
    }
}

int main(int argc, char **argv) {
    int sockfd; /* socket */
    int portno; /* port to listen on */
    int clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval; /* flag value for setsockopt */
    FILE *fp;
    char buffer[MSS_SIZE];
    struct timeval tp;

    /* 
     * check command line arguments 
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }
    
    // Open throughput data file
    throughput_fp = fopen("throughput_data.txt", "w");
    if (throughput_fp == NULL) {
        error("Cannot open throughput_data.txt");
    }
    
    // Write header for throughput data
    fprintf(throughput_fp, "epoch time, bytes received, sequence number\n");
    fflush(throughput_fp);

    /* 
     * socket: create the parent socket 
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* setsockopt: Handy debugging trick that lets 
     * us rerun the server immediately after we kill it; 
     * otherwise we have to wait about 20 secs. 
     * Eliminates "ERROR on binding: Address already in use" error. 
     */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    /* 
     * bind: associate the parent socket with a port 
     */
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    /* 
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    clientlen = sizeof(clientaddr);
    init_packet_buffer();  // Initialize the packet buffer
    
    while (1) {
        /*
         * recvfrom: receive a UDP datagram from a client
         */
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom");
        }
        
        recvpkt = (tcp_packet *) buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        
        // Check if this is the EOF packet
        if (recvpkt->hdr.data_size == 0) {
            VLOG(INFO, "End Of File has been reached");
            fclose(fp);
            fclose(throughput_fp); // Close throughput data file
            break;
        }
        
        gettimeofday(&tp, NULL);
        
        // Log throughput data to both console and file
        VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);
        
        // Write to throughput data file
        fprintf(throughput_fp, "%lu, %d, %d\n", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);
        fflush(throughput_fp);
        
        // Calculate if this packet is within our window
        if (recvpkt->hdr.seqno >= next_expected_seqno && 
            recvpkt->hdr.seqno < next_expected_seqno + receiver_window_size * DATA_SIZE) {
            
            // Calculate the window index for this packet
            int window_index = get_window_index(recvpkt->hdr.seqno);
            
            // If index is within the window size
            if (window_index < WINDOW_SIZE) {
                // Save the packet in our buffer if we haven't received it yet
                if (!recv_buffer[window_index].received) {
                    recv_buffer[window_index].packet = (tcp_packet *)malloc(TCP_HDR_SIZE + recvpkt->hdr.data_size);
                    memcpy(recv_buffer[window_index].packet, recvpkt, TCP_HDR_SIZE + recvpkt->hdr.data_size);
                    recv_buffer[window_index].received = 1;
                    VLOG(DEBUG, "Stored packet with seqno %d at window index %d, data_size: %d", 
                         recvpkt->hdr.seqno, window_index, recvpkt->hdr.data_size);
                    
                    // If this is the next expected packet, write contiguous packets
                    if (window_index == 0) {
                        write_contiguous_packets(fp);
                    }
                }
            }
        }
        
        /* 
         * sendto: ACK back to the client 
         */
        sndpkt = make_packet(0);
        sndpkt->hdr.ackno = next_expected_seqno;
        sndpkt->hdr.ctr_flags = ACK;
        
        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                (struct sockaddr *) &clientaddr, clientlen) < 0) {
            error("ERROR in sendto");
        }
        
        free(sndpkt);
    }
    
    // Cleanup any remaining packets in the buffer
    for (int i = 0; i < WINDOW_SIZE; i++) {
        free_packet_buffer(i);
    }

    return 0;
}

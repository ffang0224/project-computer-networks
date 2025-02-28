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
// #include "packet.h"
#include "window.h"

/*
 * You are required to change the implementation to support
 * window size greater than one.
 * In the current implementation the window size is one, hence we have
 * only one send and receive packet
 */
tcp_packet *recvpkt;
tcp_packet *sndpkt;

// Sliding window variables
window *recv_window;
int window_size = 10;  // Maximum number of packets that can be buffered
int expected_seqno = 0;  // Sequence number of the next in-order packet we expect to receive
int highest_seqno_received = -1;  // Tracks the highest sequence number we've seen so far

// Write packets to file in sequential order
void write_in_order(FILE *fp) {
    // Cast the void pointer to tcp_packet pointer array for easier access
    tcp_packet **buffer = (tcp_packet **) recv_window->buffer_ptr;
    
    // Keep writing packets as long as we find sequential ones in the buffer
    while (1) {
        int found = 0;  // Flag to track if we found the next expected packet
        for (unsigned int i = 0; i < recv_window->window_size; i++) {
            // Check if this slot has the packet we're looking for
            if (buffer[i] != NULL && buffer[i]->hdr.seqno == expected_seqno) {
                // Write the packet's data to the file (no seek needed as we write in order)
                fwrite(buffer[i]->data, 1, buffer[i]->hdr.data_size, fp);
                
                // Move the window forward by the size of data we just wrote
                expected_seqno += buffer[i]->hdr.data_size;
                
                // Free the buffer slot for new packets
                remove_packet_from_buffer(buffer[i]->hdr.seqno);
                found = 1;
                break;
            }
        }
        // Exit if we can't find the next packet in sequence
        if (!found) break;
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

    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

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
     * Initialize sliding window
     */
    recv_window = set_window(window_size);
    if (recv_window == NULL) {
        error("ERROR initializing receiver window");
    }

    /* 
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");
    printf("Receiver started. Listening on port %d\n", portno);

    clientlen = sizeof(clientaddr);
    while (1) {
        VLOG(DEBUG, "waiting from server\n");
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom");
        }
        
        recvpkt = (tcp_packet *) buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);

        VLOG(DEBUG, "Received packet with seqno %d, size %d", 
            recvpkt->hdr.seqno, recvpkt->hdr.data_size);

        if (recvpkt->hdr.data_size == 0) {
            VLOG(INFO, "End Of File has been reached");
            fclose(fp);
            break;
        }

        gettimeofday(&tp, NULL);
        VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);

        // Check if this packet falls within our current window range
        if (recvpkt->hdr.seqno >= expected_seqno && 
            recvpkt->hdr.seqno < expected_seqno + (window_size * DATA_SIZE)) {
            
            // Only store the packet if we have space in our buffer
            if (buffer_full(recv_window) == -1) {  // -1 means buffer is not full
                // Create a copy of the packet for storage
                tcp_packet *pkt_copy = make_packet(recvpkt->hdr.data_size);
                memcpy(pkt_copy, recvpkt, TCP_HDR_SIZE + recvpkt->hdr.data_size);
                
                // Store this packet in our receive window for potential out-of-order handling
                add_packet_to_buffer(pkt_copy);
                
                // Update our highest received sequence number if this packet is newer
                if (recvpkt->hdr.seqno > highest_seqno_received) {
                    highest_seqno_received = recvpkt->hdr.seqno;
                }
                
                // Attempt to write any sequential packets we now have to the file
                write_in_order(fp);
            }
        }

        // Create an ACK packet for the sender
        sndpkt = make_packet(0);
        // Tell sender which sequence number we expect next
        sndpkt->hdr.ackno = expected_seqno;
        // Mark this as an ACK packet
        sndpkt->hdr.ctr_flags = ACK;
        
        VLOG(DEBUG, "Sending ACK for seqno %d", expected_seqno);
        
        // Send the ACK back to the client
        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                (struct sockaddr *) &clientaddr, clientlen) < 0) {
            error("ERROR in sendto");
        }
        
        free(sndpkt);  // Free the ACK packet after sending
    }

    return 0;
}

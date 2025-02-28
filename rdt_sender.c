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

// #include"packet.h" // debug: packet.h is included in window.h !!! (rdt_receiver.c should also not include packet.h)
#include"common.h"
#include"window.h"

#define STDIN_FD    0
#define RETRY  120 //millisecond

int next_seqno=0;
int send_base=0; // seqno of 
int window_size = 10;

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
tcp_packet *resending_pkt_ptr; // pointer to packet that needs to be resent; used in resend_packets()
sigset_t sigmask;       
window *sliding_window;

/*

// debug: add comments

*/
void resend_packets(int sig) // issue 4
{
    if (sig == SIGALRM)
    {
        VLOG(INFO, "Timeout happend");
        if(sendto(sockfd, resending_pkt_ptr, TCP_HDR_SIZE + get_data_size(resending_pkt_ptr), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }
            
    }
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


int main (int argc, char **argv)
{
    int portno, len;
    int next_seqno, base_of_packet; // base_of_packet = beginning byte of a single packet
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

    /* Set sliding window */
    sliding_window = set_window(window_size);

    //Go-Back-N Protocol

    init_timer(RETRY, resend_packets); // issue 3
    next_seqno = 0;
    while (1) // iterates every time send_base is updated (send_base should NOT be changed except for sliding the window)
    {
        base_of_packet = send_base;
        len = fread(buffer, 1, DATA_SIZE, fp);

        /* check whether End of File has been reached */
        if ( len <= 0)
        {
            VLOG(INFO, "End Of File has been reached");
            sndpkt = make_packet(0);
            sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                    (const struct sockaddr *)&serveraddr, serverlen);
            break;
        }

        /* repeat making a packet until the window buffer is full;
           at first RTT, this will repeat 10 times, which is the window size
        */
        while(buffer_full(sliding_window)!=-1)
        {
            // make 1 packet
            base_of_packet = next_seqno;
            next_seqno = base_of_packet + len;
            sndpkt = make_packet(len);          // debug: will the previous packet get overwritten safely?
                                                // malloc is in make_packet(), not in rdt_sender.c
                                                // so, free(sndpkt) should be called making the next packet
            memcpy(sndpkt->data, buffer, len);
            sndpkt->hdr.seqno = base_of_packet;

            add_packet_to_buffer(sndpkt); // add the whole packet to window buffer;
                                          // when a packet needs to be retransmitted, sender uses data in packet_in_buffer->hdr.seqno

            VLOG(DEBUG, "Sending packet %d to %s", 
                base_of_packet, inet_ntoa(serveraddr.sin_addr));
            /*
            * If the sendto is called for the first time, the system will
            * will assign a random port number so that server can send its
            * response to the src port.
            */
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }

            free(sndpkt); // each make_packet() call does new malloc; 
                          // so, free the old packet before making the next packet;
                          // resend_packets() is modified, so that it uses resending_pkt_ptr instead of the sndpkt variable
        }

        //Wait for ACK
        do {
            start_timer();
            //ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
            //struct sockaddr *src_addr, socklen_t *addrlen);


            resending_pkt_ptr = return_packet_of_smallest_seqno(); // In retransmission, 
                                                                   // resend_packets() should send the packet with smallest seq number in the buffer

            // Update ACK
            // issue 5
            do      
            {
                if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                            (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
                {
                    error("recvfrom");
                }

                recvpkt = (tcp_packet *)buffer;
                printf("%d \n", get_data_size(recvpkt));
                assert(get_data_size(recvpkt) <= DATA_SIZE);
            }while(recvpkt->hdr.ackno < next_seqno);    //ignore duplicate ACKs; issue 9
            stop_timer();
            /*resend pack if don't recv ACK */
        } while(recvpkt->hdr.ackno != next_seqno);
    }

    return 0;

}




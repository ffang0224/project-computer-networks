#include"packet.h"

typedef struct {
    unsigned int window_size;
    // unsigned int buffer[window_size];
    void* buffer_ptr;
    unsigned int smallest_seqno_idx; // index in the buffer array at which the packet of smallest sequence number is stored
} window;

window* set_window(unsigned int window_size); // sets the window according to the window size given
int buffer_full(window *window);              // checks whether the buffer is full; returns 1 if full, -1 otherwise
void add_packet_to_buffer(tcp_packet *pkt);         // adds the packet to the buffer at smallest index

// int find_buffer_space(window *window);        // checks whether the buffer has space left, and returns the smallest index of empty slot in the buffer array;
//                                               // if buffer is full, return -1
#include <stdlib.h>
#include <stdio.h>
#include "window.h"

// Global window reference since add_packet_to_buffer doesn't take a window parameter
static window *global_window = NULL;

window* set_window(unsigned int window_size)
{
    // Allocate memory for the window structure
    window *w = (window *) malloc(sizeof(window));
    if (w == NULL) {
        return NULL;  // Memory allocation failed
    }
    
    w->window_size = window_size;
    
    // Allocate memory for the buffer to hold packet pointers
    // We use void* in the struct but cast it to tcp_packet** for usage
    w->buffer_ptr = malloc(window_size * sizeof(tcp_packet *));
    if (w->buffer_ptr == NULL) {
        free(w);  // Clean up the window structure if buffer allocation fails
        return NULL;
    }
    
    // Initialize all slots in buffer to NULL to indicate they're empty
    tcp_packet **buffer = (tcp_packet **) w->buffer_ptr;
    for (unsigned int i = 0; i < window_size; i++) {
        buffer[i] = NULL;
    }
    
    // Initialize the index tracking the smallest sequence number
    w->smallest_seqno_idx = 0;
    
    // Store the window reference globally for use by functions that don't take a window parameter
    global_window = w;
    
    return w;
}

int buffer_full(window *w)
{
    if (w == NULL || w->buffer_ptr == NULL) {
        return -1;  // Error case or buffer not full
    }
    
    tcp_packet **buffer = (tcp_packet **) w->buffer_ptr;
    
    // Check if any slot is empty - if so, the buffer is not full
    for (unsigned int i = 0; i < w->window_size; i++) {
        if (buffer[i] == NULL) {
            return -1;  // Buffer is not full - found at least one empty slot
        }
    }
    
    return 1;  // Buffer is full - no empty slots found
}

// Helper function to find the first empty slot in the buffer
static int find_empty_slot(window *w) {
    if (w == NULL || w->buffer_ptr == NULL) {
        return -1;  // Error case
    }
    
    tcp_packet **buffer = (tcp_packet **) w->buffer_ptr;
    
    // Iterate through the buffer to find the first NULL (empty) slot
    for (unsigned int i = 0; i < w->window_size; i++) {
        if (buffer[i] == NULL) {
            return i;  // Return the index of the first empty slot
        }
    }
    
    return -1;  // No empty slots found - buffer is full
}

// Adds a packet to the window buffer in the first available slot
void add_packet_to_buffer(tcp_packet *pkt)
{
    // Input validation
    if (pkt == NULL || global_window == NULL || global_window->buffer_ptr == NULL) {
        return;  // Error case - invalid input
    }
    
    tcp_packet **buffer = (tcp_packet **) global_window->buffer_ptr;
    
    // Find an empty slot in the buffer using helper function
    int empty_slot = find_empty_slot(global_window);
    if (empty_slot == -1) {
        // Buffer is full - cannot add the packet
        printf("Warning: Could not add packet to buffer because it is full\n");
        return;
    }
    
    // Store the packet pointer in the empty slot
    buffer[empty_slot] = pkt;
    
    // Update smallest_seqno_idx if:
    // 1. The current smallest slot is empty (NULL), or
    // 2. The new packet has a smaller sequence number than the current smallest
    if (buffer[global_window->smallest_seqno_idx] == NULL || 
        pkt->hdr.seqno < buffer[global_window->smallest_seqno_idx]->hdr.seqno) {
        global_window->smallest_seqno_idx = empty_slot;
    }
}

// Removes a packet with the specified sequence number from the buffer
// Also updates the smallest sequence number tracking if needed
void remove_packet_from_buffer(int seqno) {
    // Input validation
    if (global_window == NULL || global_window->buffer_ptr == NULL) {
        return;  // Error case - invalid window
    }
    
    tcp_packet **buffer = (tcp_packet **) global_window->buffer_ptr;
    
    // Search for the packet with the given sequence number
    for (unsigned int i = 0; i < global_window->window_size; i++) {
        if (buffer[i] != NULL && buffer[i]->hdr.seqno == seqno) {
            // Found the packet - free its memory and mark the slot as empty
            free(buffer[i]);
            buffer[i] = NULL;
            
            // If we removed the packet with the smallest sequence number,
            // we need to find the new smallest sequence number
            if (i == global_window->smallest_seqno_idx) {
                // Initialize variables to track the new smallest sequence number
                int smallest_seqno = -1;
                unsigned int new_idx = 0;
                
                // Iterate through all slots to find the new smallest sequence number
                for (unsigned int j = 0; j < global_window->window_size; j++) {
                    if (buffer[j] != NULL && (smallest_seqno == -1 || buffer[j]->hdr.seqno < smallest_seqno)) {
                        smallest_seqno = buffer[j]->hdr.seqno;
                        new_idx = j;
                    }
                }
                
                // Update the smallest_seqno_idx if we found a new smallest packet
                if (smallest_seqno != -1) {
                    global_window->smallest_seqno_idx = new_idx;
                }
                // If no packets left, smallest_seqno_idx stays at 0
            }
            
            return;  // Packet removed successfully
        }
    }
    // If we reach here, no packet with the given sequence number was found
}

// returns a pointer to a packet with the smallest sequence number in the buffer; 
// used in packet retransmission
tcp_packet* return_packet_of_smallest_seqno()
{
    int seqno = global_window->smallest_seqno_idx;
    // tcp_packet* pkt = global_window->buffer_ptr[seqno];
    return ((tcp_packet**)global_window->buffer_ptr)[seqno];
}
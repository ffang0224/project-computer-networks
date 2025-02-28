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
    
    w->window_size = window_size;  // Window size in packets
    
    // Allocate memory for the buffer to hold packet pointers
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
    
    // Count occupied slots
    unsigned int occupied_slots = 0;
    for (unsigned int i = 0; i < w->window_size; i++) {
        if (buffer[i] != NULL) {
            occupied_slots++;
        }
    }
    
    // Buffer is full if all slots are occupied (window size is in packets)
    return (occupied_slots >= w->window_size) ? 1 : -1;
}

// Helper function to find the first empty slot in the buffer
static int find_empty_slot(window *w) {
    if (w == NULL || w->buffer_ptr == NULL) {
        return -1;  // Error case
    }
    
    tcp_packet **buffer = (tcp_packet **) w->buffer_ptr;
    
    // Find first empty slot
    for (unsigned int i = 0; i < w->window_size; i++) {
        if (buffer[i] == NULL) {
            return i;
        }
    }
    
    return -1;  // No empty slots found
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
        printf("Warning: Could not add packet to buffer - window full\n");
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
void remove_packet_from_buffer(int seqno) {
    if (global_window == NULL || global_window->buffer_ptr == NULL) {
        return;
    }
    
    tcp_packet **buffer = (tcp_packet **) global_window->buffer_ptr;
    
    // Search for the packet with the given sequence number
    for (unsigned int i = 0; i < global_window->window_size; i++) {
        if (buffer[i] != NULL && buffer[i]->hdr.seqno == seqno) {
            // Free the packet and mark slot as empty
            free(buffer[i]);
            buffer[i] = NULL;
            
            // If we removed the packet with the smallest sequence number,
            // find the new smallest
            if (i == global_window->smallest_seqno_idx) {
                int smallest_seqno = -1;
                unsigned int new_idx = 0;
                
                for (unsigned int j = 0; j < global_window->window_size; j++) {
                    if (buffer[j] != NULL && 
                        (smallest_seqno == -1 || buffer[j]->hdr.seqno < smallest_seqno)) {
                        smallest_seqno = buffer[j]->hdr.seqno;
                        new_idx = j;
                    }
                }
                
                global_window->smallest_seqno_idx = (smallest_seqno != -1) ? new_idx : 0;
            }
            return;
        }
    }
}

// Returns pointer to packet with smallest sequence number in the buffer
tcp_packet* return_packet_of_smallest_seqno()
{
    if (global_window == NULL || global_window->buffer_ptr == NULL) {
        return NULL;
    }
    
    tcp_packet **buffer = (tcp_packet **) global_window->buffer_ptr;
    return buffer[global_window->smallest_seqno_idx];
}
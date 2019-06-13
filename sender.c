#include "util.h"

/*
 * Parses the arguments and flags of the program and initializes the struct state
 * with those parameters (or the default ones if no custom flags are given).
 */
void init_state(struct state *state, int argc, char **argv)
{
    uint64_t pid = printPID();
    // The following calculations are based on the paper:
    //      C5: Cross-Cores Cache Covert Channel (dimva 2015)
    int L3_way_stride = ipow(2, LOG_CACHE_SETS_L3 + LOG_CACHE_LINESIZE);
    uint64_t bsize = 64 * CACHE_WAYS_L3 * L3_way_stride;

    // Allocate a buffer of the size of the LLC
    // state->buffer = malloc((size_t) bsize);
    state->buffer = allocateBuffer(bsize);
    printf("buffer pointer addr %p\n", state->buffer);

    // Initialize the buffer to be be the non-zero page
    for (uint32_t i = 0; i < bsize; i += 64) {
        *(state->buffer + i) = pid;
    }

    // Set some default state values.
    state->addr_set = NULL;
    state->benchmark_mode = false;

    // This number may need to be tuned up to the specific machine in use
    // NOTE: Make sure that interval is the same in both sender and receiver
    state->cache_region = CHANNEL_DEFAULT_REGION;
    state->interval = CHANNEL_DEFAULT_INTERVAL;
    state->access_period = CHANNEL_DEFAULT_ACCESS_PERIOD;
    state->prime_period = 0; // default is half of (interval - access_period)


    // Parse the command line flags
    //      -d is used to enable the debug prints
    //      -b is used to enable the benchmark mode (to measure the sending bitrate)
    //      -i is used to specify a custom value for the time interval
    int option;
    while ((option = getopt(argc, argv, "di:a:br:")) != -1) {
        switch (option) {
            case 'b':
                state->benchmark_mode = true;
                break;
            case 'i':
                state->interval = atoi(optarg);
                break;
            case 'r':
                state->cache_region = atoi(optarg);
                break;
            case 'a':
                state->access_period = atoi(optarg);
                break;
            case '?':
                fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
                exit(1);
            default:
                exit(1);
        }
    }

    if (state->prime_period == 0) {
        state->prime_period = (state->interval - state->access_period) / 2;
        state->probe_period = (state->interval - state->access_period) / 2;
    }
    else {
        state->probe_period = (state->interval - state->prime_period - state->access_period);
    }

    // Construct the addr_set by taking the addresses that have cache set index 0
    // There should be 128 such addresses in our buffer:
    //  one per line per cache set 0 of each slice (8 * 16).
    uint32_t addr_set_size = 0;
    for (int set_index = 0; set_index < CACHE_SETS_L3; set_index++) {
        for (int line_index = 0; line_index < 64 * CACHE_WAYS_L3; line_index++) {

            ADDR_PTR addr = (ADDR_PTR) (state->buffer + \
                    set_index * CACHE_LINESIZE + line_index * L3_way_stride);
            if (get_L3_cache_set_index(addr) == state->cache_region) {
                append_string_to_linked_list(&state->addr_set, addr);
                addr_set_size++;
            }
        }
    }
    printf("Found addr_set size of %u\n", addr_set_size);

}

/*
 * Sends a bit to the receiver by repeatedly flushing the addresses of the addr_set
 * for the clock length of state->interval when we are sending a one, or by doing nothing
 * for the clock length of state->interval when we are sending a zero.
 */
void send_bit(bool one, const struct state *state)
{
    uint64_t start_t, curr_t;

    start_t = getTime();
    curr_t = start_t;

    if (one) {
        if ((curr_t - start_t) < state->interval) {
            // wait for receiver to prime the cache set
            // while (getTime() - start_t < state->prime_period) {}

            // access
            struct Node *current = NULL;
            // uint64_t stopTime = start_t + state->prime_period + state->access_period;
            uint64_t stopTime = start_t + state->interval;
            do {
                current = state->addr_set;
                while (current != NULL && current->next != NULL && getTime() < stopTime) {
                // while (current != NULL && getTime() < stopTime) {
                    volatile uint64_t* addr1 = (uint64_t*) current->addr;
                    volatile uint8_t* addr2 = (uint8_t*) current->next->addr;
                    *addr1;
                    *addr2;
                    *addr1;
                    *addr2;
                    *addr1;
                    *addr2;
                    current = current->next;
                }
            } while (getTime() < stopTime);

            // wait for receiver to probe
            while (getTime() < state->interval) {}

        }

    } else {
        while (getTime() - start_t < state->interval) {}
    }
}

int main(int argc, char **argv)
{
    // Initialize state and local variables
    struct state state;
    init_state(&state, argc, argv);
    uint64_t start_t, end_t;
    int sending = 1;
    printf("Please type a message (exit to stop).\n");
    char all_one_msg[129];
    for (uint32_t i = 0; i < 120; i++)
        all_one_msg[i] = '1';
    for (uint32_t i = 120; i < 128; i++)
        all_one_msg[i] = '1';
    all_one_msg[128] = '\0';
    while (sending) {
#if 1
        // Get a message to send from the user
        printf("< ");
        char text_buf[128];
        fgets(text_buf, sizeof(text_buf), stdin);

        if (strcmp(text_buf, "exit\n") == 0) {
            sending = 0;
        }

        char *msg = string_to_binary(text_buf);
#else
        char *msg = all_one_msg;
#endif

        // If we are in benchmark mode, start measuring the time
        if (state.benchmark_mode) {
            start_t = getTime();
        }

        // Send a '10101011' byte to let the receiver detect that
        // I am about to send a start string and sync
        size_t msg_len = strlen(msg);

        // sync on clock edge
        while((getTime() & 0x003fffff) > 20000) {}

        for (int i = 0; i < 10; i++) {
            while((getTime() & 0x003fffff) > 20000) {}
            send_bit(i % 2 == 0, &state);
        }
        while((getTime() & 0x003fffff) > 20000) {}
        send_bit(true, &state);
        while((getTime() & 0x003fffff) > 20000) {}
        send_bit(true, &state);

        // Send the message bit by bit
        for (int ind = 0; ind < msg_len; ind++) {
            if (msg[ind] == '0') {
                send_bit(false, &state);
            } else {
                send_bit(true, &state);
            }
        }

        debug("message %s sent\n", msg);

        // If we are in benchmark mode, finish measuring the
        // time and print the bit rate.
        // if (state.benchmark_mode) {
        //     end_t = getTime();
        //     printf("Bitrate: %.2f Bytes/second\n",
        //            ((double) strlen(text_buf)) / ((double) (end_t - start_t) / CLOCKS_PER_SEC));
        // }
    }

    printf("Sender finished\n");
    return 0;
}

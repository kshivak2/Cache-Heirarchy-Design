
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <iostream>
#include <cassert>
#include <cmath>
#include "sim.h"

/*  "argc" holds the number of command-line arguments.
    "argv[]" holds the arguments themselves.

    Example:
    ./sim 32 8192 4 262144 8 3 10 gcc_trace.txt
    argc = 9
    argv[0] = "./sim"
    argv[1] = "32"
    argv[2] = "8192"
    ... and so on
*/
using namespace std;
#define ADDRESS_WIDTH 32
class Cache
{
public:
    int **arr; // Pointer to a dynamic 2D array
    int **valid;
    int **dirty;
    int **lRU;
    Cache *next;

    int n; // number of streambuffers
    int m; // total number of memory blocks
    int **stream_buffer;

    int num_set;
    int assoc;
    int blk_size;
    int index_bits;
    int tag_bits;
    int blk_offset_bits;

    int l1_reads;
    int l2_reads;
    int num_of_write_request_received;
    int num_of_prefetch_request_received;
    int num_of_read_miss;  // implemented in line 157
    int num_of_read_hit;   // implemented in line 153
    int num_of_write_miss; // implemened in line 200
    int num_of_write_hit;  // implemented in line 192
    int num_of_prefetch_miss;
    int num_of_read_forwarded_from_streambuffer;
    int num_of_write_forwarded_from_streambuffer;
    int num_of_prefetch_forwarded_from_stream_buffer;
    int num_of_prefetch_issued_to_next_level;
    int num_of_writeback_during_eviction;
    int l1_write_backs;
    int l2_writebacks;
    int l2_write;
    int mem_traffic;

    uint32_t tag;

    Cache(int num_set, int assoc, int blk_size, Cache *next_cache) : num_set(num_set), assoc(assoc), blk_size(blk_size), next(next_cache)
    {
        // Allocate memory for the dynamic 2D array
        arr = new int *[num_set];
        valid = new int *[num_set];
        dirty = new int *[num_set];
        lRU = new int *[num_set];

        blk_offset_bits = log2(blk_size);
        index_bits = log2(num_set);
        tag_bits = ADDRESS_WIDTH - index_bits - blk_offset_bits;

        for (int i = 0; i < num_set; ++i)
        {
            arr[i] = new int[assoc];
            valid[i] = new int[assoc];
            dirty[i] = new int[assoc];
            lRU[i] = new int[assoc];
        }

        for (int i = 0; i < num_set; i++)
        { // LRU array initialization
            for (int j = 0; j < assoc; j++)
            {
                lRU[i][j] = j;
            }
        }
    }

    Cache(int n, int m) : n(n), m(m)
    {
        if (n == 0)
        {
            // No stram buffers
        }
        stream_buffer = new int *[m];
        for (int i = 1; i < n; i++)
        {
            stream_buffer[i] = new int[n];
        }
    }

    void lru_update(int setIndex, int way)
    {
        int oldLRU = lRU[setIndex][way];
        // lRU[setIndex][way] = 0; // Hit LRU becomes 0

        for (int j = 0; j < assoc; j++)
        {
            if (lRU[setIndex][j] < oldLRU)
            {
                ++lRU[setIndex][j]; // LRU increments for lesser than hit (i). Others remain the same.
            }
        }
        lRU[setIndex][way] = 0; // Hit LRU becomes 0
    }

    uint32_t calc_index(uint32_t address)
    {
        uint32_t setIndex = (address >> blk_offset_bits) & ((1 << index_bits) - 1);
        return setIndex;
    }

    uint32_t calc_tag(uint32_t address)
    {

        uint32_t tag = (address >> (blk_offset_bits + index_bits)) & ((1 << tag_bits) - 1);
        return tag;
    }

    uint32_t reconstruct_blk_addr(uint32_t tag, uint32_t index)
    {
        // return ((tag << (index_bits + blk_offset_bits )) | (index << index_bits));
        return ((tag << (ADDRESS_WIDTH - tag_bits)) | (index << blk_offset_bits));
    }

    void read(uint32_t address)
    {
        int hit_way;
        uint32_t tag;
        uint32_t index;
        index = calc_index(address);
        tag = calc_tag(address);
        hit_way = check_hit(index, tag);

        if (hit_way < assoc)
        {
            // hit
            //  TODO: increment read hit counter - Done
            num_of_read_hit++;
            lru_update(index, hit_way);
        }
        else
        {
            int victim_way;
            // TODO: update the read miss counter - Done
            num_of_read_miss++;

            victim_way = find_victim(index);
            assert(0 <= victim_way && victim_way < assoc);
            eviction(index, victim_way);
            refill(index, victim_way, tag);
            // miss
            /*
            To handle a miss
                    1. (find victim)  find the LRU block (lru = ASSOC-1)
                    2. (eviction)     if the victim is valid && dirty, issue write to next level
                    3. (refill)       issue read to next level, update the tag, set the dirty=0 and valid=1
                    4. (update dirty) set dirty=1 if write happens
            */
        }
    }

    void write(uint32_t address)
    {
        int hit_way;
        uint32_t tag;
        uint32_t index;
        index = calc_index(address);
        tag = calc_tag(address);
        hit_way = check_hit(index, tag);

        if (hit_way < assoc)
        {
            // hit
            //  TODO: increment write hit counter
            num_of_write_hit++;
            lru_update(index, hit_way);
            dirty[index][hit_way] = 1;
            // if (tag == 524458)
            // {
            //     // void printarr_L2();
            //     cout << "inside l2 write hit!" << endl;
            //     for (int j = 0; j < assoc; j++)
            //     {
            //         cout << hex << arr[index][j] << " " << endl;
            //     }
            // }
        }
        else
        {
            int victim_way;
            // TODO: update the write miss counter - Done
            num_of_write_miss++;
            victim_way = find_victim(index);
            assert(0 <= victim_way && victim_way < assoc);
            eviction(index, victim_way);
            refill(index, victim_way, tag);
            dirty[index][victim_way] = 1;

        }
    }

    int check_hit(int setIndex, uint32_t tag)
    {
        for (int i = 0; i < assoc; i++)
        {
            if (valid[setIndex][i] == 1 && tag == arr[setIndex][i])
            {
                return i;
            }
        }
        return assoc;
    }

    int find_victim(int set_index)
    {
        int victim_way;
        for (victim_way = 0; victim_way < assoc; victim_way++)
        {
            if (lRU[set_index][victim_way] == assoc - 1)
            {
                return victim_way;
            }
        }
        assert(false);
    }

    void eviction(int set_index, int victim_way)
    {
        if (valid[set_index][victim_way] == 1 && dirty[set_index][victim_way] == 1)
        {
            if (next != nullptr)
            {

                uint32_t blk_address_of_victim = reconstruct_blk_addr(arr[set_index][victim_way], set_index);
                next->write(blk_address_of_victim);
                l1_write_backs++;
            }
            else
            {
                // write to main memory
                // TODO: increment the counter for memory traffic
                l2_writebacks++;
                l1_write_backs++;
                mem_traffic++;
            }
        }
    }

    void refill(int set_index, int way_to_refill, uint32_t tag)
    {
        // issue read to next level, update the tag, set the dirty=0 and valid=1
        uint32_t blk_address = reconstruct_blk_addr(tag, set_index);
        if (next != nullptr)
        {
            next->read(blk_address);
        }
        else
        {
            // read from main memory
            // TODO: increment the counter for memory traffic
            mem_traffic++;
        }
        arr[set_index][way_to_refill] = tag;
        valid[set_index][way_to_refill] = 1;
        dirty[set_index][way_to_refill] = 0;
        lru_update(set_index, way_to_refill);
    }

    void printarr_L1()
    {

        for (int i = 0; i < num_set; i++)
        {
            for (int j = 0; j < assoc; j++)
            {
                cout << hex << arr[i][j] << " ";
            }
            cout << endl;
        }
        cout << dec << "L1 reads: " << num_of_read_miss + num_of_read_hit << endl;
        cout << "L1 read misses: " << num_of_read_miss << endl;
        cout << "L1 writes: " << num_of_write_miss + num_of_write_hit << endl;
        cout << "L1 write misses: " << num_of_write_miss << endl;
        cout << "L1 writebacks: " << l1_write_backs << endl;
        cout << "total mem traffic: " << mem_traffic << endl;
    }
    void printarr_L2()
    {

        for (int i = 0; i < num_set; i++)
        {
            for (int j = 0; j < assoc; j++)
            {
                cout << hex << arr[i][j] << " ";
            }
            cout << endl;
        }
        cout << dec << "L2 reads (demand): " << num_of_read_miss + num_of_read_hit << endl;
        cout << "L2 read misses (demand): " << num_of_read_miss << endl;
        cout << "L2 writes: " << num_of_write_hit + num_of_write_miss << endl;
        cout << "L2 write misses: " << num_of_write_miss << endl;
        cout << "L2 write backs: " << l2_writebacks << endl;
        cout << "total mem traffic: " << mem_traffic << endl;
        // cout<<"inside l2 loop";
    }
    void printvalid()
    {

        for (int i = 0; i < num_set; i++)
        {
            for (int j = 0; j < assoc; j++)
            {
                cout << valid[i][j] << " ";
            }
            cout << endl;
        }
    }
    void printdirty()
    {

        for (int i = 0; i < num_set; i++)
        {
            for (int j = 0; j < assoc; j++)
            {
                cout << dirty[i][j] << " ";
            }
            cout << endl;
        }
    }
    void printlRU()
    {

        for (int i = 0; i < num_set; i++)
        {
            for (int j = 0; j < assoc; j++)
            {
                cout << lRU[i][j] << " ";
            }
            cout << endl;
        }
    }
};

int main(int argc, char *argv[])
{
    FILE *fp;              // File pointer.
    char *trace_file;      // This variable holds the trace file name.
    cache_params_t params; // Look at the sim.h header file for the definition of struct cache_params_t.
    char rw;               // This variable holds the request's type (read or write) obtained from the trace.
    uint32_t addr;         // This variable holds the request's address obtained from the trace.
                           // The header file <inttypes.h> above defines signed and unsigned integers of various sizes in a machine-agnostic way.  "uint32_t" is an unsigned integer of 32 bits.
    Cache *L1;
    Cache *L2;
    Cache *SB;
    // Exit with an error if the number of command-line arguments is incorrect.
    if (argc != 9)
    {
        printf("Error: Expected 8 command-line arguments but was provided %d.\n", (argc - 1));
        exit(EXIT_FAILURE);
    }

    // "atoi()" (included by <stdlib.h>) converts a string (char *) to an integer (int).
    params.BLOCKSIZE = (uint32_t)atoi(argv[1]);
    params.L1_SIZE = (uint32_t)atoi(argv[2]);
    params.L1_ASSOC = (uint32_t)atoi(argv[3]);
    params.L2_SIZE = (uint32_t)atoi(argv[4]);
    params.L2_ASSOC = (uint32_t)atoi(argv[5]);
    params.PREF_N = (uint32_t)atoi(argv[6]);
    params.PREF_M = (uint32_t)atoi(argv[7]);
    trace_file = argv[8];

    // Open the trace file for reading.
    fp = fopen(trace_file, "r");
    if (fp == (FILE *)NULL)
    {
        // Exit with an error if file open failed.
        printf("Error: Unable to open file %s\n", trace_file);
        exit(EXIT_FAILURE);
    }

    // Print simulator configuration.
    printf("===== Simulator configuration =====\n");
    printf("BLOCKSIZE:  %u\n", params.BLOCKSIZE);
    printf("L1_SIZE:    %u\n", params.L1_SIZE);
    printf("L1_ASSOC:   %u\n", params.L1_ASSOC);
    printf("L2_SIZE:    %u\n", params.L2_SIZE);
    printf("L2_ASSOC:   %u\n", params.L2_ASSOC);
    printf("PREF_N:     %u\n", params.PREF_N);
    printf("PREF_M:     %u\n", params.PREF_M);
    printf("trace_file: %s\n", trace_file);
    printf("===================================\n");

    int blockOffsetBits = log2(params.BLOCKSIZE);
    int setIndexBits = log2(params.L1_SIZE / (params.BLOCKSIZE * params.L1_ASSOC));

    // uint32_t indexLength = pow(2,setIndexBits);
    // int i,j;
    // int tagArr[indexLength][params.L1_ASSOC]; //arrasy initialization
    // int tagArr[4][64]; //arrasy initialization
    int indexLength = pow(2, setIndexBits);

    if (params.L2_SIZE == 0)
    {
        int l1_num_set = (params.L1_SIZE / params.BLOCKSIZE) / params.L1_ASSOC;
        L1 = new Cache(l1_num_set, params.L1_ASSOC, params.BLOCKSIZE, nullptr);
    }
    else
    {
        int l2_num_set = (params.L2_SIZE / params.BLOCKSIZE) / params.L2_ASSOC;
        L2 = new Cache(l2_num_set, params.L2_ASSOC, params.BLOCKSIZE, nullptr);

        int l1_num_set = (params.L1_SIZE / params.BLOCKSIZE) / params.L1_ASSOC;
        L1 = new Cache(l1_num_set, params.L1_ASSOC, params.BLOCKSIZE, L2);

        //         address of L1  (0x0000150)          address of L2 (0x00002000)
        //         \/                                  \/
        // Memory: |L1: an instance of Cache class| ... |L2: an instance of Cache class |
        //         | Cache* next = L2;            |     |    cache* next -> nullptr     |
        //                   \                          /\
        //                    ---------------------------
    }
    // if (params.L2_SIZE == 0)
    // {

    //     int l1_num_set = (params.L1_SIZE / params.BLOCKSIZE) / params.L1_ASSOC;
    //     L1 = new Cache(l1_num_set, params.L1_ASSOC, params.BLOCKSIZE, nullptr);
    // }
    // else
    // {

    //     int l2_num_set = (params.L2_SIZE / params.BLOCKSIZE) / params.L2_ASSOC;
    //     L2 = new Cache(l2_num_set, params.L2_ASSOC, params.BLOCKSIZE, nullptr);

    //     int l1_num_set = (params.L1_SIZE / params.BLOCKSIZE) / params.L1_ASSOC;
    //     L1 = new Cache(l1_num_set, params.L1_ASSOC, params.BLOCKSIZE, L2);
    // }

    // Read requests from the trace file and echo them back.
    while (fscanf(fp, "%c %x\n", &rw, &addr) == 2)
    { // Stay in the loop if fscanf() successfully parsed two tokens as specified.
        if (rw == 'r')
            // printf("r %x\n", addr);
            L1->read(addr);
        //(*L1).read(addr);
        else if (rw == 'w')
            // printf("w %x\n", addr);
            L1->write(addr);
        else
        {
            printf("Error: Unknown request type %c.\n", rw);
            exit(EXIT_FAILURE);
        }
        // L1->printarr_L1();
    }

    L1->printarr_L1();
    (*L2).printarr_L2();
    // L1->printvalid();
    // L1->printdirty();
    // L1->printlRU();

    return (0);
}

#include <mpi.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
/* wątki */
#include <pthread.h>
/* sem_init sem_destroy sem_post sem_wait */
//#include <semaphore.h>
/* flagi dla open */
//#include <fcntl.h>
/* boolean */
#define TRUE 1
#define FALSE 0

/* defaultowe wartości dla random_sleep */
#define DEFAULT_MIN_SLEEP 100
#define DEFAULT_MAX_SLEEP 500

/* typy wiadomości */
#define REQ_A 1
#define REQ_G 2
#define ACK_A 3
#define REQ_SLOT 4
#define ACK_REQ_SLOT 5
#define RELEASE_SLOT 6

#define MAX_SLOTS 10
#define MAX_ARTISTS 10
#define MAX_ENGINEERS 10

#define ROLE_A 0 
#define ROLE_G 1 

/* inicjalizacja zegara Lamporta */
int lamport_clock = 0;

char passive = FALSE;

pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;
volatile int end = 0;

/* komunikatory grupowe */
MPI_Comm artist_comm = MPI_COMM_NULL;
MPI_Comm engineer_comm = MPI_COMM_NULL;

/* typy wiadomości */
MPI_Datatype MPI_MESSAGE_T;
MPI_Datatype MPI_SLOT_REQUEST_T;

typedef struct {
    int type;
    int sender_id;
    int clock;
} message;

typedef struct {
    int sender_id;
    int clock;
    int g_pair;
    int num_slots;
} slot_request;

int role;
int paired = -1; // is the artist paired with an engineer?
int pending_req[MAX_ENGINEERS]; // pending requests from engineers
int priority[MAX_ENGINEERS]; // table of priorities for engineers
int request_from_a; //pending request from artist to engineer
slot_request slot_requests[MAX_ARTISTS];
int has_slot_request[MAX_ARTISTS]; // TRUE jeśli mamy zapisany request od danego artysty
int ack_slot_received_from_artists[MAX_ARTISTS];

/* Funkcja do losowego usypiania wątków */
void random_sleep(int min_ms, int max_ms) {
    int range = max_ms - min_ms + 1;
    int sleep_ms = min_ms + rand() % range;
    usleep(sleep_ms * 1000); // usleep przyjmuje mikrosekundy
}

/* Zwiększa zegar Lamporta o 1 (przed wysłaniem wiadomości) */
void increment_lamport() {
    pthread_mutex_lock(&mut);
    lamport_clock++;
    pthread_mutex_unlock(&mut);
}

/* Aktualizuje zegar Lamporta po otrzymaniu wiadomości */
void update_lamport(int received_clock) {
    pthread_mutex_lock(&mut);
    if (lamport_clock < received_clock) {
        lamport_clock = received_clock;
    }
    lamport_clock++;
    pthread_mutex_unlock(&mut);
}

/* Zwraca aktualną wartość zegara */
int get_lamport() {
    pthread_mutex_lock(&mut);
    int val = lamport_clock;
    pthread_mutex_unlock(&mut);
    return val;
}

/* MPI_Send wraper obługujący zegar lamporta */
int lamport_send(void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm) {
    // Ustaw zegar w strukturze wiadomości — zależnie od typu
    if (datatype == MPI_MESSAGE_T) {
        ((message*)buf)->clock = get_lamport();
    } else if (datatype == MPI_SLOT_REQUEST_T) {
        ((slot_request*)buf)->clock = get_lamport();
    }

    return MPI_Send(buf, count, datatype, dest, tag, comm);
}

void create_message_types() {
    const int nitems = 3;
    int blocklengths[3] = {1, 1, 1};
    MPI_Datatype typy[3] = {MPI_INT, MPI_INT, MPI_INT};
    MPI_Aint offsets[3];

    offsets[0] = offsetof(message, type);
    offsets[1] = offsetof(message, sender_id);
    offsets[2] = offsetof(message, clock);

    MPI_Type_create_struct(nitems, blocklengths, offsets, typy, &MPI_MESSAGE_T);
    MPI_Type_commit(&MPI_MESSAGE_T);

    const int nitems2 = 4;
    int blocklengths2[4] = {1, 1, 1, 1};
    MPI_Datatype typy2[4] = {MPI_INT, MPI_INT, MPI_INT, MPI_INT};
    MPI_Aint offsets2[4];

    offsets2[0] = offsetof(slot_request, sender_id);
    offsets2[1] = offsetof(slot_request, clock);
    offsets2[2] = offsetof(slot_request, g_pair);
    offsets2[3] = offsetof(slot_request, num_slots);

    MPI_Type_create_struct(nitems2, blocklengths2, offsets2, typy2, &MPI_SLOT_REQUEST_T);
    MPI_Type_commit(&MPI_SLOT_REQUEST_T);
}

void create_role_comms(MPI_Comm world_comm, int rank, int size) {
    int artist_ranks[MAX_ARTISTS];
    int engineer_ranks[MAX_ENGINEERS];
    int artist_count = 0, engineer_count = 0;

    for (int i = 0; i < size; i++) {
        if (i < MAX_ARTISTS)
            artist_ranks[artist_count++] = i;
        else
            engineer_ranks[engineer_count++] = i;
    }

    MPI_Group world_group;
    MPI_Comm_group(world_comm, &world_group);

    MPI_Group artist_group, engineer_group;
    MPI_Group_incl(world_group, artist_count, artist_ranks, &artist_group);
    MPI_Group_incl(world_group, engineer_count, engineer_ranks, &engineer_group);

    if (rank < MAX_ARTISTS)
        MPI_Comm_create(world_comm, artist_group, &artist_comm);
    else
        MPI_Comm_create(world_comm, engineer_group, &engineer_comm);
}

void send_message_to_artists(message *msg, int tag) {
    increment_lamport();
    for (int i = 0; i < MAX_ARTISTS; i++) {
        if (i != msg->sender_id) { // nie wysyłaj do siebie
            lamport_send(msg, 1, MPI_MESSAGE_T, i, tag, MPI_COMM_WORLD);
        }
    }
}

void send_message_to_engineers(message *msg, int tag) {
    increment_lamport();
    for (int i = MAX_ARTISTS; i < MAX_ARTISTS + MAX_ENGINEERS; i++) {
        if (i != msg->sender_id) {
            lamport_send(msg, 1, MPI_MESSAGE_T, i, tag, MPI_COMM_WORLD);
        }
    }
}

void send_message_to_process(message *msg, int dest, int tag) {
    increment_lamport();
    lamport_send(msg, 1, MPI_MESSAGE_T, dest, tag, MPI_COMM_WORLD);
}

// ARTIST THREAD 
void *artist_thread_func(void *ptr) {
    message msg;
    slot_request req;
    while (!end) {
        // get max value from priority array
        int max_priority = -1;
        int highest_priority_index = -1;
        for (int i = 0; i < MAX_ENGINEERS; i++) {
            if (priority[i] > max_priority) {
                max_priority = priority[i];
                highest_priority_index = i;
            }
        }
        // find the first engineer with max priority
        int engineer_id = -1;
        if (highest_priority_index != -1) {
            if (pending_req[highest_priority_index] == TRUE) {
                engineer_id = highest_priority_index + MAX_ARTISTS;
                msg.type = REQ_G;
                msg.sender_id = rank;
                msg.clock = get_lamport();
                send_message_to_process(&msg, engineer_id, REQ_G);

                // co jezeli w tym czasie g znajdzie innego a to ...
                // wychodzimy z petli kiedy dostaniemy info ze engineer id wzial kogos innego
                
                while (pending_req[highest_priority_index] == TRUE){
                    if (paired == -1) {
                        continue;
                    }
                    req.sender_id = rank;
                    req.clock = get_lamport();
                    req.g_pair = paired;
                    req.num_slots = 3; // EXAMPLE: NEED TO IMPLEMENT RANDOM od 1 do max slots
                    send_message_to_engineers(&req, REQ_SLOT);

                    printf("[Rank %d | Clock %d] Sending SLOT_REQUEST to engineer %d (num of slots: %d, paired with g nr: %d)\n",
                        rank, get_lamport(), paired, req.num_slots, req.g_pair);

                    //asking for slots
                    // waiting for ACK_SLOT
                    // if ACK_SLOT received

                    random_sleep(DEFAULT_MIN_SLEEP, DEFAULT_MAX_SLEEP); // simulate working

                    msg.type = RELEASE_SLOT;
                    msg.sender_id = rank;
                    msg.clock = get_lamport();
                    send_message_to_artists(&msg, paired, RELEASE_SLOT);
                    paired = -1; // reset paired
                    random_sleep(DEFAULT_MIN_SLEEP, DEFAULT_MAX_SLEEP); // simulate taking a break
                
                }
            } // else mniejsze priorytety, sortujemy priorytety wg wartosci
        }


        //----------------------------------------
        // SLOT REQUEST LOGIC
        //-----------------------------------------

        // for (int i = 0; i < MAX_ARTISTS; i++) {
        //     if (i != rank) {
        //         ack_slot_received_from_artists[i] = FALSE;
        //     }
        // }

        // send_message_to_artists(&req, REQ_SLOT);
        // printf("[Rank %d | Clock %d] Sent SLOT_REQUEST to all other artists\n", rank, get_lamport());

        // // wait for ACK_REQ_SLOT from all other artists
        // while (1) {
        //     int all_received = TRUE;
        //     for (int i = 0; i < MAX_ARTISTS; i++) {
        //         if (i != rank && ack_slot_received_from_artists[i] == FALSE) {
        //             all_received = FALSE;
        //             break;
        //         }
        //     }
        //     if (all_received) break;
        //     random_sleep(DEFAULT_MIN_SLEEP, DEFAULT_MAX_SLEEP);
        // }
        // printf("[Rank %d | Clock %d] All ACK_REQ_SLOT received from other artists\n", rank, get_lamport());

    }
    return NULL;
}

// ENGINEER THREAD 
void *engineer_thread_func(void *ptr) {
    message msg;
    int waiting;

    while (!end) {
        msg.type = REQ_A;
        msg.sender_id = rank;
        msg.clock = get_lamport();
        send_message_to_artists(&msg, REQ_A);
        waiting = TRUE;

        while (waiting) {
            if(request_from_a == -1){
                continue;
            }
            msg.type = ACK_A;
            msg.sender_id = rank;
            msg.clock = get_lamport();
            send_message_to_process(&msg, request_from_a, ACK_A);
            request_from_a = -1;
            
            random_sleep(DEFAULT_MIN_SLEEP, DEFAULT_MAX_SLEEP); // simulate working
            waiting = FALSE;
        }
    }
    return NULL;
}

// COMMUNICATION THREAD HANDLING MESSAGES
void *comm_thread_func(void *ptr) {
    MPI_Status status;

    while (!end) {
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        if (status.MPI_TAG == REQ_SLOT) {
            // Handle receiving a slot request
            slot_request req;
            MPI_Recv(&req, 1, MPI_SLOT_REQUEST_T, status.MPI_SOURCE, status.MPI_TAG, MPI_COMM_WORLD, &status);
            update_lamport(req.clock);

            printf("[Rank %d | Clock %d] Received SLOT_REQUEST from %d (needed num of slots: %d, paired with g nr: %d)\n",
                   rank, get_lamport(), req.sender_id, req.num_slots, req.g_pair);

            pending_req[req.g_pair] = FALSE;

            int idx = req.sender_id;
            if (idx >= 0 && idx < MAX_ARTISTS) {
                slot_requests[idx] = req;
                has_slot_request[idx] = TRUE;
            } else {
                printf("[Rank %d] Invalid sender_id in SLOT_REQUEST: %d\n", rank, req.sender_id);
            }

            slot_requests[idx] = req;
            has_slot_request[idx] = TRUE;

            // Send ACK_REQ_SLOT to the sender
            message ack;
            ack.type = ACK_REQ_SLOT;
            ack.sender_id = rank;
            ack.clock = get_lamport();
            send_message_to_process(&ack, req.sender_id, ACK_REQ_SLOT);

            printf("[Rank %d | Clock %d] Sent ACK_REQ_SLOT to artist %d\n", 
               rank, get_lamport(), req.sender_id);
            
            continue;
        }

        message msg;
        MPI_Recv(&msg, 1, MPI_MESSAGE_T, status.MPI_SOURCE, status.MPI_TAG, MPI_COMM_WORLD, &status);
        update_lamport(msg.clock);

        printf("[Rank %d | Clock %d] Received message from %d (type %d)\n",
                rank, get_lamport(), msg.sender_id, msg.type);

        switch (msg.type) {
            case REQ_A: {
                if (role == ROLE_A) {
                    int sender = msg.sender_id;
                    pending_req[sender - MAX_ARTISTS] = TRUE;
                }
                break;
            }
            case REQ_G:
                if (role == ROLE_G) {
                    request_from_a = msg.sender_id;
                }
                break;
            case ACK_A:
                if (role == ROLE_A) {
                    int sender = msg.sender_id;
                    pending_req[sender - MAX_ARTISTS] = FALSE;
                    for (int g = 0; g < MAX_ENGINEERS; g++) {
                        if (g == sender - MAX_ARTISTS) {
                            priority[g] = 0;
                        } else {
                            priority[g] += 1;
                        }
                    }
                    paired = sender;
                }
                break;
            case ACK_REQ_SLOT:
                if (role == ROLE_A) {
                    int sender = msg.sender_id;
                    if (sender >= 0 && sender < MAX_ARTISTS) {
                        ack_slot_received_from_artists[sender] = TRUE;
                        printf("[Rank %d | Clock %d] Received ACK_REQ_SLOT from artist %d\n", 
                            rank, get_lamport(), sender);
                    }
                }
                break;
            case RELEASE_SLOT:
                int sender = msg.sender_id;
                if (sender >= 0 && sender < MAX_ARTISTS) {
                    has_slot_request[sender] = FALSE; // Clear request tracking

                    printf("[Rank %d | Clock %d] RELEASE_SLOT received from %d. Slot now free.\n",
                        rank, get_lamport(), sender);
                } else {
                    printf("[Rank %d] Invalid sender in RELEASE_SLOT: %d\n", rank, sender);
                }
                break;
            default:
                printf("[Rank %d] Unknown message type: %d\n", rank, msg.type);
                break;
        }
    }
    return NULL;
}



int main(int argc, char **argv) {
    printf("poczatek\n");

    srand(0);

    int provided;
    MPI_Init_thread(&argc, &argv,MPI_THREAD_MULTIPLE, &provided);

    printf("THREAD SUPPORT: %d\n", provided);
    switch (provided) {
        case MPI_THREAD_SINGLE: 
            printf("Brak wsparcia dla wątków, kończę\n");
            /* Nie ma co, trzeba wychodzić */
	    fprintf(stderr, "Brak wystarczającego wsparcia dla wątków - wychodzę!\n");
	    MPI_Finalize();
	    exit(-1);
	    break;
        case MPI_THREAD_FUNNELED: 
            printf("tylko te wątki, ktore wykonaly mpi_init_thread mogą wykonać wołania do biblioteki mpi\n");
	    break;
        case MPI_THREAD_SERIALIZED: 
            /* Potrzebne zamki wokół wywołań biblioteki MPI */
            printf("tylko jeden watek naraz może wykonać wołania do biblioteki MPI\n");
	    break;
        case MPI_THREAD_MULTIPLE: printf("Pełne wsparcie dla wątków\n");
	    break;
        default: printf("Nikt nic nie wie\n");
    }

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    // Ustal rolę
    if (rank < MAX_ARTISTS) {
        role = ROLE_A;
    } else {
        role = ROLE_G;
    }

    create_message_types();
    create_role_comms(MPI_COMM_WORLD, rank, size);

    for (int i = 0; i < MAX_ARTISTS; i++) {
        has_slot_request[i] = FALSE;
        ack_slot_received_from_artists[i] = FALSE;
    }

    // Start komunikacji i logiki roli
    pthread_t comm_thread, role_thread;
    pthread_create(&comm_thread, NULL, comm_thread_func, NULL);

    if (role == ROLE_A) {
        pthread_create(&role_thread, NULL, artist_thread_func, NULL);
    } else {
        pthread_create(&role_thread, NULL, engineer_thread_func, NULL);
    }

    // Czekaj na zakończenie
    pthread_join(role_thread, NULL);
    pthread_join(comm_thread, NULL);

    // Sprzątanie
    MPI_Type_free(&MPI_MESSAGE_T);
    MPI_Type_free(&MPI_SLOT_REQUEST_T);
    pthread_mutex_destroy(&mut);
    MPI_Finalize();

    return 0;
}
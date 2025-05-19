#include <mpi.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
/* wątki */
#include <pthread.h>
/* sem_init sem_destroy sem_post sem_wait */
//#include <semaphore.h>
/* flagi dla open */
//#include <fcntl.h>
/* boolean */
#define TRUE 1
#define FALSE 0

/* typy wiadomości */
//#define FINISH 1
//#define APP_MSG 2
#define REQ_A 1
#define REQ_G 2
#define ACK_A 3
#define REQ_SLOT 4
#define RELEASE_SLOT 5

#define MAX_SLOTS 10
#define MAX_ARTISTS 10
#define MAX_ENGINEERS 10

#define ROLE_A 0 
#define ROLE_G 1 

/* inicjalizacja zegara Lamporta */
int lamport_clock = 0;

char passive = FALSE;

pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;

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
    int type;
    int sender_id;
    int clock;
    int g_pair;
    int num_slots;
} slot_request;

int role;
int pending_req[MAX_ENGINEERS];
int request_from_a;
int priority[MAX_ENGINEERS];
int paired;

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

    const int nitems2 = 5;
    int blocklengths2[5] = {1, 1, 1, 1, 1};
    MPI_Datatype typy2[5] = {MPI_INT, MPI_INT, MPI_INT, MPI_INT, MPI_INT};
    MPI_Aint offsets2[5];

    offsets2[0] = offsetof(slot_request, type);
    offsets2[1] = offsetof(slot_request, sender_id);
    offsets2[2] = offsetof(slot_request, clock);
    offsets2[3] = offsetof(slot_request, g_pair);
    offsets2[4] = offsetof(slot_request, num_slots);

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

void send_message_to_all(void *msg, int count, MPI_Datatype datatype, int tag, int self_rank, int comm_size) {
    increment_lamport();
    for (int dest = 0; dest < comm_size; dest++) {
        if (dest != self_rank) {
            lamport_send(msg, count, datatype, dest, tag, MPI_COMM_WORLD);
        }
    }
}


void *comm_thread_func(void *ptr) {
    message message;
    MPI_Status status;

    while (!end) {
        MPI_Recv(&message, 1, MPI_MESSAGE_T, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        update_lamport(message.clock);

        printf("[Rank %d | Clock %d] Received message from %d (type %d)\n",
               rank, get_lamport(), message.sender_id, message.type);

        switch (message.type) {
            case REQ_A: {
                if (role == ROLE_A) {
                    int sender = message.sender_id;
                    // Store the pending request
                    pending_req[sender - MAX_ARTISTS] = TRUE;
                }
                break;
            }
            case REQ_G:
                if (role == ROLE_G) {
                    int sender = message.sender_id;
                    request_from_a = sender;
                }
                break;
            case ACK_A:
                if (role == ROLE_A) {
                        int sender = message.sender_id;
                        pending_req[sender - MAX_ARTISTS] = FALSE;
                        for (int g = 0; g < MAX_ENGINEERS; g++){
                            if (g == sender - MAX_ARTISTS) {
                                priority[g] = 0;
                            } else {
                                priority[g] += 1;
                            }
                        }
                        paired = sender;
                    }
                break;
            case REQ_SLOT:
                // handle request 
                break;
            case RELEASE_SLOT:
                break;
            default:
                printf("[Rank %d] Unknown message type: %d\n", rank, message.type);
                break;
        }
    }
    return NULL;
}


int main(int argc, char **argv) {
    printf("poczatek\n");

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

    pthread_t comm_thread;
    pthread_create(&comm_thread, NULL, comm_thread_func, NULL);

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